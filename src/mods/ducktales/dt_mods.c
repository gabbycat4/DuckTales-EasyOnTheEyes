/*
 * dt_mods.c - DuckTales' side of the mods (app_mods.h, docs/mods.md): what
 * the core (src/mods/core) and the NES module (src/mods/nes) are told about
 * this game, what its assets mean, and the dt table its scripts get.
 *
 *   game       "DuckTales (USA)", mod.toml prg_sha256 = the running PRG's,
 *              archives *.dtmod, the data folder's mods folder first
 *   tiles      tiles/<key>.png (fm_nes.h keys), painted over the presented
 *              picture: it is drawn again with nv_render_source to know each
 *              pixel's tile and palette, and a pixel is changed only where
 *              the presented picture shows the colour that drawing gives it
 *   palettes   palettes/global.pal or palettes/<land>.pal; a land's palette is
 *              used while $F6 is that land and the music is neither Land
 *              Select's nor the title's
 *   sounds     music/<id> and sfx/<id> (dt_mods_audio.c)
 *   loading    top first; a mod is used whole or not at all: any unreadable
 *              or invalid asset refuses it with the reason. Per asset the
 *              first mod that has it wins: a tile key, a land's palette (the
 *              mod's file for that land, else its global.pal), a sound id
 *   scripts    events = include/ducktales/events.h; dt.state and dt.set =
 *              FIELDS below, read and written through the game API
 *              (ducktales/hud.h, game.h, player.h, objects.h) inside a RAM
 *              transaction; dt.store in the data folder's mods-data; toasts
 *              drawn with cyc_overlay over the presented picture only
 */
#include "mods/app_mods.h"
#include "mods/ducktales/dt_mods_internal.h"
#include "mods/nes/fm_nes.h"

#include "ducktales/events.h"
#include "ducktales/game.h"
#include "ducktales/hud.h"
#include "ducktales/objects.h"
#include "ducktales/player.h"
#include "game/ducktales_symbols.h"
#include "ui/app_rom.h"

#include "cyc_overlay.h"

#include <stdlib.h>
#include <string.h>

#define LANDS 5

static bool s_configured, s_started;
static uint32_t *s_palettes[FM_MAX_MODS * (LANDS + 1)];
static int s_palette_count;
static const uint32_t *s_palette_for[LANDS + 1];
static const uint32_t *s_painted_palette;
static char s_store_dir[1024];

static const char *const LAND_NAMES[LANDS] = { "amazon", "transylvania", "african-mines", "himalayas", "moon" };

/* ---- the script API's game side ---- */

static bool get_land(long long *v)
{
    *v = gLand;
    return true;
}

static bool get_lives(long long *v)
{
    *v = gLives;
    return true;
}

static bool get_hp(long long *v)
{
    *v = Hud_GetHp(NULL);
    return true;
}

static bool get_hp_max(long long *v)
{
    uint8_t max = 0;
    Hud_GetHp(&max);
    *v = max;
    return true;
}

static bool get_money(long long *v)
{
    uint32_t money = 0;
    if (!Hud_GetMoney(&money)) {
        return false;
    }
    *v = money;
    return true;
}

static bool get_time(long long *v)
{
    *v = Hud_GetTime();
    return true;
}

static bool get_difficulty(long long *v)
{
    *v = gDifficulty;
    return true;
}

static bool get_paused(long long *v)
{
    *v = gPaused != 0;
    return true;
}

static bool get_boss_hits(long long *v)
{
    *v = gBossHits;
    return true;
}

static bool get_treasures(long long *v)
{
    *v = gTreasures;
    return true;
}

static bool get_key_items(long long *v)
{
    *v = gKeyItems;
    return true;
}

static bool get_pad(long long *v)
{
    *v = gPadHeld;
    return true;
}

static bool get_pad_new(long long *v)
{
    *v = gPadNew;
    return true;
}

static bool get_music(long long *v)
{
    *v = gMusic;
    return true;
}

static bool get_mode(long long *v)
{
    *v = gPlayerMode;
    return true;
}

static bool get_invulnerable(long long *v)
{
    *v = gPlayerInvulnerable;
    return true;
}

