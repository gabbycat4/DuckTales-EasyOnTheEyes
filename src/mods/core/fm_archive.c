/*
 * fm_archive.c - a mod's files (fm_mods.h): a .dtmod archive, a
 * zip read into memory whose entries are stored or deflated, or a loose
 * folder on disk. The archive is checked as it is read: its central
 * directory, every entry's local header and its CRC-32. Encrypted entries,
 * ZIP64 and other compression methods are refused with a reason. When
 * mod.toml is not at the archive's root but in exactly one folder (a zipped
 * mod folder), that folder is the mod's root. Paths are UTF-8; Windows
 * gets them through the wide calls.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "mods/core/fm_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

#define ZIP_EOCD     0x06054b50u
#define ZIP_CENTRAL  0x02014b50u
#define ZIP_LOCAL    0x04034b50u
#define MOD_FILE_MAX (512u << 20)

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t fm_crc32(const uint8_t *data, size_t size)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

#ifdef _WIN32
static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    }
    return w;
}

static FILE *open_utf8(const char *path, const char *mode)
{
    wchar_t *w = wide(path), wmode[8];
    if (!w) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    FILE *f = _wfopen(w, wmode);
    free(w);
    return f;
}

bool fm_is_dir(const char *path)
{
    wchar_t *w = wide(path);
    if (!w) {
        return false;
    }
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool fm_make_dir(const char *path)
{
    if (fm_is_dir(path)) {
        return true;
    }
    wchar_t *w = wide(path);
    if (!w) {
        return false;
    }
    bool ok = CreateDirectoryW(w, NULL) != 0;
    free(w);
    return ok;
}

void fm_list_dir(const char *dir, void (*fn)(void *user, const char *name, bool folder), void *user)
{
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    wchar_t *w = wide(pattern);
    if (!w) {
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char name[FM_NAME_MAX * 3];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) {
            continue;
        }
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        fn(user, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static FILE *open_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}

bool fm_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool fm_make_dir(const char *path)
{
    return fm_is_dir(path) || mkdir(path, 0755) == 0;
}

void fm_list_dir(const char *dir, void (*fn)(void *user, const char *name, bool folder), void *user)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        fn(user, e->d_name, fm_is_dir(path));
    }
    closedir(d);
}
#endif

uint8_t *fm_read_file(const char *path, size_t *size)
{
    FILE *f = open_utf8(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t *data = NULL;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        long long n = _ftelli64(f);
        if (n >= 0 && (unsigned long long)n <= MOD_FILE_MAX && _fseeki64(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t)n + 1);
            if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
                data[n] = 0;
                *size = (size_t)n;
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(f);
    return data;
}

static bool add_entry(FmArchive *a, const FmEntry *e)
{
    FmEntry *grown = realloc(a->entries, (size_t)(a->count + 1) * sizeof *grown);
    if (!grown) {
        return false;
    }
    a->entries = grown;
    a->entries[a->count++] = *e;
    return true;
}

typedef struct {
    FmArchive *a;
    char base[FM_NAME_MAX];
    int depth;
} Walk;

static void walk_folder(void *user, const char *name, bool folder);

static void walk(FmArchive *a, const char *base, int depth)
{
    char dir[1400];
    snprintf(dir, sizeof dir, "%s%s%s", a->root, base[0] ? FM_SEP : "", base);
    Walk w = { a, "", depth };
    snprintf(w.base, sizeof w.base, "%s", base);
    fm_list_dir(dir, walk_folder, &w);
}

static void walk_folder(void *user, const char *name, bool folder)
{
    Walk *w = user;
    char rel[FM_NAME_MAX];
    if (snprintf(rel, sizeof rel, "%s%s%s", w->base, w->base[0] ? "/" : "", name) >= (int)sizeof rel) {
        return;
    }
    if (folder) {
        if (w->depth < 4) {
            walk(w->a, rel, w->depth + 1);
        }
        return;
    }
    FmEntry e;
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", rel);
    add_entry(w->a, &e);
}

static bool fail(char *err, size_t cap, const char *what)
{
    snprintf(err, cap, "%s", what);
    return false;
}

static bool open_zip(FmArchive *a, char *err, size_t cap)
{
    const uint8_t *d = a->data;
    size_t n = a->data_size;
    if (n < 22) {
        return fail(err, cap, "Not a zip archive (too short).");
    }
    size_t eocd = (size_t)-1;
    size_t lowest = n > 22 + 0xFFFF ? n - 22 - 0xFFFF : 0;
    for (size_t at = n - 22 + 1; at-- > lowest;) {
        if (u32(d + at) == ZIP_EOCD && at + 22 + u16(d + at + 20) == n) {
            eocd = at;
            break;
        }
    }
    if (eocd == (size_t)-1) {
        return fail(err, cap, "Not a zip archive (no end of central directory).");
    }
    uint32_t entries = u16(d + eocd + 10), dir_size = u32(d + eocd + 12), dir_at = u32(d + eocd + 16);
    if (u16(d + eocd + 4) || u16(d + eocd + 6)) {
        return fail(err, cap, "Split zip archives are not supported.");
    }
    if (dir_at == 0xFFFFFFFFu || entries == 0xFFFF) {
        return fail(err, cap, "ZIP64 archives are not supported.");
    }
    if ((uint64_t)dir_at + dir_size > eocd) {
        return fail(err, cap, "Broken zip archive (central directory out of range).");
    }
    size_t at = dir_at;
    for (uint32_t i = 0; i < entries; i++) {
        if (at + 46 > eocd || u32(d + at) != ZIP_CENTRAL) {
            return fail(err, cap, "Broken zip archive (central directory).");
        }
        uint16_t flags = u16(d + at + 8), method = u16(d + at + 10), name_len = u16(d + at + 28);
        size_t next = at + 46 + name_len + u16(d + at + 30) + u16(d + at + 32);
        if (next > eocd) {
            return fail(err, cap, "Broken zip archive (central directory).");
        }
        FmEntry e;
        memset(&e, 0, sizeof e);
        e.crc = u32(d + at + 16);
        e.packed = u32(d + at + 20);
        e.size = u32(d + at + 24);
        e.offset = u32(d + at + 42);
        e.method = method;
        if (name_len >= FM_NAME_MAX) {
            return fail(err, cap, "Broken zip archive (a name is too long).");
        }
        memcpy(e.name, d + at + 46, name_len);
        for (char *c = e.name; *c; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }
        at = next;
        if (name_len && e.name[name_len - 1] == '/') {
            continue;
        }
        if (flags & 1) {
            snprintf(err, cap, "%s is encrypted; encrypted archives are not supported.", e.name);
            return false;
        }
        if (method != 0 && method != 8) {
            snprintf(err, cap, "%s uses compression method %u; only stored and deflate are supported.", e.name, method);
            return false;
        }
        if (e.packed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu) {
            return fail(err, cap, "ZIP64 archives are not supported.");
        }
        if (e.size > MOD_FILE_MAX) {
            snprintf(err, cap, "%s is larger than 512 MiB.", e.name);
            return false;
        }
        if (!add_entry(a, &e)) {
            return fail(err, cap, "Out of memory.");
        }
    }
    return true;
}

static void find_prefix(FmArchive *a)
{
    a->prefix[0] = 0;
    const char *found = NULL;
    for (int i = 0; i < a->count; i++) {
        const char *n = a->entries[i].name, *slash = strchr(n, '/');
        if (!strcmp(n, "mod.toml")) {
            return;
        }
        if (slash && !strchr(slash + 1, '/') && !strcmp(slash + 1, "mod.toml")) {
            if (found) {
                return;
            }
            found = n;
        }
    }
    if (found) {
        snprintf(a->prefix, sizeof a->prefix, "%.*s", (int)(strchr(found, '/') - found + 1), found);
    }
}

bool fm_archive_open(FmArchive *a, const char *path, bool folder, char *err, size_t cap)
{
    memset(a, 0, sizeof *a);
    a->folder = folder;
    snprintf(a->root, sizeof a->root, "%s", path);
    if (folder) {
        walk(a, "", 0);
        return true;
    }
    a->data = fm_read_file(path, &a->data_size);
    if (!a->data) {
        return fail(err, cap, "Cannot read the file.");
    }
    if (!open_zip(a, err, cap)) {
        fm_archive_close(a);
        return false;
    }
    find_prefix(a);
    return true;
}

void fm_archive_close(FmArchive *a)
{
    free(a->data);
    free(a->entries);
    memset(a, 0, sizeof *a);
}

const char *fm_archive_name(const FmArchive *a, int index)
{
    const char *n = a->entries[index].name;
    size_t k = strlen(a->prefix);
    return strncmp(n, a->prefix, k) ? NULL : n + k;
}

int fm_archive_find(const FmArchive *a, const char *name)
{
    for (int i = 0; i < a->count; i++) {
        const char *n = fm_archive_name(a, i);
        if (n && !strcmp(n, name)) {
            return i;
        }
    }
    return -1;
}

uint8_t *fm_archive_read(const FmArchive *a, int index, size_t *size, char *err, size_t cap)
{
    const FmEntry *e = &a->entries[index];
    if (a->folder) {
        char path[1400];
        snprintf(path, sizeof path, "%s" FM_SEP "%s", a->root, e->name);
        for (char *c = path + strlen(a->root); *c; c++) {
            if (*c == '/') {
                *c = FM_SEP_CHAR;
            }
        }
        uint8_t *data = fm_read_file(path, size);
        if (!data) {
            snprintf(err, cap, "%s: cannot read the file.", e->name);
        }
        return data;
    }
    const uint8_t *d = a->data;
    if ((uint64_t)e->offset + 30 > a->data_size || u32(d + e->offset) != ZIP_LOCAL) {
        snprintf(err, cap, "%s: broken local header.", e->name);
        return NULL;
    }
    uint64_t start = (uint64_t)e->offset + 30 + u16(d + e->offset + 26) + u16(d + e->offset + 28);
    if (start + e->packed > a->data_size) {
        snprintf(err, cap, "%s: data out of range (truncated archive?).", e->name);
        return NULL;
    }
    uint8_t *out;
    if (e->method == 0) {
        if (e->packed != e->size) {
            snprintf(err, cap, "%s: stored sizes disagree.", e->name);
            return NULL;
        }
        out = malloc((size_t)e->size + 1);
        if (out) {
            memcpy(out, d + start, e->size);
        }
    } else {
        out = fm_inflate(d + start, e->packed, e->size);
    }
    if (!out) {
        snprintf(err, cap, "%s: cannot be decompressed.", e->name);
        return NULL;
    }
    out[e->size] = 0;
    if (fm_crc32(out, e->size) != e->crc) {
        free(out);
        snprintf(err, cap, "%s: CRC-32 mismatch (damaged archive).", e->name);
        return NULL;
    }
    *size = e->size;
    return out;
}
