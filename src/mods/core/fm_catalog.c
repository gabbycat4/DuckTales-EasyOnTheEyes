/*
 * fm_catalog.c - the catalogue (fm_mods.h): the mods folders, mod.toml, the
 * player's order and switches, and what a restart would change.
 *
 *   settings   [Mods] Enabled = on|off (default off) and Order = id:on,
 *              id:off, ... top first; ids no longer present are kept at the
 *              end of Order so a mod taken out and put back keeps its place
 *   mod.toml   id, name, version, author, description, game (must be the
 *              game's name) and the target's SHA-256 (the game's key, which
 *              must be the running target's)
 *   order      new mods join the bottom of the list, switched on
 */
#include "mods/core/fm_mods.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FOLDERS 4

typedef struct {
    char id[64];
    bool on;
} OrderItem;

static FmGame s_game = { "", "target_sha256", "target", ".mod", "[mods]" };
static char s_target[65];
static char s_folders[MAX_FOLDERS][1024];
static int s_folder_count;
static bool s_enabled = false;
static OrderItem s_order[FM_MAX_MODS];
static int s_order_count;
static FmMod s_mods[FM_MAX_MODS];
static int s_count;
static bool s_marked, s_loaded_enabled;
static char s_loaded_set[FM_MAX_MODS * 66];

void fm_configure(const FmGame *game)
{
    s_game = *game;
}

void fm_set_target(const char *sha256_hex)
{
    snprintf(s_target, sizeof s_target, "%s", sha256_hex);
}

const char *fm_log_prefix(void)
{
    return s_game.log_prefix;
}

/* ---- small text helpers ---- */

bool fm_word_on(const char *v, bool *out)
{
    static const char *ON[] = { "on", "1", "true", "yes" }, *OFF[] = { "off", "0", "false", "no" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(v, ON[i])) {
            *out = true;
            return true;
        }
        if (!_stricmp(v, OFF[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    return s;
}

bool fm_valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n > 63 || !isalnum((unsigned char)id[0])) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '-' || id[i] == '_' ||
              id[i] == '.')) {
            return false;
        }
    }
    return true;
}

bool fm_ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !_stricmp(s + a - b, tail);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool fm_parse_hex(const char *s, int digits, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < digits; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return true;
}

static void lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/* ---- settings ---- */

void fm_set_order(const char *text)
{
    s_order_count = 0;
    char buf[FM_MAX_MODS * 72];
    snprintf(buf, sizeof buf, "%s", text);
    for (char *item = strtok(buf, ","); item && s_order_count < FM_MAX_MODS; item = strtok(NULL, ",")) {
        char *t = trim(item), *colon = strrchr(t, ':');
        bool on = true;
        if (colon) {
            *colon = 0;
            if (!fm_word_on(trim(colon + 1), &on)) {
                on = true;
            }
        }
        t = trim(t);
        if (!fm_valid_id(t)) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < s_order_count; i++) {
            dup |= !strcmp(s_order[i].id, t);
        }
        if (dup) {
            continue;
        }
        snprintf(s_order[s_order_count].id, sizeof s_order[0].id, "%s", t);
        s_order[s_order_count++].on = on;
    }
}

static void order_from_catalogue(void)
{
    OrderItem kept[FM_MAX_MODS];
    int n = 0;
    for (int i = 0; i < s_count && n < FM_MAX_MODS; i++) {
        if (!fm_valid_id(s_mods[i].id)) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < n; k++) {
            dup |= !strcmp(kept[k].id, s_mods[i].id);
        }
        if (dup) {
            continue;
        }
        snprintf(kept[n].id, sizeof kept[n].id, "%s", s_mods[i].id);
        kept[n++].on = s_mods[i].enabled;
    }
    for (int j = 0; j < s_order_count && n < FM_MAX_MODS; j++) {
        bool present = false;
        for (int k = 0; k < n; k++) {
            present |= !strcmp(kept[k].id, s_order[j].id);
        }
        if (!present) {
            kept[n++] = s_order[j];
        }
    }
    memcpy(s_order, kept, (size_t)n * sizeof kept[0]);
    s_order_count = n;
}

bool fm_setting(const char *key, const char *value)
{
    if (!_stricmp(key, "Enabled")) {
        bool on;
        if (fm_word_on(value, &on)) {
            s_enabled = on;
        } else {
            fprintf(stderr, "%s [Mods] Enabled = %s: expected on or off; using %s\n", s_game.log_prefix, value,
                    s_enabled ? "on" : "off");
        }
        return true;
    }
    if (!_stricmp(key, "Order")) {
        fm_set_order(value);
        return true;
    }
    return false;
}

