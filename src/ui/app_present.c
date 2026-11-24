/*
 * app_present.c - the window's presentation settings and the picture it shows
 * (app_present.h, docs/presentation.md).
 *
 * The picture comes from cyc_render_present, so a compositor the game
 * installed (cyc_render.h) is already in it. This file only edits a copy:
 *
 *   1. HUD split: the first line whose scroll, control or mask (as the PPU
 *      started it) differs from the line above, a few lines under a
 *      behind-background sprite 0, is the first playfield line. In DuckTales
 *      it shows the HUD's tile row wherever the camera covers nametable 0, so
 *      its HUD/playfield boundary crawls with the camera; it is drawn in the
 *      backdrop colour. The line above it, where the game's writes land
 *      (fine X changes before its end), is drawn again from the nametable
 *      with its own starting scroll. The opaque pixels of sprite 0 (the split
 *      sprite) over the backdrop are drawn in the backdrop colour too. A
 *      palette mod's colours apply to these pixels; its tiles do not.
 *   2. Overscan: the rows and columns cut off.
 *   3. Scaling: with nearest pixels and square pixels the cropped picture goes
 *      to the host as it is (its nearest and integer scaling apply). Otherwise
 *      it is resampled here to the drawable's size, so the host draws it 1:1:
 *      nearest, sharp (each output pixel the area average of the source pixels
 *      it covers: whole pixels stay solid, only the one output pixel on a
 *      boundary blends) or smooth (bilinear). Integer scaling keeps the
 *      vertical factor whole; 8:7 widens each pixel by 8/7.
 *
 * A picture is made once per machine frame, drawable size and setting.
 */
#include "app_present.h"

#include "cyc_core.h"
#include "cyc_render.h"
#include "mods/app_mods.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static AppPresentation s_p = { APP_PACING_AUTO, 1, APP_OVERSCAN_OFF, APP_PIXELS_NEAREST, APP_ASPECT_SQUARE, APP_RATE_60,
                                APP_SMOOTH_CALM };

static const char *const PACING[] = { "auto", "vsync", "steady" };
static const char *const SPLIT[] = { "original", "clean" };
static const char *const OVERSCAN[] = { "off", "top-bottom", "all" };
static const char *const PIXELS[] = { "nearest", "sharp", "smooth" };
static const char *const ASPECT[] = { "square", "8:7" };
static const char *const RATE[] = { "60", "120", "180", "240" };
static const char *const SMOOTH[] = { "off", "calm", "calmer", "slowest" };

static const struct {
    const char *key;
    const char *const *names;
    int count;
    size_t offset;
} KEYS[] = {
    { "FramePacing", PACING, APP_PACING_COUNT, offsetof(AppPresentation, pacing) },
    { "HudSplit", SPLIT, 2, offsetof(AppPresentation, clean_split) },
    { "Overscan", OVERSCAN, APP_OVERSCAN_COUNT, offsetof(AppPresentation, overscan) },
    { "Pixels", PIXELS, APP_PIXELS_COUNT, offsetof(AppPresentation, pixels) },
    { "PixelAspect", ASPECT, APP_ASPECT_COUNT, offsetof(AppPresentation, aspect) },
    { "PicturesPerSecond", RATE, APP_RATE_COUNT, offsetof(AppPresentation, rate) },
    { "SmoothScrolling", SMOOTH, APP_SMOOTH_COUNT, offsetof(AppPresentation, smooth) },
};

#define KEY_COUNT (sizeof KEYS / sizeof KEYS[0])

static int *field(AppPresentation *p, size_t k)
{
    return (int *)((char *)p + KEYS[k].offset);
}

void app_present_get(AppPresentation *out)
{
    *out = s_p;
}

void app_present_set(const AppPresentation *p)
{
    for (size_t k = 0; k < KEY_COUNT; k++) {
        int v = *field((AppPresentation *)p, k);
        *field(&s_p, k) = v < 0 ? 0 : v >= KEYS[k].count ? KEYS[k].count - 1 : v;
    }
}

bool app_present_load(const char *key, const char *value)
{
    for (size_t k = 0; k < KEY_COUNT; k++) {
        if (strcmp(key, KEYS[k].key)) {
            continue;
        }
        for (int v = 0; v < KEYS[k].count; v++) {
            if (!strcmp(value, KEYS[k].names[v])) {
                *field(&s_p, k) = v;
                return true;
            }
        }
        fprintf(stderr, "[ducktales] config.ini [Game] %s = %s: unknown value; using %s\n", key, value,
                KEYS[k].names[*field(&s_p, k)]);
        return true;
    }
    return false;
}

