/*
 * smooth_sim.c - src/native/native_smooth.c offline on a compact log
 * (tools/measure/compact.py): the camera ($F3:$F2) and Scrooge's screen
 * position ($0650, $0640) of every frame, shown at 4 pictures a frame.
 * Reports, per level: speed jolts a second (> 0.25 px/frame between
 * pictures) of the scenery and of Scrooge's motion through the world, how
 * far the scenery trails, and how much Scrooge wobbles on screen while the
 * camera follows him (the blur the first version had).
 *
 *   cc -O2 -Isrc tools/smooth-test/smooth_sim.c src/native/native_smooth.c -lm -o /tmp/smooth_sim
 *   /tmp/smooth_sim logs/amazon.compact
 */
#include "native/native_present.h"
#include "native/native_smooth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC (4 + 32 + 256 + 2048)

static int s_on, s_x, s_y;

bool native_present_playfield(int *band, int *x, int *y)
{
    *band = 47;
    *x = s_x & 511;
    *y = s_y;
    return s_on;
}

void native_present_shifted_background(int band, int dx, int dy, uint32_t *out)
{
    (void)band, (void)dx, (void)dy, (void)out;
}

void native_present_refresh(void)
{
}

bool native_present_playfield_merged(int band, int *x, int *y)
{
    (void)band, (void)x, (void)y;
    return false;
}

typedef struct {
    double prev, prev_v;
    int have, have_v;
    long jolts;
} Track;

static void track(Track *t, double x)
{
    if (t->have) {
        double v = (x - t->prev) * 4;
        if (t->have_v && fabs(v - t->prev_v) > 0.25) {
            t->jolts++;
        }
        t->prev_v = v;
        t->have_v = 1;
    }
    t->prev = x;
    t->have = 1;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        return 1;
    }
    fclose(f);
    const long frames = size / REC;
    for (int level = 0; level <= 3; level++) {
        smooth_set_level(level);
        Track scenery = { 0 }, scrooge = { 0 };
        long shown = 0, follow = 0;
        double lag_sum = 0, lag_max = 0, wobble_sum = 0, wobble_max = 0;
        int game_x = 0, last = -1;
        for (long i = 0; i < frames; i++) {
            const unsigned char *ram = data + i * REC + 292;
            int cam = ram[0xF3] * 256 + ram[0xF2];
            s_on = ram[0xEA] && !ram[0xE9];
            int dx = last < 0 ? 0 : cam - last;
            dx = dx > 256 ? dx - 512 : dx < -256 ? dx + 512 : dx;
            last = cam;
            game_x += dx;
            s_x = game_x;
            s_y = 0;
            const int sx = ram[0x650], sy = ram[0x640];
            smooth_frame(s_on, sx, sy);
            if (!s_on || abs(dx) > 16) {
                memset(&scenery, 0, sizeof scenery);
                memset(&scrooge, 0, sizeof scrooge);
                continue;
            }
            shown++;
            for (int k = 0; k < 4; k++) {
                double cdx = 0, cdy = 0, pdx = 0, pdy = 0;
                if (level) {
                    smooth_shift(k / 4.0, 1, &cdx, &cdy);
                    smooth_scrooge_shift(k / 4.0, 1, &pdx, &pdy);
                } else if (k) {
                    cdx = 0;
                }
                double view = game_x - cdx;            /* the picture's camera */
                double screen = sx + pdx;              /* Scrooge on screen */
                track(&scenery, view);
                track(&scrooge, view + screen);
                lag_sum += fabs(cdx);
                lag_max = fabs(cdx) > lag_max ? fabs(cdx) : lag_max;
                if (dx && sx == 128) {
                    follow++;
                    wobble_sum += fabs(screen - 128);
                    wobble_max = fabs(screen - 128) > wobble_max ? fabs(screen - 128) : wobble_max;
                }
            }
        }
        printf("level %d: scenery jolts %.2f/s, Scrooge jolts %.2f/s, trailing mean %.1f max %.1f px; "
               "Scrooge off X 128 while followed: mean %.2f max %.2f px\n",
               level, scenery.jolts / (shown / 60.0), scrooge.jolts / (shown / 60.0), lag_sum / (shown * 4.0),
               lag_max, follow ? wobble_sum / follow : 0, wobble_max);
    }
    return 0;
}