void fm_read_config(const char *path)
{
    size_t size;
    char *text = (char *)fm_read_file(path, &size);
    if (!text) {
        return;
    }
    bool in = false;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = trim(line);
        if (*t == '[') {
            in = !_strnicmp(t, "[Mods]", 6);
            continue;
        }
        char *eq = strchr(t, '=');
        if (!in || !eq || *t == '#' || *t == ';') {
            continue;
        }
        *eq = 0;
        fm_setting(trim(t), trim(eq + 1));
    }
    free(text);
}

void fm_write_settings(FILE *f)
{
    fprintf(f, "[Mods]\nEnabled = %s\nOrder = ", s_enabled ? "on" : "off");
    for (int i = 0; i < s_order_count; i++) {
        fprintf(f, "%s%s:%s", i ? ", " : "", s_order[i].id, s_order[i].on ? "on" : "off");
    }
    fprintf(f, "\n");
}

bool fm_enabled(void)
{
    return s_enabled;
}

void fm_set_enabled(bool on)
{
    s_enabled = on;
}

void fm_add_folder(const char *dir, bool create)
{
    if (!dir || !*dir || s_folder_count >= MAX_FOLDERS) {
        return;
    }
    for (int i = 0; i < s_folder_count; i++) {
        if (!_stricmp(s_folders[i], dir)) {
            return;
        }
    }
    if (create) {
        fm_make_dir(dir);
    }
    snprintf(s_folders[s_folder_count++], sizeof s_folders[0], "%s", dir);
}

const char *fm_folder(void)
{
    return s_folder_count ? s_folders[0] : "";
}

void fm_put_folder_first(const char *dir)
{
    char moved[MAX_FOLDERS][1024];
    int n = s_folder_count;
    memcpy(moved, s_folders, sizeof moved);
    s_folder_count = 0;
    fm_add_folder(dir, true);
    for (int i = 0; i < n; i++) {
        fm_add_folder(moved[i], false);
    }
}

/* ---- mod.toml: top-level keys with string values (basic, literal and multi-line) ---- */

static bool toml_string(char **cursor, char *out, size_t cap, char *err, size_t errcap)
{
    char *p = *cursor;
    size_t n = 0;
    bool multi = !strncmp(p, "\"\"\"", 3) || !strncmp(p, "'''", 3), literal = *p == '\'';
    char quote = *p;
    p += multi ? 3 : 1;
    if (multi && *p == '\r') {
        p++;
    }
    if (multi && *p == '\n') {
        p++;
    }
    for (;;) {
        if (!*p || (!multi && (*p == '\n' || *p == '\r'))) {
            snprintf(err, errcap, "unterminated string");
            return false;
        }
        if (multi ? (p[0] == quote && p[1] == quote && p[2] == quote) : *p == quote) {
            break;
        }
        char c = *p++;
        if (c == '\\' && !literal) {
            char e = *p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '"' ? '"' : e == '\\' ? '\\' : 0;
            if (!c) {
                snprintf(err, errcap, "unsupported escape \\%c", e);
                return false;
            }
        }
        if (c == '\r') {
            continue;
        }
        if (n + 1 < cap) {
            out[n++] = c;
        }
    }
    out[n] = 0;
    *cursor = p + (multi ? 3 : 1);
    return true;
}

