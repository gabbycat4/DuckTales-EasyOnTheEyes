/*
 * native_present.c - the cycle host's presentation interface (NESRecomp's
 * cyc_render.h, the frame records of cyc_core.h, and the cyc_mod.h reads and
 * stores hook callbacks use) served from the native runtime, so the port's
 * presentation code builds unchanged on it: ui/app_present.c (HUD split,
 * overscan, scaling, aspect), host/ducktales_sprites.c (No sprite flicker, a
 * stock-width compositor) and the enhancement hooks in src/game.
 *
 *   memories   CHR RAM, CIRAM (vertical mirroring), palette RAM and OAM as
 *              line 0 of the frame began (NativeFrame.start); DuckTales
 *              changes them only in VBlank or with rendering off
 *   lines      what the PPU used for each visible line, as the cycle host
 *              records it at dot 1 (cyc_frame_lines): the frame's register
 *              writes and latch resets, replayed in raster order with the
 *              PPU's scroll counters (coarse X every 8 dots, Y at dot 256,
 *              the horizontal copy at dot 257, the next line's two tiles at
 *              dots 328 and 336) while rendering is on
 *   picture    the native renderer (native_render.h) on NvVideo filled from
 *              those (nv_cycle_fill): the console's picture, its background
 *              coverage, and the background alone for a compositor
 *              (cyc_render_background)
 *   present    cyc_render_present: the picture, or the compositor's with
 *              stock-width compositing on; once per frame
 */
#include "native/native_present.h"

#include "native/native_cycle.h"
#include "native/native_render.h"
#include "native/native_runtime.h"

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_ring.h"

#include <string.h>

uint32_t cyc_ring_frame;

static NativeVideoMemory s_mem;
static CycLine s_lines[NV_HEIGHT];
static uint16_t s_index[NV_WIDTH * NV_HEIGHT];
static uint8_t s_bg[NV_WIDTH * NV_HEIGHT];
static uint32_t s_argb[NV_WIDTH * NV_HEIGHT], s_background[NV_WIDTH * NV_HEIGHT], s_canvas[NV_WIDTH * NV_HEIGHT];
static bool s_capture, s_stock, s_painted;
static CycCompositorFn s_fn;
static void *s_user;
static uint64_t s_generation = 1, s_composed;

/* ---- the lines ---- */

static uint16_t inc_x(uint16_t v)
{
    return (v & 31) == 31 ? (uint16_t)((v & ~31u) ^ 0x0400) : (uint16_t)(v + 1);
}

static uint16_t inc_y(uint16_t v)
{
    if ((v & 0x7000) != 0x7000) {
        return (uint16_t)(v + 0x1000);
    }
    v &= (uint16_t)~0x7000u;
    unsigned y = (v >> 5) & 31;
    if (y == 29) {
        y = 0;
        v ^= 0x0800;
    } else if (y == 31) {
        y = 0;
    } else {
        y++;
    }
    return (uint16_t)((v & ~0x03E0u) | (y << 5));
}

typedef struct {
    uint16_t v, t;
    uint8_t x, w, ctrl, mask;
} Scroll;

static bool rendering(const Scroll *s)
{
    return (s->mask & 0x18) != 0;
}

static void dot_event(Scroll *s, unsigned dot)
{
    if (!rendering(s)) {
        return;
    }
    if ((dot >= 8 && dot <= 256 && !(dot & 7)) || dot == 328 || dot == 336) {
        s->v = inc_x(s->v);
    }
    if (dot == 256) {
        s->v = inc_y(s->v);
    }
    if (dot == 257) {
        s->v = (uint16_t)((s->v & ~0x041Fu) | (s->t & 0x041Fu));
    }
}

