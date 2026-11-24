/*
 * render_equal.c - the native renderer (src/native/native_render.c) against
 * a reference copy of an older version, on random frames: every pixel's
 * index and background coverage must match. Also times both.
 *
 * From the repository root (bash):
 *   git show HEAD~1:src/native/native_render.c > /tmp/old_render.c
 *   R="-Dnv_render=old_nv_render -Dnv_render_lines=old_nv_render_lines \
 *      -Dnv_render_source=old_nv_render_source -Dnv_render_options_console=old_nv_render_options_console \
 *      -Dnv_advance_y=old_nv_advance_y -Dnv_sprite0_hit=old_nv_sprite0_hit \
 *      -Dnv_palette_argb=old_nv_palette_argb -Dnv_to_argb=old_nv_to_argb"
 *   cc -O2 -Isrc $R -c /tmp/old_render.c -o /tmp/old.o
 *   cc -O2 -Isrc tools/render-test/render_equal.c src/native/native_render.c /tmp/old.o -lm -o /tmp/render_equal
 *   /tmp/render_equal 3000
 */
#include "native/native_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void old_nv_render(const NvVideo *, const NvRenderOptions *, uint16_t *, uint8_t *);

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void random_video(NvVideo *v)
{
    for (int i = 0; i < 0x2000; i++) v->chr[i] = (uint8_t)rnd();
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 0x400; i++) v->ciram[p][i] = (uint8_t)rnd();
    v->mirroring = (NvMirroring)(rnd() % 4);
    for (int i = 0; i < 32; i++) v->palette[i] = (uint8_t)(rnd() & 63);
    for (int i = 0; i < 256; i++) v->oam[i] = (uint8_t)rnd();
    int n = 1 + (int)(rnd() % 4);
    int line = 0;
    v->segment_count = 0;
    for (int i = 0; i < n && line < 240; i++) {
        NvSegment *s = &v->segment[v->segment_count++];
        s->first_line = (uint8_t)line;
        s->ctrl = (uint8_t)(rnd() & 0x38);
        s->mask = (uint8_t)rnd();
        if (rnd() % 3) s->mask |= NV_MASK_BG | NV_MASK_SPRITES;
        s->x = (uint16_t)(rnd() & 0x1FF);
        s->y = (uint16_t)(rnd() & 0x1FF);
        s->split_column = rnd() % 2 ? NV_NO_SPLIT : (uint16_t)(rnd() % 256);
        s->split_x = (uint16_t)(rnd() & 0x1FF);
        s->split_y = (uint16_t)(rnd() & 0x1FF);
        line += 1 + (int)(rnd() % 120);
    }
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 3000;
    static NvVideo v;
    static uint16_t a[NV_WIDTH * NV_HEIGHT], b[NV_WIDTH * NV_HEIGHT];
    static uint8_t oa[NV_WIDTH * NV_HEIGHT], ob[NV_WIDTH * NV_HEIGHT];
    static NvSprite list[300];
    long bad = 0, checked = 0;
    for (int f = 0; f < frames; f++) {
        random_video(&v);
        NvRenderOptions opts[3];
        nv_render_options_console(&opts[0]);
        int count = (int)(rnd() % 300);
        for (int i = 0; i < count; i++) {
            list[i] = (NvSprite){ (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(),
                                  rnd() % 3 ? NV_TABLE_LINE : (uint16_t)(rnd() % 2 ? 0x1000 : 0) };
        }
        opts[1] = (NvRenderOptions){ 0, list, count };
        opts[2] = (NvRenderOptions){ 0, list, 0 };
        for (int k = 0; k < 4; k++) {
            const NvRenderOptions *o = k < 3 ? &opts[k] : NULL;
            memset(oa, 7, sizeof oa);
            memset(ob, 7, sizeof ob);
            old_nv_render(&v, o, a, oa);
            nv_render(&v, o, b, ob);
            checked++;
            if (memcmp(a, b, sizeof a) || memcmp(oa, ob, sizeof oa)) {
                if (bad++ < 5) {
                    for (int i = 0; i < NV_WIDTH * NV_HEIGHT; i++)
                        if (a[i] != b[i] || oa[i] != ob[i]) {
                            printf("frame %d opts %d: first difference at x %d y %d: old %03x/%d new %03x/%d\n", f, k,
                                   i % 256, i / 256, a[i], oa[i], b[i], ob[i]);
                            break;
                        }
                }
            }
        }
    }
    printf("%ld renders compared, %ld differed\n", checked, bad);
    /* timing: a DuckTales-like frame (HUD + playfield, 2 segments, OAM). */
    random_video(&v);
    v.mirroring = NV_MIRROR_VERTICAL;
    v.segment_count = 2;
    v.segment[0] = (NvSegment){ 0, 0x10, 0x1E, 0, 0, NV_NO_SPLIT, 0, 0 };
    v.segment[1] = (NvSegment){ 47, 0x10, 0x1E, 0x45, 0x2F, NV_NO_SPLIT, 0, 0 };
    NvRenderOptions none = { 0, list, 0 };
    for (int pass = 0; pass < 2; pass++) {
        double t0 = now();
        for (int i = 0; i < 500; i++) {
            (pass ? nv_render : old_nv_render)(&v, NULL, a, oa);
            (pass ? nv_render : old_nv_render)(&v, &none, a, NULL);
        }
        printf("%s: %.3f ms per frame (picture + background)\n", pass ? "new" : "old", (now() - t0) * 1000 / 500);
    }
    return bad != 0;
}