static bool parse_toml(char *text, FmMod *m, char *game, size_t game_cap, char *target, size_t target_cap, char *err,
                       size_t cap)
{
    char *p = text;
    bool top = true;
    int line = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\n') {
            p++;
            line++;
            continue;
        }
        if (*p == '#') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (*p == '[') {
            top = false;
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        char *key = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
            p++;
        }
        size_t klen = (size_t)(p - key);
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!klen || *p != '=') {
            snprintf(err, cap, "mod.toml line %d: expected key = value", line);
            return false;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char value[1024], why[64];
        if (*p == '"' || *p == '\'') {
            char *start = p;
            if (!toml_string(&p, value, sizeof value, why, sizeof why)) {
                snprintf(err, cap, "mod.toml line %d: %s", line, why);
                return false;
            }
            for (char *q = start; q < p; q++) {
                line += *q == '\n';
            }
        } else {
            char *v = p;
            while (*p && *p != '\n' && *p != '#') {
                p++;
            }
            snprintf(value, sizeof value, "%.*s", (int)(p - v), v);
            trim(value);
        }
        while (*p && *p != '\n') {
            if (*p == '#') {
                while (*p && *p != '\n') {
                    p++;
                }
                break;
            }
            if (*p != ' ' && *p != '\t' && *p != '\r') {
                snprintf(err, cap, "mod.toml line %d: unexpected text after the value", line);
                return false;
            }
            p++;
        }
        if (!top) {
            continue;
        }
#define KEY(name) (klen == sizeof name - 1 && !strncmp(key, name, klen))
        if (KEY("id")) {
            snprintf(m->id, sizeof m->id, "%s", value);
        } else if (KEY("name")) {
            snprintf(m->name, sizeof m->name, "%s", value);
        } else if (KEY("version")) {
            snprintf(m->version, sizeof m->version, "%s", value);
        } else if (KEY("author")) {
            snprintf(m->author, sizeof m->author, "%s", value);
        } else if (KEY("description")) {
            snprintf(m->description, sizeof m->description, "%s", value);
        } else if (KEY("game")) {
            snprintf(game, game_cap, "%s", value);
        } else if ((klen == strlen(s_game.target_key) && !strncmp(key, s_game.target_key, klen))) {
            snprintf(target, target_cap, "%s", value);
        }
#undef KEY
    }
    return true;
}

/* ---- the catalogue ---- */

void fm_base_name(const char *path, char *out, size_t cap)
{
    const char *b = path;
    for (const char *c = path; *c; c++) {
        if (*c == '\\' || *c == '/') {
            b = c + 1;
        }
    }
    snprintf(out, cap, "%s", b);
}

static void read_mod(FmMod *m)
{
    char err[256] = "";
    FmArchive a;
    fm_base_name(m->path, m->name, sizeof m->name);
    snprintf(m->id, sizeof m->id, "%s", m->name);
    lower(m->id);
    if (!fm_archive_open(&a, m->path, m->folder, err, sizeof err)) {
        snprintf(m->error, sizeof m->error, "Cannot be read: %s", err);
        return;
    }
    int at = fm_archive_find(&a, "mod.toml");
    size_t size;
    char *text = at < 0 ? NULL : (char *)fm_archive_read(&a, at, &size, err, sizeof err);
    char game[128] = "", target[80] = "";
    if (at < 0) {
        snprintf(m->error, sizeof m->error, "Cannot be read: no mod.toml in the archive.");
    } else if (!text) {
        snprintf(m->error, sizeof m->error, "Cannot be read: %s", err);
    } else {
        char name_from_file[128];
        snprintf(name_from_file, sizeof name_from_file, "%s", m->name);
        m->id[0] = m->name[0] = 0;
        if (!parse_toml(text, m, game, sizeof game, target, sizeof target, err, sizeof err)) {
            snprintf(m->error, sizeof m->error, "Cannot be read: %s", err);
        } else if (!fm_valid_id(m->id)) {
            snprintf(m->error, sizeof m->error,
                     "mod.toml: id \"%s\" must be 1-63 characters of a-z, 0-9, '-', '_' or '.'.", m->id);
        } else if (!m->name[0] || !m->version[0] || !game[0] || !target[0]) {
            snprintf(m->error, sizeof m->error, "mod.toml: name, version, game and %s are required.",
                     s_game.target_key);
        } else if (strcmp(game, s_game.name)) {
            snprintf(m->error, sizeof m->error, "Made for \"%s\", not %s.", game, s_game.name);
        } else {
            lower(target);
            if (s_target[0] && strcmp(target, s_target)) {
                snprintf(m->error, sizeof m->error,
                         "Made for a different version of the game: it targets %s SHA-256 %.12s..., this game's is "
                         "%.12s....",
                         s_game.target_label, target, s_target);
            }
        }
        if (!m->id[0]) {
            snprintf(m->id, sizeof m->id, "%s", name_from_file);
            lower(m->id);
        }
        if (!m->name[0]) {
            snprintf(m->name, sizeof m->name, "%s", name_from_file);
        }
    }
    free(text);
    for (int i = 0; i < a.count; i++) {
        const char *n = fm_archive_name(&a, i);
        if (!n) {
            continue;
        }
        m->script |= !_stricmp(n, "scripts/main.lua");
    }
    fm_archive_close(&a);
}

typedef struct {
    const char *dir;
} ScanCtx;

