/*
 * fm_mods.h - the game-agnostic mod core (src/mods/core, to become
 * formae-mod): the package format, the catalogue, the file formats and the
 * Lua script sandbox. Nothing here knows the game. The game describes itself
 * (FmGame, FmScriptGame) and applies what the assets mean: src/mods/nes for
 * NES tile keys and palettes, src/mods/ducktales for DuckTales.
 *
 *   package    a zip archive with the game's extension, or a folder, holding
 *              mod.toml (id, name, version, author, description, game and the
 *              target's SHA-256) and the game's asset folders; scripts/main.lua
 *              is the core's
 *   catalogue  the mods of the folders in the player's order, each enabled or
 *              not, with the reason it is refused; [Mods] Enabled and Order
 *   scripts    a Lua 5.4 state per mod, sandboxed, with the dt table the game
 *              describes (fm_script.c)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_MODS 256

/* Path separator and case-insensitive compares: Windows' own, or POSIX. */
#ifdef _WIN32
#define FM_SEP      "\\"
#define FM_SEP_CHAR '\\'
#else
#include <strings.h>
#define FM_SEP      "/"
#define FM_SEP_CHAR '/'
#define _stricmp    strcasecmp
#define _strnicmp   strncasecmp
#endif

/* ---- the game ---- */

typedef struct {
    const char *name;         /* mod.toml game = "..." must be this */
    const char *target_key;   /* the mod.toml key holding the target's SHA-256 ("prg_sha256") */
    const char *target_label; /* what the target is called in messages ("PRG") */
    const char *extension;    /* the archives' extension (".dtmod") */
    const char *log_prefix;   /* the log lines' prefix ("[ducktales]") */
} FmGame;

void fm_configure(const FmGame *game);
/* The running target's SHA-256 in lower-case hex; mods made for another are refused. */
void fm_set_target(const char *sha256_hex);
const char *fm_log_prefix(void);

/* ---- small helpers ---- */

bool fm_word_on(const char *v, bool *out);
/* 1-63 characters of a-z, 0-9, '-', '_' and '.', starting with a letter or digit. */
bool fm_valid_id(const char *id);
bool fm_ends_with(const char *s, const char *tail);
bool fm_parse_hex(const char *s, int digits, uint64_t *out);
void fm_base_name(const char *path, char *out, size_t cap);

#define FM_NAME_MAX 256

typedef struct {
    char name[FM_NAME_MAX]; /* '/' separated, relative to the mod's root */
    uint32_t offset;        /* archive: local header; folder: unused */
    uint32_t packed, size, crc;
    uint16_t method;
} FmEntry;

/* A mod's files: a zip read into memory, or a folder on disk. */
typedef struct {
    bool folder;
    char root[1024];          /* the folder, or the archive's path */
    char prefix[FM_NAME_MAX]; /* the archive's folder holding mod.toml ("" at its root) */
    uint8_t *data;
    size_t data_size;
    FmEntry *entries;
    int count;
} FmArchive;

/* ---- a mod's files (fm_archive.c) ---- */
bool fm_archive_open(FmArchive *a, const char *path, bool folder, char *err, size_t cap);
void fm_archive_close(FmArchive *a);
/* The entry's index by its name under the prefix, or -1. */
int fm_archive_find(const FmArchive *a, const char *name);
/* The name under the prefix ("tiles/x.png"), or NULL for files outside it. */
const char *fm_archive_name(const FmArchive *a, int index);
/* The file's bytes (malloc'd, NUL-terminated past size), or NULL with err. */
uint8_t *fm_archive_read(const FmArchive *a, int index, size_t *size, char *err, size_t cap);
/* UTF-8 path helpers (wide calls on Windows). */
uint8_t *fm_read_file(const char *path, size_t *size);
bool fm_is_dir(const char *path);
bool fm_make_dir(const char *path);
/* Calls fn for each entry of dir (not . or ..), with its name and whether it is a folder. */
void fm_list_dir(const char *dir, void (*fn)(void *user, const char *name, bool folder), void *user);
uint32_t fm_crc32(const uint8_t *data, size_t size);

/* ---- file formats (fm_codec.c) ---- */
/* PNG to straight-alpha 0xAARRGGBB, malloc'd; NULL when it is not a PNG. */
uint32_t *fm_png_decode(const uint8_t *data, size_t size, int *w, int *h);
bool fm_png_write(const char *path, const uint32_t *argb, int w, int h);
/* Raw deflate into exactly size bytes (malloc'd); NULL on a broken stream. */
uint8_t *fm_inflate(const uint8_t *data, size_t packed, size_t size);

