/*
 * native_present.h - the native runtime's picture for the window
 * (native/native_present.c): each machine frame's record (native_state.h)
 * drawn with the native renderer and offered through the cycle host's
 * presentation interface, so the port's presentation code runs unchanged.
 */
#pragma once
#include "native/native_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame that just ended: draw it and make it the current picture. */
void native_present_frame(const NativeFrame *frame);

/* The frame's layout for smooth scrolling (native_smooth.h): true when it
 * has a fixed band at the top (the HUD) and one scrolled playfield below it,
 * with *band its first line, *x its horizontal position (0-511) and *y the
 * nametable row its lines start from relative to the screen (0-239). */
bool native_present_playfield(int *band, int *x, int *y);

/* The same when the split vanished: the playfield at X 0 lines up with the
 * HUD (NG2's lesson), so the frame is one segment. With the band known from
 * the frames before, *x and *y are that segment's position at line band. */
bool native_present_playfield_merged(int band, int *x, int *y);

/* Draw the current picture again on its next use (a compositor setting
 * changed). */
void native_present_refresh(void);

/* The playfield's background (no sprites) with its position moved by dx
 * pixels right and dy lines down, lines band-239, as ARGB into out
 * (256 x 240; lines above band are left alone): what lies just off the
 * picture's edges. */
void native_present_shifted_background(int band, int dx, int dy, uint32_t *out);

#ifdef __cplusplus
}
#endif