static bool get_x(long long *v)
{
    *v = Obj_GetWorldX(PLAYER_SLOT);
    return true;
}

static bool get_y(long long *v)
{
    *v = gObjects->y[PLAYER_SLOT];
    return true;
}

static bool in_range(long long v, long long lo, long long hi, char *err, size_t cap)
{
    if (v >= lo && v <= hi) {
        return true;
    }
    snprintf(err, cap, "out of range %lld to %lld", lo, hi);
    return false;
}

static bool check_lives(long long v, char *err, size_t cap)
{
    return in_range(v, 0, 9, err, cap);
}

static void set_lives(long long v)
{
    gLives = (uint8_t)v;
}

static bool check_hp(long long v, char *err, size_t cap)
{
    uint8_t max = 0;
    Hud_GetHp(&max);
    if (!max) {
        snprintf(err, cap, "the HUD shows no HP now");
        return false;
    }
    return in_range(v, 0, max, err, cap);
}

/* The first v dots of the HUD's HP records full, the rest lost. */
static void set_hp(long long v)
{
    uint8_t max = 0;
    Hud_GetHp(&max);
    for (int i = 0; i < max; i++) {
        if (i < v) {
            HUD_RECORD(i)[2] &= (uint8_t)~DT_HP_DOT_LOST;
        } else {
            HUD_RECORD(i)[2] |= DT_HP_DOT_LOST;
        }
    }
}

static bool check_money(long long v, char *err, size_t cap)
{
    uint32_t now;
    if (!Hud_GetMoney(&now)) {
        snprintf(err, cap, "the HUD shows no money now");
        return false;
    }
    if (v % 10) {
        snprintf(err, cap, "money is a multiple of 10");
        return false;
    }
    return in_range(v, 0, 9999990, err, cap);
}

/* Six digit tiles of v / 10, leading zeros blank as the game draws them. */
static void set_money(long long v)
{
    long long digits = v / 10;
    for (int i = DT_MONEY_DIGIT_COUNT - 1; i >= 0; i--) {
        gRam[DT_ADDR_MONEY_DIGITS + i] = (uint8_t)(digits % 10);
        digits /= 10;
    }
    for (int i = 0; i < DT_MONEY_DIGIT_COUNT - 1 && gRam[DT_ADDR_MONEY_DIGITS + i] == 0; i++) {
        gRam[DT_ADDR_MONEY_DIGITS + i] = DT_MONEY_TILE_BLANK;
    }
}

static bool check_time(long long v, char *err, size_t cap)
{
    return in_range(v, 0, 999, err, cap);
}

static void set_time(long long v)
{
    gRam[DT_ADDR_TIME_DIGITS] = (uint8_t)(v / 100);
    gRam[DT_ADDR_TIME_DIGITS + 4] = (uint8_t)(v / 10 % 10);
    gRam[DT_ADDR_TIME_DIGITS + 8] = (uint8_t)(v % 10);
}

static const FmField FIELDS[] = {
    { "land", false, get_land, NULL, NULL },
    { "lives", false, get_lives, check_lives, set_lives },
    { "hp", false, get_hp, check_hp, set_hp },
    { "hp_max", false, get_hp_max, NULL, NULL },
    { "money", false, get_money, check_money, set_money },
    { "time", false, get_time, check_time, set_time },
    { "difficulty", false, get_difficulty, NULL, NULL },
    { "paused", true, get_paused, NULL, NULL },
    { "boss_hits", false, get_boss_hits, NULL, NULL },
    { "treasures", false, get_treasures, NULL, NULL },
    { "key_items", false, get_key_items, NULL, NULL },
    { "pad", false, get_pad, NULL, NULL },
    { "pad_new", false, get_pad_new, NULL, NULL },
    { "music", false, get_music, NULL, NULL },
    { "mode", false, get_mode, NULL, NULL },
    { "invulnerable", false, get_invulnerable, NULL, NULL },
    { "x", false, get_x, NULL, NULL },
    { "y", false, get_y, NULL, NULL },
};

static EventResult on_event(EventId id, void *user)
{
    (void)user;
    return fm_scripts_event((int)id) ? EVENT_CANCEL : EVENT_CONTINUE;
}

