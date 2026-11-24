/*
 * ducktales_sprites.c - the presented picture with every sprite the game
 * wants, in a fixed order, and none of the console's per-line or OAM limits
 * (cyc_render.h stock-width compositing; docs/rendering.md "Sprites without
 * limits"). The machine still draws its own picture with all of them, so
 * the hashes, sprite-0 hit and overflow stay the console's.
 *
 *   the list    The builder (bank 5 $8000) runs during the frame before the
 *               one that shows its table, so the RAM saved at that frame's
 *               end is the state the shown OAM was built from. The builder's
 *               rules are replayed on it slot by slot (the order of $95), and
 *               each slot's sprites must equal the next OAM entries, or the
 *               slot is left out (it was spawned after the builder ran).
 *               Entries after the last slot that are not parked are kept as
 *               the console shows them, behind everything (the builder does
 *               not always park what a shorter table left behind). While the
 *               HUD is in the OAM its entries are taken from there (a hit can
 *               change the records after the builder ran). An OAM identical
 *               to the last one rebuilt (the builder did not run: deaths,
 *               respawns) keeps that list. Until two frames were saved (power
 *               on, a loaded state) the OAM is drawn as it is (fallback).
 *   the order   OAM entry 0 (the HUD split), Scrooge (slot 0), the HUD
 *               groups, then slots 1 to 15: the builder's even-frame order,
 *               except that Scrooge goes before the HUD. The game draws the
 *               HUD only while he is below the band, so on the console they
 *               never meet; here he covers the lives counter as he covers
 *               the HUD frame, and the HUD stays in front of every enemy.
 *   the HUD     Drawn whenever the builder would draw it apart from Scrooge's
 *               Y: the game leaves it out while he is in the top band.
 *   drawing     Front to back over the exact background (cyc_render
 *               capture): the first opaque sprite pixel decides, and a
 *               behind-background one shows the background where it is
 *               opaque, as on the PPU. Pattern table, 8x16, mask and the
 *               left-column clip are each line's own; the HUD's sprites are
 *               fetched from $1000 as the top of the frame does when the HUD
 *               shows.
 *   mods        the enabled mods' tiles and palettes (mods/app_mods.h) are
 *               painted over the composed picture, drawn again natively from
 *               the same list; with the sprites off, or before a picture can
 *               be composed, over the machine's own picture
 */
#include "host/ducktales_sprites.h"

#include "game/ducktales_symbols.h"
#include "mods/app_mods.h"
#include "native/native_cycle.h"

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_ring.h"

#include <stdlib.h>
#include <string.h>

#define RAM_SIZE     0x800
#define SLOT_SPRITES 64
#define HUD_RECORDS  16
#define LIST_MAX     (64 + 3 * HUD_RECORDS + DT_OBJ_SLOTS * SLOT_SPRITES)

typedef struct {
    uint8_t y, tile, attr, x;
    bool hud;
} Sprite;

static bool s_on = DT_SPRITES_DEFAULT;
static bool s_installed;
static uint8_t s_ram[2][RAM_SIZE]; /* end of the previous frame, end of this one */
static int s_seen;                 /* frames saved since the machine was last replaced */
static uint8_t *s_prg;
static size_t s_prg_size;
static DtSpritesStats s_stats;
static FILE *s_log;
static uint8_t s_last_oam[256];
static bool s_last_valid;
static uint32_t s_background[256 * 240];
/* Scrooge apart (dt_sprites_set_scrooge_apart): his entries are list[1..s_scrooge_n]. */
static bool s_apart;
static int s_scrooge_n, s_scrooge_x, s_scrooge_y;
static const Sprite *s_list_ref;
static int s_list_n;
static uint16_t s_list_hud_table;
static uint64_t s_list_generation;

/* ---- the builder's rules on a saved RAM image ---- */

static bool load_prg(void)
{
    if (s_prg) {
        return true;
    }
    size_t size = cyc_mod_copy_cart_payload(NULL, 0);
    if (size < (DT_SPR_BANK + 1) * 0x4000u) {
        return false;
    }
    s_prg = malloc(size);
    if (!s_prg) {
        return false;
    }
    if (cyc_mod_copy_cart_payload(s_prg, size) != size) {
        free(s_prg);
        s_prg = NULL;
        return false;
    }
    s_prg_size = size;
    return true;
}

