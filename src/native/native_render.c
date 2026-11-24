/*
 * native_render.c - DuckTales' picture drawn from its video state
 * (native/native_render.h), with no PPU: each line is drawn from the segment
 * that covers it, the way the 2C02 composes it (nesdev wiki "PPU rendering",
 * "PPU sprite evaluation", "PPU sprite priority").
 *
 *   background   the nametable byte, attribute quadrant and pattern row at
 *                the line's position, fine X included (and the split's fine
 *                X from its column on the segment's last line); the left 8
 *                columns and the whole layer follow the segment's $2001 bits
 *   sprites      a line shows the sprites whose rows cover it, the first ones
 *                in list order up to the per-line limit, with the line's
 *                pattern table and size; at each column the first of them
 *                with an opaque pixel decides, and one marked behind the
 *                background shows the background where it is opaque (that
 *                sprite still hides the ones after it)
 *   blank lines  with background and sprites off a line is the backdrop
 *   output       palette RAM's entry, greyscale and emphasis as $2001 sets
 *                them, the same 9-bit index the PPU outputs
 *   source       optionally, for each pixel, the tile, the pixel in it and
 *                the palette it was drawn from (presentation that replaces
 *                tiles, src/mods)
 */
#include "native/native_render.h"

#include <math.h>
#include <string.h>

#define NV_LINE_SPRITES_MAX 2048

typedef struct {
    uint8_t lo, hi, attr;
    int x;
    uint16_t pattern;
} LineSprite;

typedef struct {
    const NvSegment *s;
    int split;
    unsigned nt_y[2], coarse_y[2], fine_y[2];
    uint16_t bg_table;
} Line;

void nv_render_options_console(NvRenderOptions *o)
{
    o->sprites_per_line = 8;
    o->sprites = NULL;
    o->sprite_count = 0;
}

uint16_t nv_advance_y(uint16_t y, int lines)
{
    unsigned nt = (y >> 8) & 1, coarse = (y >> 3) & 31, fine = y & 7;
    for (int i = 0; i < lines; i++) {
        if (++fine < 8) {
            continue;
        }
        fine = 0;
        if (coarse == 29) {
            coarse = 0;
            nt ^= 1;
        } else {
            coarse = (coarse + 1) & 31;
        }
    }
    return (uint16_t)(nt << 8 | coarse << 3 | fine);
}

static uint8_t nametable(const NvVideo *v, unsigned nt, unsigned offset)
{
    unsigned page;
    switch (v->mirroring) {
        case NV_MIRROR_HORIZONTAL:
            page = nt >> 1;
            break;
        case NV_MIRROR_SINGLE_0:
            page = 0;
            break;
        case NV_MIRROR_SINGLE_1:
            page = 1;
            break;
        default:
            page = nt & 1;
            break;
    }
    return v->ciram[page][offset & 0x3FF];
}

static void line_of(const NvVideo *v, int y, Line *l)
{
    int i = 0;
    while (i + 1 < v->segment_count && v->segment[i + 1].first_line <= y) {
        i++;
    }
    l->s = &v->segment[i];
    bool last = i + 1 < v->segment_count ? v->segment[i + 1].first_line == y + 1 : y == NV_HEIGHT - 1;
    l->split = last && l->s->split_column < NV_NO_SPLIT ? l->s->split_column : NV_NO_SPLIT;
    uint16_t pos[2] = { nv_advance_y(l->s->y, y - l->s->first_line), l->s->split_y };
    for (int k = 0; k < 2; k++) {
        l->nt_y[k] = (pos[k] >> 8) & 1;
        l->coarse_y[k] = (pos[k] >> 3) & 31;
        l->fine_y[k] = pos[k] & 7;
    }
    l->bg_table = (l->s->ctrl & NV_CTRL_BG_1000) ? 0x1000 : 0;
}

static bool rendering(const Line *l)
{
    return (l->s->mask & (NV_MASK_BG | NV_MASK_SPRITES)) != 0;
}

/* The background's 2-bit color at column x (0: transparent or not shown), its
 * palette and (source, may be NULL) the tile pixel it comes from. */