static void listen(int event, bool on)
{
    if (on) {
        Events_On((EventId)event, on_event, NULL);
    } else {
        Events_Off((EventId)event, on_event, NULL);
    }
}

static void configure(void)
{
    if (s_configured) {
        return;
    }
    s_configured = true;
    static const FmGame GAME = { APP_MODS_GAME, "prg_sha256", "PRG", APP_MODS_EXTENSION, "[ducktales]" };
    fm_configure(&GAME);
    fm_set_target(APP_MODS_PRG_SHA256);
    static FmEvent events[EVENT_COUNT];
    for (int e = 0; e < EVENT_COUNT; e++) {
        events[e] = (FmEvent){ Events_Name((EventId)e), Events_Cancelable((EventId)e) };
    }
    if (app_data_dir(s_store_dir, sizeof s_store_dir - 16)) {
        strcat(s_store_dir, FM_SEP "mods-data");
    }
    FmScriptGame script = { events,       EVENT_COUNT, FIELDS,        (int)(sizeof FIELDS / sizeof FIELDS[0]),
                            GameRam_Open, listen,      GameRam_Begin, GameRam_End,
                            s_store_dir };
    fm_scripts_configure(&script);
}

/* ---- folders, settings and options ---- */

void app_mods_add_folder(const char *dir, bool create)
{
    configure();
    fm_add_folder(dir, create);
}

const char *app_mods_folder(void)
{
    return fm_folder();
}

bool app_mods_setting(const char *key, const char *value)
{
    configure();
    return fm_setting(key, value);
}

void app_mods_read_config(const char *path)
{
    configure();
    fm_read_config(path);
}

void app_mods_write_settings(FILE *f)
{
    fm_write_settings(f);
}

bool app_mods_enabled(void)
{
    return fm_enabled();
}

void app_mods_set_enabled(bool on)
{
    fm_set_enabled(on);
}

void app_mods_use_player_folders(const char *config_path)
{
    configure();
    char dir[1024];
    if (config_path) {
        fm_read_config(config_path);
    }
    if (app_data_dir(dir, sizeof dir - 8)) {
        strcat(dir, FM_SEP "mods");
        fm_put_folder_first(dir);
    }
}

static const char *const OPTION_NAME[APP_MODS_OPTION_COUNT] = { "--mods", "--mods-dir", "--mods-order", "--mods-dump" };
static const char *const OPTION_HELP[APP_MODS_OPTION_COUNT] = {
    "on|off: load the mods (default: config.ini [Mods] Enabled in the window, off headless)",
    "DIR: also read mods from DIR (docs/mods.md)",
    "ID:on|off,...: the mods' order and switches for this run, top first",
    "DIR: write every tile drawn, once, as DIR/<key>.png (templates for tile mods)",
};

const char *app_mods_option_name(int index)
{
    return OPTION_NAME[index];
}

const char *app_mods_option_help(int index)
{
    return OPTION_HELP[index];
}

bool app_mods_option(const char *name, const char *value, bool *ok)
{
    configure();
    if (!strcmp(name, "--mods")) {
        bool on = false;
        *ok = value && fm_word_on(value, &on);
        if (*ok) {
            fm_set_enabled(on);
        }
        return true;
    }
    if (!strcmp(name, "--mods-dir")) {
        *ok = value && *value;
        fm_add_folder(value, false);
        return true;
    }
    if (!strcmp(name, "--mods-order")) {
        *ok = value != NULL;
        if (*ok) {
            fm_set_order(value);
        }
        return true;
    }
    if (!strcmp(name, "--mods-dump")) {
        *ok = value && app_mods_dump_dir(value);
        return true;
    }
    return false;
}

/* ---- the catalogue ---- */

int app_mods_count(void)
{
    return fm_count();
}

const AppMod *app_mods_get(int index)
{
    return fm_get(index);
}

void app_mods_set_mod_enabled(int index, bool on)
{
    fm_set_mod_enabled(index, on);
}

void app_mods_move(int from, int to)
{
    fm_move(from, to);
}

void app_mods_rescan(void)
{
    fm_rescan();
}

