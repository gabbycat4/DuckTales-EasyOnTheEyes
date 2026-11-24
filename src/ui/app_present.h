/*
 * app_present.h - how the window shows the machine's picture (window builds
 * only, DUCKTALES_WINDOW). docs/presentation.md has the measurements behind
 * each choice.
 *
 *   frame pacing   Auto, VSync or Steady: resolved at launch into the host's
 *                  [Display] VSync; with VSync off the window holds each
 *                  picture to an even period (NESRecomp's steady hold)
 *   HUD split      the line on which the game switches from the HUD to the
 *                  playfield, and the split sprite, drawn as the backdrop
 *   overscan       8 lines at the top and bottom, or 8 pixels on every edge
 *   pixels         nearest, sharp (area) or smooth (bilinear) scaling
 *   pixel aspect   square, or 8:7 as an NTSC television shows it
 *
 * Everything here works on the picture CycHostExtras.present hands the
 * window, after cyc_render_present (and any compositor the game installed),
 * never on the machine, its picture, its hashes or its frame logs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_PACING_AUTO,
    APP_PACING_VSYNC,
    APP_PACING_STEADY,
    APP_PACING_COUNT
} AppPacing;

typedef enum {
    APP_PIXELS_NEAREST,
    APP_PIXELS_SHARP,
    APP_PIXELS_SMOOTH,
    APP_PIXELS_COUNT
} AppPixels;

typedef enum {
    APP_ASPECT_SQUARE,
    APP_ASPECT_8_7,
    APP_ASPECT_COUNT
} AppAspect;

typedef enum {
    APP_OVERSCAN_OFF,
    APP_OVERSCAN_TOP_BOTTOM,
    APP_OVERSCAN_ALL,
    APP_OVERSCAN_COUNT
} AppOverscan;

typedef enum {
    APP_RATE_60,
    APP_RATE_120,
    APP_RATE_180,
    APP_RATE_240,
    APP_RATE_COUNT
} AppRate;

typedef enum {
    APP_SMOOTH_OFF,
    APP_SMOOTH_CALM,
    APP_SMOOTH_CALMER,
    APP_SMOOTH_SLOWEST,
    APP_SMOOTH_COUNT
} AppSmooth;

typedef struct {
    int pacing;      /* AppPacing; applies at the next launch */
    int clean_split; /* bool */
    int overscan;    /* AppOverscan */
    int pixels;      /* AppPixels */
    int aspect;      /* AppAspect */
    int rate;        /* AppRate: pictures a second; above 60 the steady hold is used, VSync off */
    int smooth;      /* AppSmooth: eased playfield camera (native player, nearest square pixels) */
    int soft_screens; /* bool: the vertical screen changes ease in and out (else one steady speed) */
} AppPresentation;

/* Pictures a second for an AppRate: 60, 120, 180 or 240. */
int app_present_rate_hz(int rate);

void app_present_get(AppPresentation *out);
void app_present_set(const AppPresentation *p);

/* config.ini [Game]: true if the key is one of ours. */
bool app_present_load(const char *key, const char *value);
void app_present_write(FILE *f);

/* The host's [Display] VSync for a pacing: Auto turns it on for a display of
 * up to 70 Hz (or an unknown rate, 0) and off, with the steady hold, above. */
bool app_present_vsync_for(int pacing, int refresh_hz);

/* The picture at 1x after the overscan crop and the pixel aspect: the window
 * size is a multiple of it. */
void app_present_base_size(int *w, int *h);

/* The picture for a drawable of out_w x out_h (0 x 0 before the window
 * exists). NULL keeps the host's own picture. *exact is set when the picture
 * is already at drawable resolution, to be drawn 1:1. */
const uint32_t *app_present_picture(int out_w, int out_h, bool integer_scale, int *w, int *h, bool *exact);

#ifdef __cplusplus
}
#endif
