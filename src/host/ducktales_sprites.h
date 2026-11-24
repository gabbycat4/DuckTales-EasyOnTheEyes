/*
 * ducktales_sprites.h - sprites without the console's limits, in the picture
 * the window presents (host/ducktales_sprites.c). The machine, its picture
 * and every hash stay as the console's; only what is shown changes.
 *
 *   NoSpriteFlicker   config.ini [Game] key, --no-sprite-flicker on|off,
 *                     the Settings page; on by default, off presents the
 *                     machine's own picture
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_SPRITES_KEY     "NoSpriteFlicker"
#define DT_SPRITES_OPTION  "--no-sprite-flicker"
#define DT_SPRITES_LABEL   "No sprite flicker"
#define DT_SPRITES_DEFAULT true

void dt_sprites_set(bool on);
bool dt_sprites_get(void);
/* Text to value: on/off/true/false/yes/no/1/0. False leaves *out alone. */
bool dt_sprites_parse(const char *text, bool *out);

/* Host callbacks: every finished frame, and a machine that no longer follows
 * the frames seen so far (power on, a loaded state). */
void dt_sprites_frame_end(void);
void dt_sprites_forget(void);
/* The mods were loaded: compose every picture while they paint tiles or palettes. */
void dt_sprites_mods_changed(void);

/* Smooth scrolling (native/native_smooth.h): with apart on, the composed
 * picture leaves Scrooge's sprites out, and dt_sprites_scrooge_layer draws
 * them alone (256 x 240 ARGB, 0 where none) for the current picture, with
 * his on-screen position as the game placed him (*x may be off screen).
 * False when there is no such layer (no rebuilt list, he is not drawn, or
 * No sprite flicker is off). */
void dt_sprites_set_scrooge_apart(bool on);
bool dt_sprites_scrooge_layer(uint32_t *out, int *x, int *y);

/* The same for the other objects (character glide): with objects apart on,
 * the composed picture leaves out every drawn object that has no sprite
 * behind the background and none in the HUD band (apart), and
 * dt_sprites_object_layer draws one of them alone into out (bw x bh ARGB, 0
 * where none, at most cap pixels) at screen position bx, by. Objects come in
 * the list's drawing order, front first. x: world X (page * 256 + low) when
 * world, else screen X; y: screen Y; action: its script (a change is a new
 * object in the slot). */
typedef struct {
    int slot, first, count;
    bool world, apart;
    int x, y, action;
} DtObject;

void dt_sprites_set_objects_apart(bool on);
int dt_sprites_objects(const DtObject **out);
bool dt_sprites_object_layer(const DtObject *o, uint32_t *out, int cap, int *bx, int *by, int *bw, int *bh);

typedef struct {
    uint64_t composed;     /* pictures drawn from the rebuilt sprite list */
    uint64_t reused;       /* pictures whose OAM equals the last rebuilt one (list kept) */
    uint64_t fallback;     /* pictures drawn from the frame's OAM in its own order */
    uint64_t declined;     /* pictures left to the machine (no background yet) */
    uint64_t hud_restored; /* pictures with the HUD the builder left out for Scrooge's Y */
    uint64_t leftover;     /* rebuilt pictures that kept unparked entries no slot made */
    uint64_t lines_over_8; /* lines with more than 8 sprites drawn */
    uint64_t beyond_63;    /* sprites drawn past the 63 OAM entries */
} DtSpritesStats;

void dt_sprites_stats(DtSpritesStats *out);
/* Developer log: one line per composed picture (DUCKTALES_DEV_DIAGNOSTICS). */
void dt_sprites_set_log(FILE *f);

#ifdef __cplusplus
}
#endif