void app_present_write(FILE *f)
{
    for (size_t k = 0; k < KEY_COUNT; k++) {
        fprintf(f, "%s = %s\n", KEYS[k].key, KEYS[k].names[*field(&s_p, k)]);
    }
}

int app_present_rate_hz(int rate)
{
    return rate <= APP_RATE_60 ? 60 : rate >= APP_RATE_240 ? 240 : 60 * (rate + 1);
}

bool app_present_vsync_for(int pacing, int refresh_hz)
{
    if (pacing == APP_PACING_VSYNC) {
        return true;
    }
    if (pacing == APP_PACING_STEADY) {
        return false;
    }
    return refresh_hz <= 70;
}

/* ---- the picture ---- */

static struct {
    uint32_t *base, *tmp, *out;
    size_t base_cap, tmp_cap, out_cap;
    int *ix, *iy;
    unsigned *wx, *wy;
    int ax_cap, ay_cap;
    uint64_t generation;
    int out_w, out_h, integer;
    AppPresentation p;
    const uint32_t *pic;
    int w, h;
    bool exact, valid;
} s;

static bool grow(uint32_t **buf, size_t *cap, size_t n)
{
    if (n <= *cap) {
        return true;
    }
    uint32_t *b = (uint32_t *)realloc(*buf, n * sizeof **buf);
    if (!b) {
        return false;
    }
    *buf = b;
    *cap = n;
    return true;
}

static bool grow_axis(int **idx, unsigned **wt, int *cap, int n)
{
    if (n <= *cap) {
        return true;
    }
    int *i = (int *)realloc(*idx, (size_t)n * sizeof *i);
    if (i) {
        *idx = i;
    }
    unsigned *w = (unsigned *)realloc(*wt, (size_t)n * sizeof *w);
    if (w) {
        *wt = w;
    }
    if (!i || !w) {
        return false;
    }
    *cap = n;
    return true;
}

static void crop(int sw, int sh, int *x0, int *y0, int *cw, int *ch)
{
    int top = s_p.overscan == APP_OVERSCAN_OFF ? 0 : 8;
    int side = s_p.overscan == APP_OVERSCAN_ALL ? 8 : 0;
    if (sh <= 2 * top) {
        top = 0;
    }
    if (sw <= 2 * side) {
        side = 0;
    }
    *x0 = side;
    *y0 = top;
    *cw = sw - 2 * side;
    *ch = sh - 2 * top;
}

void app_present_base_size(int *w, int *h)
{
    int sw = 256, sh = 240, x0, y0;
    cyc_render_present(&sw, &sh);
    crop(sw, sh, &x0, &y0, w, h);
    if (s_p.aspect == APP_ASPECT_8_7) {
        *w = (int)lround(*w * 8.0 / 7.0);
    }
}

/* Palette entry on line y, through a palette mod when one is painting. */
static uint32_t color(int entry, int y)
{
    uint16_t index = (uint16_t)((cyc_render_palette(entry) & 0x3F) | (cyc_render_line_mask(y) >> 5) << 6);
    return app_mods_color(index, cyc_render_color(entry));
}

/* Background line y as the PPU starts it (its scroll, pattern table and left
 * column at dot 1), with no sprites: the HUD's own line, before the game's
 * split writes landed in it. */
static void hud_line(uint32_t *row, int y, uint32_t backdrop)
{
    const int sx = cyc_render_line_scroll_x(y), sy = cyc_render_line_scroll_y(y);
    const uint16_t table = cyc_render_line_bg_table(y);
    const bool left = (cyc_render_line_mask(y) & 0x02) != 0;
    const int ty = (sy % 240) / 8, fy = sy % 8;
    for (int x = 0; x < 256; x++) {
        int wx = (sx + x) & 511, tx = (wx & 255) / 8, bit = 7 - (wx & 7);
        uint16_t nt = (uint16_t)(0x2000 + ((wx >> 8) << 10) + (sy >= 240 ? 0x800 : 0));
        uint8_t tile = cyc_render_nametable((uint16_t)(nt + ty * 32 + tx));
        uint8_t attr = cyc_render_nametable((uint16_t)(nt + 0x3C0 + (ty / 4) * 8 + tx / 4));
        int pal = (attr >> (((ty & 2) << 1) | (tx & 2))) & 3;
        uint16_t p = (uint16_t)(table + tile * 16 + fy);
        int v = ((cyc_render_chr(p) >> bit) & 1) | (((cyc_render_chr((uint16_t)(p + 8)) >> bit) & 1) << 1);
        row[x] = v && (left || x >= 8) ? color(pal * 4 + v, y) : backdrop;
    }
}