static uint8_t rd(uint16_t addr)
{
    size_t at = (size_t)DT_SPR_BANK * 0x4000u + (size_t)(addr - 0x8000u);
    return addr >= 0x8000 && addr < 0xC000 && at < s_prg_size ? s_prg[at] : 0;
}

static uint16_t rd16(uint16_t lo, uint16_t hi, uint8_t i)
{
    return (uint16_t)(rd((uint16_t)(lo + i)) | rd((uint16_t)(hi + i)) << 8);
}

static bool hud_allowed(const uint8_t *ram)
{
    return (ram[DT_ADDR_HUD_GATE] & 0x80) && !ram[DT_ADDR_MESSAGE] && !ram[DT_ADDR_ENDING] && !ram[DT_ADDR_PAUSE] &&
           ram[DT_ADDR_MODE] != DT_MODE_FROZEN;
}

static bool hud_in_oam(const uint8_t *ram)
{
    return hud_allowed(ram) && ram[DT_ROW_Y + DT_SLOT_SCROOGE] >= DT_HUD_MIN_SCROOGE_Y;
}

static int hud_records(const uint8_t *ram, Sprite *out)
{
    static const uint16_t GROUP[3] = { DT_ADDR_HUD_GROUP_HP, DT_ADDR_HUD_GROUP_LIVES, DT_ADDR_HUD_GROUP_TIME };
    int n = 0;
    for (int g = 0; g < 3; g++) {
        for (int i = 0; i < HUD_RECORDS && ram[GROUP[g] + 4 * i] != DT_HUD_RECORD_END_Y; i++) {
            const uint8_t *r = &ram[GROUP[g] + 4 * i];
            out[n++] = (Sprite){ r[0], r[1], r[2], r[3], true };
        }
    }
    return n;
}

/* One slot's sprites as bank 5 $8000 writes them, -1 when it draws nothing. */
static int slot_sprites(const uint8_t *ram, int n, Sprite *out)
{
    uint8_t status = ram[DT_ROW_STATUS + n], flags = ram[DT_ROW_FLAGS + n];
    if (!(status & 0x80) || flags == 0xFF) {
        return -1;
    }
    uint8_t x = ram[DT_ROW_POS_X + n];
    if (status & 0x20) {
        int borrow = ram[DT_ROW_POS_X + n] < ram[DT_ADDR_CAMERA_X];
        x = (uint8_t)(ram[DT_ROW_POS_X + n] - ram[DT_ADDR_CAMERA_X]);
        if ((uint8_t)(ram[DT_ROW_POS_PAGE + n] - ram[DT_ADDR_CAMERA_PAGE] - borrow)) {
            return -1;
        }
        if (status & 0x10) {
            return -1;
        }
    }
    if ((flags & 0x02) && !(ram[DT_ADDR_SPR_PHASE] & 0x02)) {
        return -1;
    }
    bool flip = (flags & 0x40) != 0;
    uint16_t script = rd16(DT_SPR_SCRIPT_LO, DT_SPR_SCRIPT_HI, ram[DT_ROW_ACTION + n]);
    uint8_t id = rd((uint16_t)(script + 2 + (ram[DT_ROW_FRAME + n] & 0x7F)));
    uint16_t meta = rd16(DT_SPR_META_LO, DT_SPR_META_HI, id);
    int count = rd(meta) + 1;
    uint8_t table = (uint8_t)(rd((uint16_t)(meta + 1)) + (flip ? 1 : 0));
    uint16_t offsets = (uint16_t)(rd16(DT_SPR_OFFSET_LO, DT_SPR_OFFSET_HI, table) - 2);
    int y0 = ram[DT_ROW_Y + n], made = 0;
    for (int k = 0; k < count && made < SLOT_SPRITES; k++) {
        int at = 2 + 2 * k;
        uint8_t dy = rd((uint16_t)(offsets + at)), dx = rd((uint16_t)(offsets + at + 1));
        int y = y0 + dy, xx = x + dx;
        if ((dy < 0x80 && y > 0xFF) || (dy >= 0x80 && y <= 0xFF)) {
            continue;
        }
        y &= 0xFF;
        if (n != DT_SLOT_SCROOGE && !(flags & 0x10) && y < DT_HUD_BAND_Y) {
            continue;
        }
        if ((dx < 0x80 && xx > 0xFF) || (dx >= 0x80 && xx <= 0xFF)) {
            continue;
        }
        uint8_t attr = (uint8_t)((rd((uint16_t)(meta + at + 1)) ^ (flip ? 0x40 : 0)) | ((flags & 0x04) << 3));
        out[made++] = (Sprite){ (uint8_t)y, rd((uint16_t)(meta + at)), attr, (uint8_t)xx, false };
    }
    return made;
}