/* A decoded sound being played: OGG streams from its file, WAV from its PCM. */
typedef struct FmSound FmSound;
/* Checks the file (OGG Vorbis or WAV PCM 8/16-bit, mono or stereo); NULL with err. */
FmSound *fm_sound_open(const uint8_t *file, size_t size, char *err, size_t cap);
void fm_sound_close(FmSound *s);
/* Back to the start. */
void fm_sound_rewind(FmSound *s);
/* Mono samples at the file's rate (-1..1) into out; fewer than count at the
 * end (with loop, from the loop start on). */
size_t fm_sound_read(FmSound *s, float *out, size_t count, bool loop);
int fm_sound_rate(const FmSound *s);
/* The file's own loop start in samples (OGG LOOPSTART, WAV smpl), or -1. */
long fm_sound_loop_start(const FmSound *s);

/* ---- the catalogue (fm_catalog.c) ---- */

typedef struct {
    char id[64]; /* mod.toml id; the file or folder name when it could not be read */
    char name[128];
    char version[32];
    char author[128];
    char description[512];
    char path[1024];        /* the archive or folder */
    bool folder;            /* a loose folder (development) */
    bool enabled;           /* the player's checkbox */
    bool loaded;            /* its assets are in use since the start */
    bool script;            /* ships scripts/main.lua */
    char error[320];        /* non-empty: refused or unreadable, never loaded */
    char script_error[320]; /* non-empty: its script stopped (error or limit) and why */
} FmMod;

/* Adds a folder to scan, created if missing when create is set. */
void fm_add_folder(const char *dir, bool create);
/* The first folder: the one the menu opens. Empty when none. */
const char *fm_folder(void);
/* Moves the folders added so far after dir (the player's own folder first). */
void fm_put_folder_first(const char *dir);
/* A [Mods] key; false when the key is unknown. */
bool fm_setting(const char *key, const char *value);
/* Reads the [Mods] section of an ini file. */
void fm_read_config(const char *path);
/* Writes the [Mods] section, header included. */
void fm_write_settings(FILE *f);
/* Mods on or off; off loads nothing at the next start. */
bool fm_enabled(void);
void fm_set_enabled(bool on);
/* The order and switches, "id:on, id:off, ..." top first. */
void fm_set_order(const char *text);
/* Reads the folders into the catalogue. */
void fm_scan(void);
/* What is enabled now loads: fm_restart_needed compares against it. */
void fm_mark_loaded_set(void);
int fm_count(void);
FmMod *fm_get(int index);
FmMod *fm_find(const char *id);
void fm_set_mod_enabled(int index, bool on);
void fm_move(int from, int to);
/* Reads the folders again; what is loaded does not change until a restart. */
void fm_rescan(void);
/* Enabled, the enabled mods or their order differ from what was loaded. */
bool fm_restart_needed(void);

/* ---- scripts (fm_script.c) ---- */

/* A game event scripts can handle with dt.on(name, fn). */
typedef struct {
    const char *name;
    bool cancelable;
} FmEvent;

/* A field of dt.state, and of dt.set when check and set are given. */
typedef struct {
    const char *name;
    bool boolean;                  /* read as a boolean, not an integer */
    bool (*get)(long long *value); /* false: nil now */
    /* dt.set.<name>(value): check validates it against the state now (false
     * with the reason); set stores it inside the frame's write window. */
    bool (*check)(long long value, char *err, size_t cap);
    void (*set)(long long value);
} FmField;

typedef struct {
    const FmEvent *events;
    int event_count;
    const FmField *fields;
    int field_count;
    bool (*state_open)(void);           /* the game's state can be read now */
    void (*listen)(int event, bool on); /* turns a game event's site on or off */
    void (*begin_frame)(void);          /* opens the frame-end window: "frame" handlers and dt.set writes */
    void (*end_frame)(void);
    const char *store_dir; /* the folder of dt.store's <id>.ini files; "" for none */
} FmScriptGame;

void fm_scripts_configure(const FmScriptGame *game);
/* Starts a loaded mod's scripts/main.lua; false when it failed (the mod's script_error says why). */
bool fm_script_add(const FmMod *mod);
int fm_script_count(void);
/* The game raised event: runs its handlers; true when one cancelled it. */
bool fm_scripts_event(int event);
/* After each machine frame: "frame" handlers, queued dt.set writes, toasts, hot reload, stores. */
void fm_scripts_frame(void);
/* The script notice to show now (dt.toast); false when none. */
bool fm_toast(char *title, size_t title_cap, char *body, size_t body_cap);
/* Writes the stores that changed (also done once a second and at exit). */
void fm_scripts_flush(void);

#ifdef __cplusplus
}
#endif
