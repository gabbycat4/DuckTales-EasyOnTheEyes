/*
 * fm_nes.c - the NES module of the mod core (fm_nes.h): tiles addressed by
 * their CHR content and palette, .pal palettes and the tile templates
 * --mods-dump writes.
 *
 *   key       FNV-1a 64 of the tile's 16 CHR bytes in 16 hex digits, and
 *             optionally _ and the palette it is drawn with in 8 (the NES
 *             colours of the backdrop and the three entries, a byte each)
 *   .pal      64 or 512 RGB triples (the emulators' format); 64 entries take
 *             the emphasis rows from the game's own table
 */
#include "mods/nes/fm_nes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FmNesTile *s_tiles;
static size_t s_tile_cap, s_tile_count;
static char s_dump_dir[1024];
static uint64_t *s_dumped;
static size_t s_dumped_cap, s_dumped_count;

size_t fm_nes_tile_count(void)
{
    return s_tile_count;
}

uint64_t fm_nes_tile_hash(const uint8_t *chr)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 16; i++) {
        h = (h ^ chr[i]) * 0x100000001b3ull;
    }
    return h;
}

static size_t tile_slot(uint64_t hash, uint32_t palette)
{
    uint64_t k = hash ^ ((uint64_t)palette * 0x9E3779B97F4A7C15ull);
    return (size_t)(k ^ (k >> 29)) & (s_tile_cap - 1);
}

const FmNesTile *fm_nes_tile_find(uint64_t hash, uint32_t palette)
{
    if (!s_tile_cap) {
        return NULL;
    }
    for (size_t i = tile_slot(hash, palette);; i = (i + 1) & (s_tile_cap - 1)) {
        FmNesTile *t = &s_tiles[i];
        if (!t->argb) {
            return NULL;
        }
        if (t->hash == hash && t->palette == palette) {
            return t;
        }
    }
}

static void tile_put(const FmNesTile *art)
{
    for (size_t i = tile_slot(art->hash, art->palette);; i = (i + 1) & (s_tile_cap - 1)) {
        if (!s_tiles[i].argb) {
            s_tiles[i] = *art;
            return;
        }
    }
}

/* Takes art->argb; false (and frees it) when the key is taken or memory is short. */
bool fm_nes_tile_insert(FmNesTile *art)
{
    if (fm_nes_tile_find(art->hash, art->palette)) {
        free(art->argb);
        return false;
    }
    if ((s_tile_count + 1) * 2 > s_tile_cap) {
        size_t old_cap = s_tile_cap, cap = old_cap ? old_cap * 2 : 1024;
        FmNesTile *old = s_tiles, *grown = calloc(cap, sizeof *grown);
        if (!grown) {
            free(art->argb);
            return false;
        }
        s_tiles = grown;
        s_tile_cap = cap;
        for (size_t i = 0; i < old_cap; i++) {
            if (old[i].argb) {
                tile_put(&old[i]);
            }
        }
        free(old);
    }
    tile_put(art);
    s_tile_count++;
    return true;
}

/* The base name of a tile file as its key: false when it is not one. */
bool fm_nes_tile_key(const char *name, uint64_t *hash, uint32_t *palette)
{
    char base[FM_NAME_MAX];
    fm_base_name(name, base, sizeof base);
    size_t n = strlen(base);
    if (n < 4 || _stricmp(base + n - 4, ".png")) {
        return false;
    }
    n -= 4;
    uint64_t pal;
    if (n == 16 && fm_parse_hex(base, 16, hash)) {
        *palette = FM_NES_ANY_PALETTE;
        return true;
    }
    if (n == 25 && base[16] == '_' && fm_parse_hex(base, 16, hash) && fm_parse_hex(base + 17, 8, &pal)) {
        if ((pal & 0xC0C0C0C0u) != 0) {
            return false;
        }
        *palette = (uint32_t)pal;
        return true;
    }
    return false;
}

uint32_t *fm_nes_pal_read(const uint8_t *d, size_t size, const uint32_t *base)
{
    if (size != 64 * 3 && size != 512 * 3) {
        return NULL;
    }
    uint32_t *pal = malloc(512 * sizeof *pal);
    if (!pal) {
        return NULL;
    }
    size_t entries = size / 3;
    for (size_t i = 0; i < 512; i++) {
        if (entries == 64 && i >= 64) {
            pal[i] = base[i];
            continue;
        }
        const uint8_t *p = d + i * 3;
        pal[i] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    return pal;
}

static bool dumped(uint64_t key)
{
    if ((s_dumped_count + 1) * 2 > s_dumped_cap) {
        size_t old_cap = s_dumped_cap, cap = old_cap ? old_cap * 2 : 4096;
        uint64_t *old = s_dumped, *grown = calloc(cap, sizeof *grown);
        if (!grown) {
            return true;
        }
        s_dumped = grown;
        s_dumped_cap = cap;
        for (size_t i = 0; i < old_cap; i++) {
            if (old[i]) {
                for (size_t k = old[i] & (cap - 1);; k = (k + 1) & (cap - 1)) {
                    if (!grown[k]) {
                        grown[k] = old[i];
                        break;
                    }
                }
            }
        }
        free(old);
    }
    key |= 1;
    for (size_t k = key & (s_dumped_cap - 1);; k = (k + 1) & (s_dumped_cap - 1)) {
        if (s_dumped[k] == key) {
            return true;
        }
        if (!s_dumped[k]) {
            s_dumped[k] = key;
            s_dumped_count++;
            return false;
        }
    }
}

bool fm_nes_dump_to(const char *dir)
{
    if (!fm_make_dir(dir)) {
        return false;
    }
    snprintf(s_dump_dir, sizeof s_dump_dir, "%s", dir);
    return true;
}

bool fm_nes_dumping(void)
{
    return s_dump_dir[0] != 0;
}

void fm_nes_dump_tile(const uint8_t *chr, bool sprite, uint64_t hash, uint32_t palette, const uint32_t *base)
{
    if (!s_dump_dir[0] || dumped(hash * 31 + palette * 0x9E3779B1u)) {
        return;
    }
    uint32_t px[64];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int c = ((chr[y] >> (7 - x)) & 1) | ((chr[y + 8] >> (7 - x)) & 1) << 1;
            px[y * 8 + x] = c == 0 && sprite ? 0 : base[(palette >> (8 * (3 - c))) & 0x3F];
        }
    }
    char path[1200];
    snprintf(path, sizeof path, "%s" FM_SEP "%016llx_%08x.png", s_dump_dir, (unsigned long long)hash, palette);
    fm_png_write(path, px, 8, 8);
}