static bool same(const Sprite *s, const uint8_t *e)
{
    return s->y == e[0] && s->tile == e[1] && s->attr == e[2] && s->x == e[3];
}

/* The list in drawing order (front first) from the RAM image the frame's OAM
 * was built from. */
static int rebuild(const uint8_t *ram, const uint8_t *oam, Sprite *list, bool *hud_restored, int *leftover)
{
    static Sprite slot[DT_OBJ_SLOTS][SLOT_SPRITES];
    int count[DT_OBJ_SLOTS], pos = 1;
    Sprite hud[3 * HUD_RECORDS];
    int hud_n = hud_allowed(ram) ? hud_records(ram, hud) : 0;
    if (hud_in_oam(ram)) {
        for (int i = 0; i < hud_n && pos < 64; i++, pos++) {
            hud[i] = (Sprite){ oam[pos * 4], oam[pos * 4 + 1], oam[pos * 4 + 2], oam[pos * 4 + 3], true };
        }
    }
    bool odd = ram[DT_ADDR_SPR_PHASE] & 1;
    for (int i = 0; i < DT_OBJ_SLOTS; i++) {
        int n = odd ? DT_OBJ_SLOTS - 1 - i : i;
        count[n] = slot_sprites(ram, n, slot[n]);
        if (count[n] <= 0 || pos >= 64) {
            continue;
        }
        int shown = count[n] < 64 - pos ? count[n] : 64 - pos, k = 0;
        while (k < shown && same(&slot[n][k], &oam[(pos + k) * 4])) {
            k++;
        }
        if (k < shown) {
            count[n] = -1;
        } else {
            pos += shown;
        }
    }
    int n = 0;
    list[n++] = (Sprite){ oam[0], oam[1], oam[2], oam[3], false };
    for (int k = 0; k < count[DT_SLOT_SCROOGE]; k++) {
        list[n++] = slot[DT_SLOT_SCROOGE][k];
    }
    s_scrooge_n = count[DT_SLOT_SCROOGE] > 0 ? count[DT_SLOT_SCROOGE] : 0;
    {
        const int z = DT_SLOT_SCROOGE;
        int x = ram[DT_ROW_POS_X + z];
        if (ram[DT_ROW_STATUS + z] & 0x20) {
            x = (int)(ram[DT_ROW_POS_PAGE + z] * 256 + ram[DT_ROW_POS_X + z]) -
                (int)(ram[DT_ADDR_CAMERA_PAGE] * 256 + ram[DT_ADDR_CAMERA_X]);
        }
        s_scrooge_x = x;
        s_scrooge_y = ram[DT_ROW_Y + z];
    }
    for (int i = 0; i < hud_n; i++) {
        list[n++] = hud[i];
    }
    *hud_restored = hud_n > 0 && !hud_in_oam(ram);
    for (int s = 0; s < DT_OBJ_SLOTS; s++) {
        if (s != DT_SLOT_SCROOGE) {
            for (int k = 0; k < count[s]; k++) {
                list[n++] = slot[s][k];
            }
        }
    }
    *leftover = 0;
    for (int e = pos; e < 64; e++) {
        if (oam[e * 4] >= 0xF0) {
            continue;
        }
        list[n++] = (Sprite){ oam[e * 4], oam[e * 4 + 1], oam[e * 4 + 2], oam[e * 4 + 3], false };
        ++*leftover;
    }
    return n;
}

/* ---- drawing ---- */

/* skip_from..skip_to (inclusive) are left out; layer: out starts
 * transparent (0) and only sprite pixels are written. */