static void clean_split(uint32_t *pic, int width, int height)
{
    const uint8_t *oam = cyc_render_oam();
    const CycLine *lines = cyc_frame_lines();
    if (height != 240 || !(oam[2] & 0x20) || oam[0] >= 239) {
        return;
    }
    const int top = oam[0] + 1, tall = cyc_render_line_sprite16(top) ? 16 : 8;
    int split = -1;
    for (int y = top + 1; y < 240 && y <= top + tall + 4; y++) {
        const CycLine *a = &lines[y - 1], *b = &lines[y];
        if ((a->v & 0x041F) != (b->v & 0x041F) || a->fine_x != b->fine_x || a->ctrl != b->ctrl || a->mask != b->mask) {
            split = y;
            break;
        }
    }
    if (split < 0) {
        return;
    }
    const uint32_t backdrop = color(0, split);
    const int ox = cyc_render_native_origin(width);
    const uint8_t *opaque = cyc_frame_bg_opaque();
    const uint8_t tile = oam[1], attr = oam[2];
    for (int y = top; y < top + tall && y < 240; y++) {
        int r = y - top;
        if (attr & 0x80) {
            r = tall - 1 - r;
        }
        uint16_t pattern = tall == 16
                               ? (uint16_t)(((tile & 1) << 12) + (tile & 0xFE) * 16 + (r >= 8 ? 16 : 0) + (r & 7))
                               : (uint16_t)(cyc_render_line_sprite_table(y) + tile * 16 + r);
        uint8_t lo = cyc_render_chr(pattern), hi = cyc_render_chr((uint16_t)(pattern + 8));
        for (int i = 0; i < 8; i++) {
            int bit = attr & 0x40 ? i : 7 - i, x = oam[3] + i;
            if (x > 255 || !(((lo >> bit) | (hi >> bit)) & 1) || opaque[y * 256 + x]) {
                continue;
            }
            if (ox + x >= 0 && ox + x < width) {
                pic[y * width + ox + x] = backdrop;
            }
        }
    }
    if (ox >= 0 && ox + 256 <= width) {
        hud_line(pic + (size_t)(split - 1) * width + ox, split - 1, backdrop);
    }
    for (int x = 0; x < width; x++) {
        pic[split * width + x] = backdrop;
    }
}

/* Source index and its weight (0-256; the rest goes to the next one) for each
 * of dst output pixels, in exact integer steps of src / dst. */
