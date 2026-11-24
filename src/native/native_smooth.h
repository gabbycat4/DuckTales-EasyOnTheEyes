/*
 * native_smooth.h - smooth scrolling for the native player's window
 * (native/native_smooth.c): an eased camera for the playfield, Scrooge eased
 * on his own layer, and in-between steps for the vertical screen changes,
 * shown at fractional positions on the in-between pictures. Presentation
 * only: the machine, its picture and every hash are untouched.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pixels of picture kept on each side of the playfield for the eased camera
 * (it never trails the game's camera by more than SMOOTH_MARGIN - 2), and
 * lines above and below it for the vertical steps. */
#define SMOOTH_MARGIN   32
#define SMOOTH_MARGIN_Y 32

/* [Game] SmoothScrolling (app_present.h AppSmooth): 0 off, 1 calm, 2 calmer,
 * 3 slowest. */
void smooth_set_level(int level);

/* [Game] ScreenChanges: soft eases the vertical screen changes in and out
 * (on), even moves them at one steady speed from start to stop. */
void smooth_set_soft_screen_changes(bool on);

/* After each machine frame (native_present_frame): follow the playfield's
 * position, and Scrooge's on-screen position (have: his layer exists). */
void smooth_frame(bool have, int scrooge_x, int scrooge_y);

/* Is the playfield eased this frame? *band: its first line (picture lines,
 * before any crop). */
bool smooth_active(int *band);

/* How far right and down the playfield is drawn, in NES pixels, for a
 * picture at phase 0 <= p < 1 of the frame (in_between: more than one picture
 * a frame). */
void smooth_shift(double p, bool in_between, double *dx, double *dy);

/* The same for Scrooge's layer. False when he has none this frame. */
bool smooth_scrooge_shift(double p, bool in_between, double *dx, double *dy);

/* What lies off the picture, each 256 x 240 ARGB: the playfield moved
 * SMOOTH_MARGIN pixels left / right (its off-picture columns at the left /
 * right edge), and SMOOTH_MARGIN_Y lines up / down. Made at most once a
 * frame. */
void smooth_margins(const uint32_t **left, const uint32_t **right, const uint32_t **up, const uint32_t **down);

/* Character glide: after smooth_frame, the objects drawn apart this frame
 * (host/ducktales_sprites.h DtObject: slot, world, x, y). */
#define SMOOTH_SLOTS 16
void smooth_objects(const int *slot, const bool *world, const int *x, const int *y, int count);

/* How far right and down object `slot` is drawn from where the game put
 * it, for a picture at phase p. False when it is not tracked. */
bool smooth_object_shift(int slot, double p, bool in_between, double *dx, double *dy);

/* The nametable row the playfield's screen line 0 starts from (0-239):
 * screen line L shows row (smooth_content_y() + L) % 240. */
int smooth_content_y(void);

/* For the pacing log: on, the playfield's position and this frame's steps,
 * and the camera's shift at the frame's first and last picture. */
void smooth_debug(bool *on, int *x, int *y, int *dx, int *dy, double *shift0, double *shift1);

#ifdef __cplusplus
}
#endif