bool app_mods_restart_needed(void)
{
    return fm_restart_needed();
}

void app_mods_scripts_frame(void)
{
    fm_scripts_frame();
}

/* ---- loading ---- */

static int land_of_file(const char *name)
{
    char base[FM_NAME_MAX];
    fm_base_name(name, base, sizeof base);
    size_t n = strlen(base);
    if (n < 4) {
        return -2;
    }
    base[n - 4] = 0;
    if (!_stricmp(base, "global")) {
        return LANDS;
    }
    for (int l = 0; l < LANDS; l++) {
        if (!_stricmp(base, LAND_NAMES[l])) {
            return l;
        }
    }
    return -2;
}

static bool sound_id(const char *name, const char *folder, int *id)
{
    char base[FM_NAME_MAX];
    if (strchr(name + strlen(folder), '/')) {
        return false;
    }
    fm_base_name(name, base, sizeof base);
    uint64_t v;
    if (strlen(base) < 6 || !fm_parse_hex(base, 2, &v) || (base[2] != '.' && base[2] != '-')) {
        return false;
    }
    *id = (int)v;
    return true;
}

typedef struct {
    bool music;
    int id;
    uint8_t *file;
    size_t size;
} PendingSound;

/* Reads and checks every asset of a mod, then uses it whole: false with the reason. */
static bool load_mod(AppMod *m, char *err, size_t cap)
{
    FmArchive a;
    if (!fm_archive_open(&a, m->path, m->folder, err, cap)) {
        return false;
    }
    FmNesTile *tiles = NULL;
    int tile_n = 0;
    uint32_t *pals[LANDS + 1] = { NULL };
    PendingSound *sounds = NULL;
    int sound_n = 0;
    bool ok = true;
    for (int i = 0; i < a.count && ok; i++) {
        const char *n = fm_archive_name(&a, i);
        if (!n) {
            continue;
        }
        bool tile = !_strnicmp(n, "tiles/", 6), pal = !_strnicmp(n, "palettes/", 9);
        bool music = !_strnicmp(n, "music/", 6), sfx = !_strnicmp(n, "sfx/", 4);
        if (!tile && !pal && !music && !sfx) {
            continue;
        }
        if (tile && !fm_ends_with(n, ".png")) {
            continue;
        }
        if (pal && !fm_ends_with(n, ".pal")) {
            continue;
        }
        if ((music || sfx) && !fm_ends_with(n, ".ogg") && !fm_ends_with(n, ".wav")) {
            continue;
        }
        uint64_t hash = 0;
        uint32_t palette = 0;
        int land = 0, id = 0;
        if (tile && !fm_nes_tile_key(n, &hash, &palette)) {
            snprintf(err, cap, "%s: a tile is named <16 hex digits>.png or <16 hex digits>_<8 hex digits>.png.", n);
            ok = false;
            break;
        }
        if (pal && (land = land_of_file(n)) < 0) {
            snprintf(err, cap,
                     "%s: palettes are global.pal or amazon, transylvania, african-mines, himalayas, moon .pal.", n);
            ok = false;
            break;
        }
        if ((music || sfx) && !sound_id(n, music ? "music/" : "sfx/", &id)) {
            snprintf(err, cap, "%s: a sound is named by its request id in 2 hex digits (07.ogg, 13-swing.wav).", n);
            ok = false;
            break;
        }
        size_t size;
        uint8_t *data = fm_archive_read(&a, i, &size, err, cap);
        if (!data) {
            ok = false;
            break;
        }
        if (tile) {
            int w, h;
            uint32_t *argb = fm_png_decode(data, size, &w, &h);
            free(data);
            if (!argb) {
                snprintf(err, cap, "%s: not a readable PNG.", n);
                ok = false;
            } else if (w != 8 || h != 8) {
                free(argb);
                snprintf(err, cap, "%s: %dx%d; replacement tiles must be 8x8 for now.", n, w, h);
                ok = false;
            } else {
                FmNesTile *grown = realloc(tiles, (size_t)(tile_n + 1) * sizeof *grown);
                if (!grown) {
                    free(argb);
                    ok = false;
                    snprintf(err, cap, "Out of memory.");
                } else {
                    tiles = grown;
                    tiles[tile_n++] = (FmNesTile){ hash, palette, w, h, argb };
                }
            }
        } else if (pal) {
            uint32_t *p = fm_nes_pal_read(data, size, nv_palette_argb());
            free(data);
            if (!p) {
                snprintf(err, cap, "%s: %zu bytes; a .pal holds 64 or 512 RGB triples (192 or 1536 bytes).", n, size);
                ok = false;
            } else {
                free(pals[land]);
                pals[land] = p;
            }
        } else {
            FmSound *probe = fm_sound_open(data, size, err, cap);
            if (!probe) {
                char why[200];
                snprintf(why, sizeof why, "%s", err);
                snprintf(err, cap, "%s: %s", n, why);
                free(data);
                ok = false;
            } else {
                fm_sound_close(probe);
                PendingSound *grown = realloc(sounds, (size_t)(sound_n + 1) * sizeof *grown);
                if (!grown) {
                    free(data);
                    ok = false;
                    snprintf(err, cap, "Out of memory.");
                } else {
                    sounds = grown;
                    sounds[sound_n++] = (PendingSound){ music, id, data, size };
                    if (music ? id >= DT_SOUND_MUSIC_IDS : (id < DT_SOUND_MUSIC_IDS || id >= DT_SOUND_IDS)) {
                        snprintf(err, cap, "%s: %s", n,
                                 music ? "music ids are 00-0F (docs/audio.md)."
                                       : "effect ids are 10-37 (docs/audio.md).");
                        ok = false;
                    }
                }
            }
        }
    }
    fm_archive_close(&a);
    if (ok) {
        for (int i = 0; i < tile_n; i++) {
            fm_nes_tile_insert(&tiles[i]);
        }
        for (int l = 0; l <= LANDS; l++) {
            if (pals[l]) {
                s_palettes[s_palette_count++] = pals[l];
            }
        }
        for (int l = 0; l < LANDS; l++) {
            if (!s_palette_for[l] && (pals[l] || pals[LANDS])) {
                s_palette_for[l] = pals[l] ? pals[l] : pals[LANDS];
            }
        }
        if (!s_palette_for[LANDS] && pals[LANDS]) {
            s_palette_for[LANDS] = pals[LANDS];
        }
        for (int i = 0; i < sound_n; i++) {
            char why[160];
            if (!mods_audio_has(sounds[i].music, sounds[i].id)) {
                mods_audio_add(sounds[i].music, sounds[i].id, sounds[i].file, sounds[i].size, why, sizeof why);
            } else {
                free(sounds[i].file);
            }
        }
    } else {
        for (int i = 0; i < tile_n; i++) {
            free(tiles[i].argb);
        }
        for (int l = 0; l <= LANDS; l++) {
            free(pals[l]);
        }
        for (int i = 0; i < sound_n; i++) {
            free(sounds[i].file);
        }
    }
    free(tiles);
    free(sounds);
    return ok;
}