static void axis(int src, int dst, int mode, int *idx, unsigned *wt)
{
    for (int x = 0; x < dst; x++) {
        long long i;
        unsigned w = 256;
        if (mode == APP_PIXELS_NEAREST) {
            i = ((2LL * x + 1) * src) / (2LL * dst);
        } else if (mode == APP_PIXELS_SHARP && src <= dst) {
            long long a = (long long)x * src, edge;
            i = a / dst;
            edge = (i + 1) * dst;
            if (a + src > edge) {
                w = (unsigned)(((edge - a) * 256 + src / 2) / src);
            }
        } else {
            long long u = (2LL * x + 1) * src - dst, d = 2LL * dst;
            i = u >= 0 ? u / d : -1;
            if (i < 0) {
                i = 0;
            } else {
                w = 256 - (unsigned)(((u - i * d) * 256 + dst) / d);
            }
        }
        if (i > src - 1) {
            i = src - 1;
        }
        idx[x] = (int)i;
        wt[x] = i + 1 < src ? w : 256;
    }
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned w)
{
    if (w >= 256) {
        return a;
    }
    unsigned v = 256 - w;
    uint32_t rb = (((a & 0xFF00FFu) * w + (b & 0xFF00FFu) * v) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * w + (b & 0x00FF00u) * v) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

static const uint32_t *resample(const uint32_t *src, int sw, int sh, int dw, int dh)
{
    if (!grow(&s.tmp, &s.tmp_cap, (size_t)dw * sh) || !grow(&s.out, &s.out_cap, (size_t)dw * dh) ||
        !grow_axis(&s.ix, &s.wx, &s.ax_cap, dw) || !grow_axis(&s.iy, &s.wy, &s.ay_cap, dh)) {
        return NULL;
    }
    axis(sw, dw, s_p.pixels, s.ix, s.wx);
    axis(sh, dh, s_p.pixels, s.iy, s.wy);
    for (int y = 0; y < sh; y++) {
        const uint32_t *row = src + (size_t)y * sw;
        uint32_t *t = s.tmp + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            t[x] = mix(row[s.ix[x]], row[s.ix[x] + (s.wx[x] < 256)], s.wx[x]);
        }
    }
    for (int y = 0; y < dh; y++) {
        const uint32_t *a = s.tmp + (size_t)s.iy[y] * dw;
        uint32_t *o = s.out + (size_t)y * dw;
        if (s.wy[y] >= 256) {
            memcpy(o, a, (size_t)dw * sizeof *o);
            continue;
        }
        const uint32_t *b = a + dw;
        for (int x = 0; x < dw; x++) {
            o[x] = mix(a[x], b[x], s.wy[y]);
        }
    }
    return s.out;
}

const uint32_t *app_present_picture(int out_w, int out_h, bool integer_scale, int *w, int *h, bool *exact)
{
    const bool plain = !s_p.clean_split && s_p.overscan == APP_OVERSCAN_OFF && s_p.pixels == APP_PIXELS_NEAREST &&
                       s_p.aspect == APP_ASPECT_SQUARE;
    *exact = false;
    if (plain) {
        return NULL;
    }
    const uint64_t generation = cyc_render_generation();
    if (s.valid && s.generation == generation && s.out_w == out_w && s.out_h == out_h && s.integer == integer_scale &&
        !memcmp(&s.p, &s_p, sizeof s_p)) {
        *w = s.w;
        *h = s.h;
        *exact = s.exact;
        return s.pic;
    }
    int sw = 0, sh = 0;
    const uint32_t *src = cyc_render_present(&sw, &sh);
    if (!src || sw <= 0 || sh <= 0) {
        return NULL;
    }
    int x0, y0, cw, ch;
    crop(sw, sh, &x0, &y0, &cw, &ch);
    if (!grow(&s.base, &s.base_cap, (size_t)sw * sh)) {
        return NULL;
    }
    memcpy(s.base, src, (size_t)sw * sh * sizeof *src);
    if (s_p.clean_split) {
        clean_split(s.base, sw, sh);
    }
    for (int y = 0; y < ch; y++) {
        memmove(s.base + (size_t)y * cw, s.base + (size_t)(y + y0) * sw + x0, (size_t)cw * sizeof *s.base);
    }
    const uint32_t *pic = s.base;
    int pw = cw, ph = ch;
    bool at_drawable = false;
    if (s_p.pixels != APP_PIXELS_NEAREST || s_p.aspect != APP_ASPECT_SQUARE) {
        const double aw = s_p.aspect == APP_ASPECT_8_7 ? (double)lround(cw * 8.0 / 7.0) : cw;
        if (out_w > 0 && out_h > 0) {
            double f = out_w / aw < (double)out_h / ch ? out_w / aw : (double)out_h / ch;
            if (integer_scale && f >= 1.0) {
                f = floor(f);
            }
            pw = (int)lround(aw * f);
            ph = (int)lround(ch * f);
            if (pw > out_w) {
                pw = out_w;
            }
            if (ph > out_h) {
                ph = out_h;
            }
            at_drawable = true;
        } else {
            pw = (int)lround(aw);
        }
        if (pw < 1 || ph < 1 || !(pic = resample(s.base, cw, ch, pw, ph))) {
            return NULL;
        }
    }
    s.valid = true;
    s.generation = generation;
    s.out_w = out_w;
    s.out_h = out_h;
    s.integer = integer_scale;
    s.p = s_p;
    s.pic = pic;
    s.w = *w = pw;
    s.h = *h = ph;
    s.exact = *exact = at_drawable;
    return pic;
}