static int draw_part(uint32_t *out, const Sprite *list, int n, uint16_t hud_table, int skip_from, int skip_to,
                     bool layer)
{
    int crowded = 0;
    const uint8_t *opaque = cyc_frame_bg_opaque();
    for (int y = 0; y < 240; y++) {
        uint32_t *line = &out[y * 256];
        if (layer) {
            memset(line, 0, 256 * sizeof *line);
        } else {
            memcpy(line, &s_background[y * 256], 256 * sizeof *line);
        }
        uint8_t mask = cyc_render_line_mask(y);
        if (!(mask & 0x10)) {
            continue;
        }
        bool tall = cyc_render_line_sprite16(y);
        int h = tall ? 16 : 8, on_line = 0;
        uint16_t table = cyc_render_line_sprite_table(y);
        uint32_t color[256];
        uint8_t taken[256] = { 0 }, behind[256];
        for (int i = 0; i < n; i++) {
            if (i >= skip_from && i <= skip_to) {
                continue;
            }
            const Sprite *s = &list[i];
            int r = y - (s->y + 1);
            if (s->y >= 0xEF || r < 0 || r >= h) {
                continue;
            }
            on_line++;
            int row = (s->attr & 0x80) ? h - 1 - r : r;
            uint16_t pattern = tall ? (uint16_t)(((s->tile & 1) ? 0x1000 : 0) + (s->tile & 0xFE) * 16 +
                                                 (row >= 8 ? 16 : 0))
                                    : (uint16_t)((s->hud ? hud_table : table) + s->tile * 16);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8; c++) {
                int px = s->x + c;
                if (px > 255) {
                    break;
                }
                if (taken[px] || (px < 8 && !(mask & 0x04))) {
                    continue;
                }
                int bit = (s->attr & 0x40) ? c : 7 - c;
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) {
                    continue;
                }
                taken[px] = 1;
                behind[px] = (s->attr & 0x20) != 0;
                color[px] = cyc_render_color(16 + (s->attr & 3) * 4 + v);
            }
        }
        crowded += on_line > 8;
        for (int x = 0; x < 256; x++) {
            if (taken[x] && !(behind[x] && opaque[y * 256 + x])) {
                line[x] = color[x];
            }
        }
    }
    return crowded;
}

static int draw(uint32_t *out, const Sprite *list, int n, uint16_t hud_table)
{
    return draw_part(out, list, n, hud_table, -1, -1, false);
}

/* The mods' tiles and palettes over the picture, drawn from list (NULL: the OAM). */
static void paint_mods(uint32_t *out, const Sprite *list, int n, uint16_t hud_table)
{
    static NvVideo video;
    static NvSprite sprites[LIST_MAX];
    if (!nv_cycle_fill(&video)) {
        return;
    }
    NvRenderOptions options = { 8, NULL, 0 };
    if (list) {
        for (int i = 0; i < n; i++) {
            sprites[i] = (NvSprite){ list[i].y, list[i].tile, list[i].attr, list[i].x,
                                     list[i].hud ? hud_table : NV_TABLE_LINE };
        }
        options = (NvRenderOptions){ 0, sprites, n };
    }
    app_mods_video_apply(&video, &options, s_ram[1], out);
    app_mods_draw_overlay(out, 256, 240);
}

static int compose(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user)
{
    (void)native_x0;
    (void)user;
    if (!s_on || width != 256 || height != 240 || !load_prg() || !cyc_render_background(s_background)) {
        s_stats.declined++;
        if (s_log) {
            fprintf(s_log, "frame=%lu declined\n", (unsigned long)(cyc_ring_frame - 1));
        }
        if (!app_mods_video_active() || width != 256 || height != 240 || !native) {
            return 0;
        }
        memcpy(out, native, 256 * 240 * sizeof *out);
        paint_mods(out, NULL, 0, 0);
        return 1;
    }
    static Sprite list[LIST_MAX];
    static int n, leftover;
    static bool hud_restored;
    static uint16_t hud_table;
    const uint8_t *oam = cyc_render_oam(), *ram = s_ram[0];
    const char *how = "reused";
    if (s_last_valid && !memcmp(oam, s_last_oam, sizeof s_last_oam)) {
        s_stats.reused++;
    } else if (s_seen >= 2) {
        n = rebuild(ram, oam, list, &hud_restored, &leftover);
        hud_table = ram[DT_ADDR_LAND] == DT_LAND_HUD_TABLE_OFF ? 0 : 0x1000;
        memcpy(s_last_oam, oam, sizeof s_last_oam);
        s_last_valid = true;
        s_stats.composed++;
        how = "rebuilt";
    } else {
        n = leftover = 0;
        hud_restored = false;
        hud_table = 0x1000;
        for (int e = 0; e < 64; e++) {
            list[n++] = (Sprite){ oam[e * 4], oam[e * 4 + 1], oam[e * 4 + 2], oam[e * 4 + 3], false };
        }
        s_stats.fallback++;
        how = "fallback";
    }
    if (hud_restored) {
        s_stats.hud_restored++;
    }
    if (leftover) {
        s_stats.leftover++;
    }
    int visible = 0;
    for (int i = 1; i < n; i++) {
        visible += list[i].y < 0xEF;
    }
    if (visible > 63) {
        s_stats.beyond_63 += (uint64_t)(visible - 63);
    }
    s_list_ref = list;
    s_list_n = n;
    s_list_hud_table = hud_table;
    s_list_generation = cyc_render_generation();
    if (strcmp(how, "fallback") == 0) {
        s_scrooge_n = 0;
    }
    int crowded = s_apart && s_scrooge_n > 0 ? draw_part(out, list, n, hud_table, 1, s_scrooge_n, false)
                                             : draw(out, list, n, hud_table);
    if (app_mods_video_active()) {
        paint_mods(out, list, n, hud_table);
    }
    s_stats.lines_over_8 += (uint64_t)crowded;
    if (s_log) {
        fprintf(s_log, "frame=%lu %s sprites=%d lines_over_8=%d hud_restored=%d leftover=%d\n",
                (unsigned long)(cyc_ring_frame - 1), how, visible, crowded, hud_restored ? 1 : 0, leftover);
    }
    return 1;
}