static unsigned bg_pixel(const NvVideo *v, const Line *l, int x, unsigned *pal, NvSource *source)
{
    const NvSegment *s = l->s;
    *pal = 0;
    if (source) {
        *source = NV_SOURCE_NONE;
    }
    if (!(s->mask & NV_MASK_BG) || (x < 8 && !(s->mask & NV_MASK_BG_LEFT))) {
        return 0;
    }
    int k = x >= l->split;
    unsigned bx = (unsigned)((k ? s->split_x : s->x) + x) & 511;
    unsigned coarse_x = (bx >> 3) & 31, coarse_y = l->coarse_y[k], nt = (bx >> 8) | l->nt_y[k] << 1;
    uint8_t tile = nametable(v, nt, coarse_y * 32 + coarse_x);
    uint16_t pattern = (uint16_t)(l->bg_table + tile * 16 + l->fine_y[k]);
    unsigned bit = 7 - (bx & 7);
    unsigned color = ((v->chr[pattern] >> bit) & 1) | (((v->chr[pattern + 8] >> bit) & 1) << 1);
    uint8_t attr = nametable(v, nt, 0x3C0 + (coarse_y >> 2) * 8 + (coarse_x >> 2));
    unsigned palette = (attr >> (((coarse_y & 2) << 1) | (coarse_x & 2))) & 3;
    if (source) {
        *source = NV_SOURCE_MAKE(pattern >> 4, bx & 7, l->fine_y[k], palette);
    }
    if (!color) {
        return 0;
    }
    *pal = palette;
    return color;
}

/* The row of sprite sp that line y shows, false when it shows none. */
static bool sprite_row(const NvVideo *v, const Line *l, const NvSprite *sp, int y, LineSprite *out)
{
    bool tall = (l->s->ctrl & NV_CTRL_SPRITE_8X16) != 0;
    int h = tall ? 16 : 8, row = y - 1 - sp->y;
    if (row < 0 || row >= h) {
        return false;
    }
    if (sp->attr & 0x80) {
        row = h - 1 - row;
    }
    uint16_t pattern;
    if (tall) {
        pattern = (uint16_t)(((sp->tile & 1) ? 0x1000 : 0) + (sp->tile & 0xFE) * 16 + (row >= 8 ? 16 : 0) + (row & 7));
    } else {
        uint16_t table = sp->table != NV_TABLE_LINE ? sp->table : (l->s->ctrl & NV_CTRL_SPRITE_1000) ? 0x1000 : 0;
        pattern = (uint16_t)(table + sp->tile * 16 + row);
    }
    *out = (LineSprite){ v->chr[pattern & 0x1FFF], v->chr[(pattern + 8) & 0x1FFF], sp->attr, sp->x,
                         (uint16_t)(pattern & 0x1FFF) };
    return true;
}

static NvSprite oam_sprite(const NvVideo *v, int i)
{
    const uint8_t *e = &v->oam[i * 4];
    return (NvSprite){ e[0], e[1], e[2], e[3], NV_TABLE_LINE };
}

static int line_sprites(const NvVideo *v, const NvRenderOptions *o, const Line *l, int y, LineSprite *out)
{
    int n = 0, count = o->sprites ? o->sprite_count : 64;
    for (int i = 0; i < count; i++) {
        if ((o->sprites_per_line && n >= o->sprites_per_line) || n >= NV_LINE_SPRITES_MAX) {
            break;
        }
        NvSprite sp = o->sprites ? o->sprites[i] : oam_sprite(v, i);
        n += sprite_row(v, l, &sp, y, &out[n]);
    }
    return n;
}

static unsigned sprite_pixel(const LineSprite *sp, int x)
{
    int c = x - sp->x;
    if (c < 0 || c > 7) {
        return 0;
    }
    int bit = (sp->attr & 0x40) ? c : 7 - c;
    return ((sp->lo >> bit) & 1) | (((sp->hi >> bit) & 1) << 1);
}

/* One line without source tracking, the same pixels as the per-pixel path
 * below: the background tile is fetched once per 8 columns, and the sprites
 * are drawn into a line buffer from the back of the list to the front, so the
 * first listed opaque sprite at a column is the one left (it decides, behind
 * the background or not, as on the PPU). */
