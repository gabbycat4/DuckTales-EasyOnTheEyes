/*
 * glide_sim.c - the object glide in src/native/native_smooth.c offline on a
 * compact log (tools/measure/compact.py): every live object slot's position
 * ($0700 status, $0720/$0730 X, $0640 Y) each frame, shown at 4 pictures a
 * frame. Counts, for moving objects, speed jolts (a change of more than
 * 0.25 px/frame between pictures) without and with glide, and how far the
 * shown position trails the game's.
 *
 *   cc -O2 -Isrc tools/smooth-test/glide_sim.c src/native/native_smooth.c -lm -o /tmp/glide_sim
 *   /tmp/glide_sim logs/amazon.compact
 */
#include "native/native_present.h"
#include "native/native_smooth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC (4 + 32 + 256 + 2048)

bool native_present_playfield(int *band, int *x, int *y)
{
    *band = 47;
    *x = 0;
    *y = 0;
    return true;
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
    smooth_set_level(1);
    const long frames = size / REC;
    double last_raw[16][2], last_show[16][2], v_raw[16][2], v_show[16][2];
    int have[16] = { 0 };
    long jolts_raw = 0, jolts_show = 0, samples = 0;
    double lag_sum = 0, lag_max = 0;
    for (long i = 0; i < frames; i++) {
        const unsigned char *ram = data + i * REC + 292;
        smooth_frame(false, 0, 0);
        int slot[16], x[16], y[16], n = 0;
        bool world[16];
        for (int s = 1; s < 16; s++) {
            if (!(ram[0x700 + s] & 0x80)) {
                have[s] = 0;
                continue;
            }
            slot[n] = s;
            world[n] = (ram[0x700 + s] & 0x20) != 0;
            x[n] = world[n] ? ram[0x730 + s] * 256 + ram[0x720 + s] : ram[0x720 + s];
            y[n] = ram[0x640 + s];
            n++;
        }
        smooth_objects(slot, world, x, y, n);
        for (int k = 0; k < n; k++) {
            const int s = slot[k];
            for (int pic = 0; pic < 4; pic++) {
                double p = pic / 4.0, dx, dy;
                double raw[2] = { x[k], y[k] };
                if (!smooth_object_shift(s, p, true, &dx, &dy)) {
                    dx = dy = 0;
                }
                double show[2] = { x[k] + dx, y[k] + dy };
                if (have[s] >= 1) {
                    for (int a = 0; a < 2; a++) {
                        double vr = (raw[a] - last_raw[s][a]) * 4, vs = (show[a] - last_show[s][a]) * 4;
                        if (fabs(vr) > 20 || fabs(vs) > 20) {
                            have[s] = 0;
                            break;
                        }
                        if (have[s] >= 2) {
                            jolts_raw += fabs(vr - v_raw[s][a]) > 0.25;
                            jolts_show += fabs(vs - v_show[s][a]) > 0.25;
                        }
                        v_raw[s][a] = vr;
                        v_show[s][a] = vs;
                    }
                }
                last_raw[s][0] = raw[0];
                last_raw[s][1] = raw[1];
                last_show[s][0] = show[0];
                last_show[s][1] = show[1];
                have[s] = have[s] < 2 ? have[s] + 1 : 2;
                double lag = hypot(dx, dy);
                lag_sum += lag;
                lag_max = lag > lag_max ? lag : lag_max;
                samples++;
            }
        }
    }
    printf("objects: %ld picture samples; jolts without glide %.1f/s, with glide %.1f/s (per object-second); "
           "trailing mean %.2f max %.2f px\n",
           samples, jolts_raw / (samples / 240.0), jolts_show / (samples / 240.0), lag_sum / samples, lag_max);
    return 0;
}