/* ---- Scrooge apart (smooth scrolling) ---- */

void dt_sprites_set_scrooge_apart(bool on)
{
    s_apart = on;
}

bool dt_sprites_scrooge_layer(uint32_t *out, int *x, int *y)
{
    int w, h;
    cyc_render_present(&w, &h);
    if (!s_apart || !s_on || !s_list_ref || s_scrooge_n <= 0 || s_list_generation != cyc_render_generation()) {
        return false;
    }
    const int last = s_scrooge_n;
    Sprite only[1 + SLOT_SPRITES];
    int n = 0;
    for (int i = 1; i <= last && i < s_list_n; i++) {
        only[n++] = s_list_ref[i];
    }
    draw_part(out, only, n, s_list_hud_table, -1, -1, true);
    *x = s_scrooge_x;
    *y = s_scrooge_y;
    return true;
}

/* ---- the setting and the host ---- */

static void apply(void)
{
    if (!s_installed) {
        cyc_render_set_compositor(compose, NULL);
        s_installed = true;
    }
    cyc_render_capture_background(s_on);
    cyc_render_set_stock_compositing(s_on || app_mods_video_active());
}

void dt_sprites_mods_changed(void)
{
    if (!s_installed) {
        apply();
    }
    cyc_render_set_stock_compositing(s_on || app_mods_video_active());
}

void dt_sprites_set(bool on)
{
    if (s_installed && on == s_on) {
        return;
    }
    s_on = on;
    apply();
}

bool dt_sprites_get(void)
{
    return s_on;
}

bool dt_sprites_parse(const char *text, bool *out)
{
    static const char *ON[] = { "1", "on", "true", "yes" }, *OFF[] = { "0", "off", "false", "no" };
    if (!text) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    char word[8];
    size_t n = 0;
    while (text[n] && text[n] != ' ' && text[n] != '\t' && text[n] != '\r' && text[n] != '\n') {
        if (n + 1 >= sizeof word) {
            return false;
        }
        word[n] = (char)(text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n]);
        n++;
    }
    word[n] = 0;
    for (const char *rest = text + n; *rest; rest++) {
        if (*rest != ' ' && *rest != '\t' && *rest != '\r' && *rest != '\n') {
            return false;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!strcmp(word, ON[i])) {
            *out = true;
            return true;
        }
        if (!strcmp(word, OFF[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

void dt_sprites_frame_end(void)
{
    if (!s_installed) {
        apply();
    }
    memcpy(s_ram[0], s_ram[1], RAM_SIZE);
    memcpy(s_ram[1], cyc_cpu_ram(), RAM_SIZE);
    if (s_seen < 2) {
        s_seen++;
    }
}

void dt_sprites_forget(void)
{
    if (!s_installed) {
        apply();
    }
    s_seen = 0;
    s_last_valid = false;
}

void dt_sprites_stats(DtSpritesStats *out)
{
    *out = s_stats;
}

void dt_sprites_set_log(FILE *f)
{
    s_log = f;
}