void app_mods_start(const uint8_t *prg, size_t prg_size)
{
    configure();
    if (s_started) {
        return;
    }
    s_started = true;
    if (prg && prg_size) {
        uint8_t digest[32];
        char hex[65];
        app_sha256(prg, prg_size, digest);
        app_sha256_hex(digest, hex);
        fm_set_target(hex);
        mods_audio_read_driver(prg, prg_size);
    }
    fm_scan();
    int used = 0, scripts = 0;
    for (int i = 0; i < fm_count() && fm_enabled(); i++) {
        FmMod *m = fm_get(i);
        if (!m->enabled || m->error[0]) {
            continue;
        }
        char err[300];
        if (load_mod(m, err, sizeof err)) {
            m->loaded = true;
            used++;
            scripts += m->script;
        } else {
            snprintf(m->error, sizeof m->error, "Not loaded: %s", err);
        }
    }
    fm_mark_loaded_set();
    printf("[ducktales] Mods: %s, %d found, %d loaded (%zu tiles, %d palettes, sounds %s, %d scripts)\n",
           fm_enabled() ? "on" : "off", fm_count(), used, fm_nes_tile_count(), s_palette_count,
           app_mods_audio_active() ? "replaced" : "none", scripts);
    for (int i = 0; i < fm_count(); i++) {
        if (fm_get(i)->error[0]) {
            printf("[ducktales] Mod %s: %s\n", fm_get(i)->id, fm_get(i)->error);
        }
    }
    fflush(stdout);
    for (int i = 0; i < fm_count(); i++) {
        if (fm_get(i)->loaded && fm_get(i)->script) {
            fm_script_add(fm_get(i));
        }
    }
}