static void draw_line_fast(const NvVideo *v, const Line *l, int n, const LineSprite *sprites, uint16_t *out,
                           uint8_t *opaque, uint16_t emphasis, uint8_t grey)
{
    const NvSegment *s = l->s;
    const uint8_t mask = s->mask;
    uint8_t addr[NV_WIDTH];
    if (mask & NV_MASK_BG) {
        int x = 0;
        if (!(mask & NV_MASK_BG_LEFT)) {
            memset(addr, 0, 8);
            x = 8;
        }
        unsigned key = ~0u, lo = 0, hi = 0, pal4 = 0;
        for (; x < NV_WIDTH; x++) {
            int k = x >= l->split;
            unsigned bx = (unsigned)((k ? s->split_x : s->x) + x) & 511;
            unsigned tile_key = (bx >> 3) | (unsigned)k << 7;
            if (tile_key != key) {
                key = tile_key;
                unsigned coarse_x = (bx >> 3) & 31, coarse_y = l->coarse_y[k], nt = (bx >> 8) | l->nt_y[k] << 1;
                uint8_t tile = nametable(v, nt, coarse_y * 32 + coarse_x);
                uint16_t pattern = (uint16_t)(l->bg_table + tile * 16 + l->fine_y[k]);
                lo = v->chr[pattern];
                hi = v->chr[pattern + 8];
                uint8_t attr = nametable(v, nt, 0x3C0 + (coarse_y >> 2) * 8 + (coarse_x >> 2));
                pal4 = ((attr >> (((coarse_y & 2) << 1) | (coarse_x & 2))) & 3) << 2;
            }
            unsigned bit = 7 - (bx & 7);
            unsigned color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            addr[x] = (uint8_t)(color ? pal4 | color : 0);
        }
    } else {
        memset(addr, 0, sizeof addr);
    }
    if (opaque) {
        for (int x = 0; x < NV_WIDTH; x++) {
            opaque[x] = addr[x] != 0;
        }
    }
    if (n) {
        uint8_t spr[NV_WIDTH + 8];
        memset(spr, 0, sizeof spr);
        for (int i = n - 1; i >= 0; i--) {
            const LineSprite *sp = &sprites[i];
            if (sp->x >= NV_WIDTH || sp->x <= -8) {
                continue;
            }
            const bool flip = (sp->attr & 0x40) != 0;
            const uint8_t base = (uint8_t)(0x10 | (sp->attr & 3) << 2 | (sp->attr & 0x20 ? 0x80 : 0));
            for (int c = 0; c < 8; c++) {
                int x = sp->x + c;
                if (x < 0 || x >= NV_WIDTH) {
                    continue;
                }
                int bit = flip ? c : 7 - c;
                unsigned sc = ((sp->lo >> bit) & 1) | (((sp->hi >> bit) & 1) << 1);
                if (sc) {
                    spr[x] = (uint8_t)(base | sc);
                }
            }
        }
        for (int x = (mask & NV_MASK_SPRITE_LEFT) ? 0 : 8; x < NV_WIDTH; x++) {
            uint8_t p = spr[x];
            if (p && (!addr[x] || !(p & 0x80))) {
                addr[x] = (uint8_t)(p & 0x1F);
            }
        }
    }
    for (int x = 0; x < NV_WIDTH; x++) {
        out[x] = (uint16_t)((v->palette[addr[x]] & grey) | emphasis);
    }
}

static void draw_lines(const NvVideo *v, const NvRenderOptions *o, int first, int last, uint16_t *index,
                       uint8_t *bg_opaque, NvSource *source)
{
    static LineSprite sprites[NV_LINE_SPRITES_MAX];
    NvRenderOptions console;
    if (!o) {
        nv_render_options_console(&console);
        o = &console;
    }
    for (int y = first < 0 ? 0 : first; y <= last && y < NV_HEIGHT; y++) {
        Line l;
        line_of(v, y, &l);
        uint16_t *out = &index[y * NV_WIDTH];
        uint8_t *opaque = bg_opaque ? &bg_opaque[y * NV_WIDTH] : NULL;
        NvSource *src = source ? &source[y * NV_WIDTH] : NULL;
        uint8_t mask = l.s->mask;
        uint16_t emphasis = (uint16_t)((mask >> 5) << 6);
        uint8_t grey = (mask & NV_MASK_GREYSCALE) ? 0x30 : 0x3F;
        if (opaque) {
            memset(opaque, 0, NV_WIDTH);
        }
        if (!rendering(&l)) {
            uint16_t backdrop = (uint16_t)((v->palette[0] & grey) | emphasis);
            for (int x = 0; x < NV_WIDTH; x++) {
                out[x] = backdrop;
            }
            if (src) {
                memset(src, 0, NV_WIDTH * sizeof *src);
            }
            continue;
        }
        int n = y > 0 && (mask & NV_MASK_SPRITES) ? line_sprites(v, o, &l, y, sprites) : 0;
        if (!src) {
            draw_line_fast(v, &l, n, sprites, out, opaque, emphasis, grey);
            continue;
        }
        for (int x = 0; x < NV_WIDTH; x++) {
            NvSource from;
            unsigned pal, color = bg_pixel(v, &l, x, &pal, src ? &from : NULL);
            if (opaque) {
                opaque[x] = color != 0;
            }
            unsigned addr = pal << 2 | color;
            if (n && (x >= 8 || (mask & NV_MASK_SPRITE_LEFT))) {
                for (int i = 0; i < n; i++) {
                    unsigned sc = sprite_pixel(&sprites[i], x);
                    if (!sc) {
                        continue;
                    }
                    if (!color || !(sprites[i].attr & 0x20)) {
                        addr = 0x10 | (sprites[i].attr & 3) << 2 | sc;
                        if (src) {
                            int c = x - sprites[i].x;
                            from = NV_SOURCE_MAKE(sprites[i].pattern >> 4, (sprites[i].attr & 0x40) ? 7 - c : c,
                                                  sprites[i].pattern & 7, 4 + (sprites[i].attr & 3));
                        }
                    }
                    break;
                }
            }
            if (src) {
                src[x] = from;
            }
            out[x] = (uint16_t)((v->palette[addr] & grey) | emphasis);
        }
    }
}