static void reg_write(Scroll *s, uint16_t addr, uint8_t value)
{
    switch (addr) {
        case 0x2000:
            s->ctrl = value;
            s->t = (uint16_t)((s->t & 0x73FF) | ((value & 3) << 10));
            break;
        case 0x2001:
            s->mask = value;
            break;
        case 0x2002:
            s->w = 0;
            break;
        case 0x2005:
            if (!s->w) {
                s->x = value & 7;
                s->t = (uint16_t)((s->t & 0x7FE0) | (value >> 3));
            } else {
                s->t = (uint16_t)((s->t & 0x0C1F) | ((value & 0xF8) << 2) | ((value & 7) << 12));
            }
            s->w ^= 1;
            break;
        case 0x2006:
            if (!s->w) {
                s->t = (uint16_t)((s->t & 0x00FF) | ((value & 0x3F) << 8));
            } else {
                s->v = s->t = (uint16_t)((s->t & 0x7F00) | value);
            }
            s->w ^= 1;
            break;
        case 0x2007:
            if (rendering(s)) {
                s->v = inc_y(inc_x(s->v));
            } else {
                s->v = (uint16_t)((s->v + ((s->ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
            }
            break;
        default:
            break;
    }
}

static void replay_lines(const NativeFrame *f)
{
    Scroll s = { f->start_regs.v, f->start_regs.t,    f->start_regs.fine_x,
                 f->start_regs.w, f->start_regs.ctrl, f->start_regs.mask };
    if (rendering(&s)) {
        s.v = inc_x(inc_x(s.t));
    }
    uint32_t next = 0;
    for (unsigned y = 0; y < NV_HEIGHT; y++) {
        CycLine *l = &s_lines[y];
        l->v = s.v;
        l->fine_x = s.x;
        l->ctrl = (uint8_t)(s.ctrl & 0x38);
        l->mask = s.mask;
        l->pad = 0;
        unsigned dot = 2;
        for (; next < f->ppu_write_count && f->ppu_writes[next].scanline == y; next++) {
            const NativeRegWrite *w = &f->ppu_writes[next];
            for (; dot <= w->dot && dot <= 340; dot++) {
                dot_event(&s, dot);
            }
            reg_write(&s, w->addr, w->value);
        }
        for (; dot <= 340; dot++) {
            dot_event(&s, dot);
        }
    }
}

/* ---- the frame ---- */

void native_present_frame(const NativeFrame *f)
{
    s_mem = f->start;
    replay_lines(f);
    NvVideo video;
    nv_cycle_fill(&video);
    nv_render(&video, NULL, s_index, s_bg);
    nv_to_argb(s_index, s_argb, NV_WIDTH * NV_HEIGHT);
    if (s_capture) {
        static const NvSprite none[1];
        NvRenderOptions bg_only = { 0, none, 0 };
        nv_render(&video, &bg_only, s_index, NULL);
        nv_to_argb(s_index, s_background, NV_WIDTH * NV_HEIGHT);
    }
    cyc_ring_frame = f->frame + 1;
    s_generation++;
}

/* ---- smooth scrolling (native_present.h) ---- */

static NvVideo s_video;
static uint64_t s_video_generation;

static const NvVideo *video(void)
{
    if (s_video_generation != s_generation) {
        nv_cycle_fill(&s_video);
        s_video_generation = s_generation;
    }
    return &s_video;
}

void native_present_refresh(void)
{
    s_generation++;
}

bool native_present_playfield(int *band, int *x, int *y)
{
    const NvVideo *v = video();
    if (v->segment_count != 2) {
        return false;
    }
    const NvSegment *hud = &v->segment[0], *field = &v->segment[1];
    if (field->first_line < 24 || field->first_line > 80 || !(field->mask & NV_MASK_BG) ||
        !(hud->mask & NV_MASK_BG)) {
        return false;
    }
    *band = field->first_line;
    *x = field->x & 511;
    const int probe = field->first_line + 2;
    *y = ((cyc_render_line_scroll_y(probe) - probe) % 240 + 240) % 240;
    return true;
}

void native_present_shifted_background(int band, int dx, int dy, uint32_t *out)
{
    static NvVideo shifted;
    static uint16_t index[NV_WIDTH * NV_HEIGHT];
    static const NvSprite none[1];
    const NvRenderOptions bg_only = { 0, none, 0 };
    shifted = *video();
    for (int i = 0; i < shifted.segment_count; i++) {
        NvSegment *s = &shifted.segment[i];
        bool covers = i + 1 >= shifted.segment_count || shifted.segment[i + 1].first_line > band;
        if (s->first_line >= band || covers) {
            const int lines = ((dy % 480) + 480) % 480;
            s->x = (uint16_t)((s->x + dx) & 511);
            s->y = nv_advance_y(s->y, lines);
            if (s->split_column < NV_NO_SPLIT) {
                s->split_x = (uint16_t)((s->split_x + dx) & 511);
                s->split_y = nv_advance_y(s->split_y, lines);
            }
        }
    }
    if (band < 0) {
        band = 0;
    }
    nv_render_lines(&shifted, &bg_only, band, NV_HEIGHT - 1, index, NULL);
    nv_to_argb(index + band * NV_WIDTH, out + band * NV_WIDTH, (NV_HEIGHT - band) * NV_WIDTH);
}

/* ---- cyc_core.h, cyc_render.h ---- */

const CycLine *cyc_frame_lines(void)
{
    return s_lines;
}

const uint8_t *cyc_frame_bg_opaque(void)
{
    return s_bg;
}

const uint32_t *cyc_frame_argb(void)
{
    return s_argb;
}

const uint8_t *cyc_cpu_ram(void)
{
    return native_cpu_ram();
}

static const CycLine *line(int y)
{
    return &s_lines[y < 0 ? 0 : y > 239 ? 239 : y];
}

int cyc_render_line_scroll_x(int y)
{
    const CycLine *l = line(y);
    unsigned x6 = ((l->v >> 5) & 0x20u) | (l->v & 0x1Fu);
    return (int)((((x6 - 2u) & 63u) << 3) | l->fine_x);
}

int cyc_render_line_scroll_y(int y)
{
    const CycLine *l = line(y);
    return (int)(((l->v >> 11) & 1u) * 240u + ((l->v >> 5) & 31u) * 8u + ((l->v >> 12) & 7u));
}

uint16_t cyc_render_line_bg_table(int y)
{
    return (line(y)->ctrl & 0x10) ? 0x1000 : 0;
}

uint16_t cyc_render_line_sprite_table(int y)
{
    return (line(y)->ctrl & 0x08) ? 0x1000 : 0;
}

bool cyc_render_line_sprite16(int y)
{
    return (line(y)->ctrl & 0x20) != 0;
}

uint8_t cyc_render_line_mask(int y)
{
    return line(y)->mask;
}

const uint8_t *cyc_render_oam(void)
{
    return s_mem.oam;
}

uint8_t cyc_render_chr(uint16_t a)
{
    return s_mem.chr[a & 0x1FFF];
}

uint8_t cyc_render_nametable(uint16_t a)
{
    return s_mem.ciram[(a & 0x3FF) | (a & 0x400)];
}

uint8_t cyc_render_palette(int entry)
{
    return s_mem.palette[entry & 31] & 0x3F;
}

uint32_t cyc_render_color(int entry)
{
    return nv_palette_argb()[(s_mem.palette[entry & 31] & 0x3F) | (unsigned)(s_lines[239].mask >> 5) << 6];
}

uint32_t cyc_render_color_value(uint8_t value)
{
    return nv_palette_argb()[(value & 0x3F) | (unsigned)(s_lines[239].mask >> 5) << 6];
}

void cyc_render_set_compositor(CycCompositorFn fn, void *user)
{
    s_fn = fn;
    s_user = user;
    s_generation++;
}

bool cyc_render_has_compositor(void)
{
    return s_fn != NULL;
}

void cyc_render_set_stock_compositing(bool enabled)
{
    if (enabled != s_stock) {
        s_generation++;
    }
    s_stock = enabled;
}

bool cyc_render_stock_compositing(void)
{
    return s_stock;
}

int cyc_render_native_origin(int width)
{
    return width <= NV_WIDTH ? 0 : (width - NV_WIDTH) / 2;
}

void cyc_render_capture_background(bool enabled)
{
    s_capture = enabled;
}

bool cyc_render_background(uint32_t *out)
{
    if (!s_capture) {
        return false;
    }
    memcpy(out, s_background, sizeof s_background);
    return true;
}

void cyc_render_frame_done(void)
{
    s_generation++;
}

uint64_t cyc_render_generation(void)
{
    return s_generation;
}

const uint32_t *cyc_render_present(int *width, int *height)
{
    *width = NV_WIDTH;
    *height = NV_HEIGHT;
    if (!s_stock || !s_fn) {
        return s_argb;
    }
    if (s_composed != s_generation) {
        for (int i = 0; i < NV_WIDTH * NV_HEIGHT; i++) {
            s_canvas[i] = 0xFF000000u;
        }
        s_painted = s_fn(s_canvas, NV_WIDTH, NV_HEIGHT, 0, s_argb, s_user) != 0;
        s_composed = s_generation;
    }
    return s_painted ? s_canvas : s_argb;
}

/* ---- cyc_mod.h: what hook callbacks read and store ---- */

uint8_t cyc_mod_peek(uint16_t addr)
{
    uint8_t v = 0;
    native_peek(addr, &v);
    return v;
}

bool cyc_mod_peek_ok(uint16_t addr, uint8_t *value)
{
    return native_peek(addr, value);
}

bool cyc_mod_poke(uint16_t addr, uint8_t value)
{
    return native_poke(addr, value);
}

size_t cyc_mod_copy_cart_payload(uint8_t *out, size_t capacity)
{
    size_t size;
    const uint8_t *prg = native_prg(&size);
    if (out && prg && capacity >= size) {
        memcpy(out, prg, size);
    }
    return prg ? size : 0;
}