/* ---- the picture ---- */

static bool painting(void)
{
    return fm_nes_tile_count() || s_palette_count || fm_nes_dumping();
}

bool app_mods_video_active(void)
{
    return painting() || fm_script_count() > 0;
}

bool app_mods_dump_dir(const char *dir)
{
    return fm_nes_dump_to(dir);
}

uint32_t app_mods_color(uint16_t index, uint32_t argb)
{
    return s_painted_palette ? s_painted_palette[index & 0x1FF] : argb;
}

void app_mods_draw_overlay(uint32_t *argb, int width, int height)
{
    char title[128], body[128];
    if (argb && fm_toast(title, sizeof title, body, sizeof body)) {
        cyc_overlay_toast(argb, width, height, title, body);
    }
}

void app_mods_video_apply(const NvVideo *video, const NvRenderOptions *options, const uint8_t *ram, uint32_t *argb)
{
    if (!painting() || !video || !argb) {
        return;
    }
    static uint16_t index[NV_WIDTH * NV_HEIGHT];
    static NvSource source[NV_WIDTH * NV_HEIGHT];
    static const FmNesTile *cache[512 * 8];
    static uint8_t cached[512 * 8];
    uint64_t hashes[512];
    nv_render_source(video, options, index, source);
    for (int t = 0; t < 512; t++) {
        hashes[t] = fm_nes_tile_hash(&video->chr[t * 16]);
    }
    memset(cached, 0, sizeof cached);
    int land = LANDS;
    if (ram && ram[DT_ADDR_LAND] < LANDS && ram[DT_ADDR_LAST_MUSIC] != DT_MUSIC_LAND_SELECT &&
        ram[DT_ADDR_LAST_MUSIC] != DT_MUSIC_TITLE) {
        land = ram[DT_ADDR_LAND];
    }
    const uint32_t *pal = s_palette_for[land] ? s_palette_for[land] : s_palette_for[LANDS];
    s_painted_palette = pal;
    const uint32_t *base = nv_palette_argb();
    const bool tiles = fm_nes_tile_count() || fm_nes_dumping();
    for (int p = 0; p < NV_WIDTH * NV_HEIGHT; p++) {
        uint16_t i = index[p] & 0x1FF;
        if (argb[p] != base[i]) {
            continue;
        }
        uint32_t out = pal ? pal[i] : base[i];
        NvSource s = source[p];
        if (s != NV_SOURCE_NONE && tiles) {
            unsigned tile = NV_SOURCE_TILE(s), group = NV_SOURCE_PALETTE(s), slot = tile * 8 + group;
            if (!cached[slot]) {
                const uint8_t *e = &video->palette[group * 4];
                uint32_t palette = (uint32_t)(video->palette[0] & 0x3F) << 24 | (uint32_t)(e[1] & 0x3F) << 16 |
                                   (uint32_t)(e[2] & 0x3F) << 8 | (e[3] & 0x3F);
                const FmNesTile *art = fm_nes_tile_find(hashes[tile], palette);
                cache[slot] = art ? art : fm_nes_tile_find(hashes[tile], FM_NES_ANY_PALETTE);
                cached[slot] = 1;
                fm_nes_dump_tile(&video->chr[tile * 16], group >= 4, hashes[tile], palette, base);
            }
            const FmNesTile *art = cache[slot];
            if (art) {
                uint32_t c =
                    art->argb[(int)NV_SOURCE_ROW(s) * art->h / 8 * art->w + (int)NV_SOURCE_COLUMN(s) * art->w / 8];
                if ((c >> 24) >= 0x80) {
                    out = c | 0xFF000000u;
                }
            }
        }
        argb[p] = out;
    }
}