void nv_render_lines(const NvVideo *v, const NvRenderOptions *o, int first, int last, uint16_t *index,
                     uint8_t *bg_opaque)
{
    draw_lines(v, o, first, last, index, bg_opaque, NULL);
}

void nv_render(const NvVideo *v, const NvRenderOptions *o, uint16_t *index, uint8_t *bg_opaque)
{
    draw_lines(v, o, 0, NV_HEIGHT - 1, index, bg_opaque, NULL);
}

void nv_render_source(const NvVideo *v, const NvRenderOptions *o, uint16_t *index, NvSource *source)
{
    draw_lines(v, o, 0, NV_HEIGHT - 1, index, NULL, source);
}

bool nv_sprite0_hit(const NvVideo *v, int *hit_x, int *hit_y)
{
    NvSprite sprite0 = oam_sprite(v, 0);
    for (int y = 1; y < NV_HEIGHT; y++) {
        Line l;
        line_of(v, y, &l);
        uint8_t mask = l.s->mask;
        LineSprite s0;
        if ((mask & (NV_MASK_BG | NV_MASK_SPRITES)) != (NV_MASK_BG | NV_MASK_SPRITES)) {
            continue;
        }
        if (!sprite_row(v, &l, &sprite0, y, &s0)) {
            continue;
        }
        for (int x = 0; x < NV_WIDTH - 1; x++) {
            if (x < 8 && !(mask & NV_MASK_SPRITE_LEFT)) {
                continue;
            }
            unsigned pal;
            if (sprite_pixel(&s0, x) && bg_pixel(v, &l, x, &pal, NULL)) {
                *hit_x = x;
                *hit_y = y;
                return true;
            }
        }
    }
    return false;
}

/* ---- colors ---- */

/* One color cycle in 12 phases: a square wave between the two levels of the
 * color's luma row, in phase with the hue; emphasis attenuates the phases of
 * its color; the receiver averages luma and takes chroma at the burst phase
 * of hue 8, 30 degrees per hue (nesdev wiki "NTSC video"). */
static uint8_t channel(double c)
{
    return c <= 0 ? 0 : c >= 1 ? 255 : (uint8_t)(c * 255.0 + 0.5);
}

static bool phase_on(int hue, int phase)
{
    return (hue + phase) % 12 < 6;
}

const uint32_t *nv_palette_argb(void)
{
    static uint32_t table[512];
    static bool ready;
    if (ready) {
        return table;
    }
    static const double low_level[4] = { 0.350, 0.518, 0.962, 1.550 }, high_level[4] = { 1.094, 1.506, 1.962, 1.962 };
    const double black = 0.518, white = 1.962, attenuation = 0.746, pi = 3.14159265358979323846;
    for (int i = 0; i < 512; i++) {
        int hue = i & 15, row = (i >> 4) & 3, emphasis = i >> 6;
        if (hue > 13) {
            row = 1;
        }
        double lo = low_level[row], hi = high_level[row];
        if (hue == 0) {
            lo = hi;
        }
        if (hue > 12) {
            hi = lo;
        }
        double luma = 0, u = 0, w = 0;
        for (int p = 0; p < 12; p++) {
            double level = phase_on(hue, p) ? hi : lo;
            if (((emphasis & 1) && phase_on(0, p)) || ((emphasis & 2) && phase_on(4, p)) ||
                ((emphasis & 4) && phase_on(8, p))) {
                level *= attenuation;
            }
            level = (level - black) / (white - black);
            double angle = pi * (p - 0.5) / 6.0;
            luma += level;
            u += level * cos(angle);
            w -= level * sin(angle);
        }
        luma /= 12.0;
        u *= 2.0 / 12.0;
        w *= 2.0 / 12.0;
        table[i] = 0xFF000000u | (uint32_t)channel(luma + 1.140 * w) << 16 |
                   (uint32_t)channel(luma - 0.395 * u - 0.581 * w) << 8 | channel(luma + 2.032 * u);
    }
    ready = true;
    return table;
}

void nv_to_argb(const uint16_t *index, uint32_t *argb, int count)
{
    const uint32_t *table = nv_palette_argb();
    for (int i = 0; i < count; i++) {
        argb[i] = table[index[i] & 0x1FF];
    }
}