static void scan_entry(void *user, const char *name, bool folder)
{
    const ScanCtx *ctx = user;
    if (s_count >= FM_MAX_MODS) {
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s" FM_SEP "%s", ctx->dir, name);
    if (folder) {
        char toml[1100];
        snprintf(toml, sizeof toml, "%s" FM_SEP "mod.toml", path);
        size_t n;
        uint8_t *probe = fm_read_file(toml, &n);
        if (!probe) {
            return;
        }
        free(probe);
    } else if (!fm_ends_with(name, s_game.extension)) {
        return;
    }
    FmMod *m = &s_mods[s_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", path);
    m->folder = folder;
    read_mod(m);
}

static int by_name(const void *a, const void *b)
{
    return _stricmp(((const FmMod *)a)->name, ((const FmMod *)b)->name);
}

void fm_scan(void)
{
    s_count = 0;
    for (int f = 0; f < s_folder_count; f++) {
        ScanCtx ctx = { s_folders[f] };
        fm_list_dir(s_folders[f], scan_entry, &ctx);
    }
    qsort(s_mods, (size_t)s_count, sizeof s_mods[0], by_name);
    for (int i = 0; i < s_count; i++) {
        for (int k = 0; k < i; k++) {
            if (!s_mods[i].error[0] && !s_mods[k].error[0] && !strcmp(s_mods[i].id, s_mods[k].id)) {
                snprintf(s_mods[i].error, sizeof s_mods[i].error, "Another mod has the same id \"%s\": %s.",
                         s_mods[i].id, s_mods[k].path);
            }
        }
    }
    static FmMod sorted[FM_MAX_MODS];
    bool used[FM_MAX_MODS] = { false };
    int n = 0;
    for (int j = 0; j < s_order_count; j++) {
        for (int i = 0; i < s_count; i++) {
            if (!used[i] && !strcmp(s_mods[i].id, s_order[j].id)) {
                used[i] = true;
                sorted[n] = s_mods[i];
                sorted[n++].enabled = s_order[j].on;
                break;
            }
        }
    }
    for (int i = 0; i < s_count; i++) {
        if (!used[i]) {
            sorted[n] = s_mods[i];
            sorted[n++].enabled = true;
        }
    }
    memcpy(s_mods, sorted, (size_t)n * sizeof sorted[0]);
    order_from_catalogue();
}

static void loaded_set(char *out, size_t cap, bool *enabled)
{
    size_t n = 0;
    out[0] = 0;
    *enabled = s_enabled;
    if (!s_enabled) {
        return;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_mods[i].enabled && !s_mods[i].error[0]) {
            n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s,", s_mods[i].id);
        }
    }
}

int fm_count(void)
{
    return s_count;
}

FmMod *fm_get(int index)
{
    return index >= 0 && index < s_count ? &s_mods[index] : NULL;
}

void fm_set_mod_enabled(int index, bool on)
{
    if (index < 0 || index >= s_count) {
        return;
    }
    s_mods[index].enabled = on;
    order_from_catalogue();
}

void fm_move(int from, int to)
{
    if (from < 0 || from >= s_count || to < 0 || to >= s_count || from == to) {
        return;
    }
    FmMod m = s_mods[from];
    if (from < to) {
        memmove(&s_mods[from], &s_mods[from + 1], (size_t)(to - from) * sizeof m);
    } else {
        memmove(&s_mods[to + 1], &s_mods[to], (size_t)(from - to) * sizeof m);
    }
    s_mods[to] = m;
    order_from_catalogue();
}

void fm_rescan(void)
{
    static FmMod before[FM_MAX_MODS];
    int n = s_count;
    memcpy(before, s_mods, (size_t)n * sizeof before[0]);
    order_from_catalogue();
    fm_scan();
    for (int i = 0; i < s_count; i++) {
        for (int k = 0; k < n; k++) {
            if (!strcmp(before[k].id, s_mods[i].id)) {
                s_mods[i].loaded = before[k].loaded;
                memcpy(s_mods[i].script_error, before[k].script_error, sizeof s_mods[i].script_error);
            }
        }
    }
}

FmMod *fm_find(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_mods[i].id, id)) {
            return &s_mods[i];
        }
    }
    return NULL;
}

void fm_mark_loaded_set(void)
{
    loaded_set(s_loaded_set, sizeof s_loaded_set, &s_loaded_enabled);
    s_marked = true;
}

bool fm_restart_needed(void)
{
    static char now[sizeof s_loaded_set];
    bool enabled;
    loaded_set(now, sizeof now, &enabled);
    return s_marked && (enabled != s_loaded_enabled || strcmp(now, s_loaded_set));
}
