/*
 * native_smooth.c - smooth scrolling (native_smooth.h).
 *
 * Camera. DuckTales moves its camera 1.5 pixels a frame as 1,2,1,2 and
 * starts and stops it at full speed in one frame (notes.txt). The eased
 * camera is the one Blades of Steel's port settled on: the average of the
 * game's camera over the last N frames, followed by a critically damped
 * spring with time constant T frames, integrated in SUB steps a frame so the
 * in-between pictures sample a smooth curve. Offline on the Amazon route
 * (tools/smooth-test/smooth_sim.c): the game's camera makes 22 speed jolts a
 * second, Calm (N 8, T 3) none, trailing 5 px on average and 18 at most.
 *
 * Scrooge. The camera keeps him at screen X 128, so he moves through the
 * world in the same 1,2,1,2 steps. Moved with the playfield he would jerk
 * forward and slide back every frame (seen as blur); he is drawn on his own
 * layer instead (host/ducktales_sprites.h), with his world X eased by the
 * same filter: while the camera follows him the two cancel and he stands
 * still at X 128, as on the console; where it stops he glides.
 *
 * Vertical. The screen changes between rows move the playfield 8 lines
 * every other frame (+8, 0, +8, 0: pacing240s3.log). The playfield's Y and
 * Scrooge's are shown as the average of the last two frames' positions,
 * stepped evenly between frames: 4 lines a frame, one a picture at 240,
 * starting and stopping in half steps. With soft screen changes
 * (smooth_set_soft_screen_changes) the playfield's Y follows the same kind of
 * filter as the camera instead (average of 4 frames, spring T 2): the same
 * speed in the middle, easing in and out at the ends, trailing up to 26
 * lines (offline: +8,0,+8,0 x 30, no jolts). Scrooge is followed in the
 * scrolling playfield's coordinates (his Y plus the playfield's), so while
 * the screen carries him he moves exactly with it.
 *
 * Objects (enemies, items, shots: character glide). Each object the
 * compositor draws apart is followed by its slot: the average of its last
 * OBJ_AVG positions (Amazon route: 1 px a frame, 1,0,1,0, and falls that
 * speed up 1,1,1,2,2,3), never more than OBJ_LAG pixels behind the game,
 * stepped evenly between frames. World objects also move with the eased
 * camera; objects placed on the screen do not. A jump of more than CUT
 * pixels, or more than OBJ_LOST frames unseen, starts it again where the
 * game put it (a new object in the slot); a shorter absence (blinking) is
 * ridden out.
 *
 * The playfield is the band below the HUD (native_present_playfield). Its
 * first line moves by one now and then (the HUD split's polling loop): that
 * changes only where the band is drawn. A jump of more than CUT pixels
 * (warps, respawns, a new screen) starts again on the game's positions; so
 * does a gap of more than LOST frames without the layout (shorter ones are
 * ridden out: pacing240s5.log reset every half second, a jump of up to
 * 15 px each time).
 */
#include "native/native_smooth.h"

#include "native/native_present.h"
#include "native/native_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CUT = 16, CUT_Y = 24, SUB = 8, HIST = 16, LOST = 4, OBJ_AVG = 4, OBJ_LOST = 3 };
static const double OBJ_LAG = 6.0;
static const int LEVEL_N[] = { 0, 8, 12, 12 };
static const double LEVEL_T[] = { 0, 3, 3, 4 };

typedef struct {
    double cam, x, v, sub[SUB + 1];
    double hist[HIST];
    int head, n;
} Ease;

static struct {
    int level;
    bool have, known;  /* this frame eased; a position to continue from */
    int lost;          /* frames without the layout since the last one */
    int band, last_x, last_y, vdx, vdy;
    double yc, yprev, yavg0, yavg1;  /* playfield Y unwrapped; last frame's; the two averages */
    Ease camera;
    bool player;
    int px, py, pdy;
    double pyprev, pyavg0, pyavg1;  /* Scrooge's Y in the playfield: py + yc */
    bool soft;
    Ease yease;
    double pworld;
    Ease walk;
    bool margins_ready;
} s;

static uint32_t s_left[NV_WIDTH * NV_HEIGHT], s_right[NV_WIDTH * NV_HEIGHT];
static uint32_t s_up[NV_WIDTH * NV_HEIGHT], s_down[NV_WIDTH * NV_HEIGHT];

static void ease_reset(Ease *e, double at)
{
    memset(e, 0, sizeof *e);
    e->cam = e->x = at;
    for (int k = 0; k <= SUB; k++) {
        e->sub[k] = at;
    }
}

static void ease_step_with(Ease *e, double to, int N, double T, double lag);

static void ease_step(Ease *e, double to, int level)
{
    ease_step_with(e, to, LEVEL_N[level], LEVEL_T[level], SMOOTH_MARGIN - 2);
}

static void ease_step_with(Ease *e, double to, int N, double T, double lag)
{
    e->cam = to;
    e->hist[e->head] = to;
    e->head = (e->head + 1) % HIST;
    if (e->n < HIST) {
        e->n++;
    }
    const int m = e->n < N ? e->n : N;
    double sum = 0;
    for (int k = 0; k < m; k++) {
        sum += e->hist[(e->head - 1 - k + HIST) % HIST];
    }
    const double target = sum / m, w = 1.0 / T, h = 1.0 / SUB;
    e->sub[0] = e->x;
    for (int k = 1; k <= SUB; k++) {
        double acc = w * w * (target - e->x) - 2 * w * e->v;
        e->v += acc * h;
        e->x += e->v * h;
        e->sub[k] = e->x;
    }
    double d = e->cam - e->x, fix = d > lag ? d - lag : d < -lag ? d + lag : 0;
    if (fix != 0) {
        e->x += fix;
        for (int k = 0; k <= SUB; k++) {
            e->sub[k] += fix;
        }
    }
}

static double ease_at(const Ease *e, double p, bool in_between)
{
    if (!in_between) {
        return e->x;
    }
    double k = p * SUB;
    int k0 = (int)k;
    k0 = k0 < 0 ? 0 : k0 > SUB - 1 ? SUB - 1 : k0;
    return e->sub[k0] + (e->sub[k0 + 1] - e->sub[k0]) * (k - k0);
}

static void rebase(double base)
{
    Ease *all[2] = { &s.camera, &s.walk };
    for (int i = 0; i < 2; i++) {
        Ease *e = all[i];
        e->cam -= base;
        e->x -= base;
        for (int k = 0; k <= SUB; k++) {
            e->sub[k] -= base;
        }
        for (int k = 0; k < HIST; k++) {
            e->hist[k] -= base;
        }
    }
    s.pworld -= base;
}

void smooth_set_level(int level)
{
    level = level < 0 ? 0 : level > 3 ? 3 : level;
    if (level != s.level) {
        s.level = level;
        s.have = s.known = false;
    }
}

static void player_restart(int px, int py)
{
    s.px = px;
    s.py = py;
    s.pdy = 0;
    s.pyprev = s.pyavg0 = s.pyavg1 = py + s.yc;
    s.pworld = s.camera.cam + px;
    ease_reset(&s.walk, s.pworld);
}

void smooth_frame(bool have, int scrooge_x, int scrooge_y)
{
    int band = 0, x = 0, y = 0;
    s.margins_ready = false;
    bool on = s.level > 0 && native_present_playfield(&band, &x, &y);
    if (!on && s.level > 0 && s.known && native_present_playfield_merged(s.band, &x, &y)) {
        on = true;
        band = s.band;
    }
    if (!on) {
        s.have = false;
        if (++s.lost > LOST) {
            s.known = false;
        }
        return;
    }
    s.band = band;
    int dx = x - s.last_x, dy = y - s.last_y;
    dx = dx > 256 ? dx - 512 : dx < -256 ? dx + 512 : dx;
    dy = dy > 120 ? dy - 240 : dy < -120 ? dy + 240 : dy;
    const int gap = s.lost + 1, cut = CUT * gap;
    s.lost = 0;
    if (!s.known || dx > cut || dx < -cut || dy > cut || dy < -cut) {
        s.have = s.known = true;
        s.band = band;
        s.last_x = x;
        s.last_y = y;
        s.vdx = s.vdy = 0;
        s.yc = s.yprev = s.yavg0 = s.yavg1 = y;
        ease_reset(&s.yease, y);
        ease_reset(&s.camera, x);
        s.player = on && have;
        if (s.player) {
            player_restart(scrooge_x, scrooge_y);
        }
        return;
    }
    s.have = true;
    s.last_x = x;
    s.last_y = y;
    s.vdx = dx;
    s.vdy = dy;
    s.yprev = s.yc;
    s.yc += dy;
    s.yavg0 = s.yavg1;
    s.yavg1 = (s.yc + s.yprev) / 2;
    ease_step_with(&s.yease, s.yc, 4, 2.0, SMOOTH_MARGIN_Y - 2);
    ease_step(&s.camera, s.camera.cam + dx, s.level);

    if (!have) {
        s.player = false;
    } else {
        double world = s.camera.cam + scrooge_x;
        int pdy = scrooge_y - s.py;
        if (!s.player || fabs(world - s.pworld) > CUT || pdy > CUT_Y || pdy < -CUT_Y) {
            s.player = true;
            player_restart(scrooge_x, scrooge_y);
        } else {
            s.pdy = pdy;
            s.px = scrooge_x;
            s.pyavg0 = s.pyavg1;
            s.pyavg1 = (scrooge_y + s.yc + s.pyprev) / 2.0;
            s.pyprev = scrooge_y + s.yc;
            s.py = scrooge_y;
            s.pworld = world;
            ease_step(&s.walk, world, s.level);
        }
    }
    if (s.camera.cam > 1e6 || s.camera.cam < -1e6) {
        rebase(s.camera.cam);
    }
    if (s.yc > 1e6 || s.yc < -1e6) {
        const double base = s.yc;
        s.yc -= base;
        s.yprev -= base;
        s.yavg0 -= base;
        s.yavg1 -= base;
        s.pyprev -= base;
        s.pyavg0 -= base;
        s.pyavg1 -= base;
        s.yease.cam -= base;
        s.yease.x -= base;
        for (int k = 0; k <= SUB; k++) {
            s.yease.sub[k] -= base;
        }
        for (int k = 0; k < HIST; k++) {
            s.yease.hist[k] -= base;
        }
    }
}

/* The playfield's Y as shown at phase p. */
static double shown_y(double p, bool in_between)
{
    if (s.soft) {
        return ease_at(&s.yease, p, in_between);
    }
    return in_between ? s.yavg0 + (s.yavg1 - s.yavg0) * p : s.yavg1;
}

void smooth_set_soft_screen_changes(bool on)
{
    s.soft = on;
}

bool smooth_active(int *band)
{
    if (!s.have) {
        return false;
    }
    *band = s.band;
    return true;
}

void smooth_shift(double p, bool in_between, double *dx, double *dy)
{
    *dx = *dy = 0;
    if (!s.have) {
        return;
    }
    *dx = s.camera.cam - ease_at(&s.camera, p, in_between);
    *dy = s.yc - shown_y(p, in_between);
}

bool smooth_scrooge_shift(double p, bool in_between, double *dx, double *dy)
{
    *dx = *dy = 0;
    if (!s.have || !s.player) {
        return false;
    }
    /* Drawn at his eased world X less the eased camera; the game drew him
     * at his world X less the game's camera. */
    double shown = ease_at(&s.walk, p, in_between) - ease_at(&s.camera, p, in_between);
    *dx = shown - (s.pworld - s.camera.cam);
    /* In the playfield's coordinates: his place there, less where the
     * playfield is shown, against where the game drew him. */
    const double in_field = in_between ? s.pyavg0 + (s.pyavg1 - s.pyavg0) * p : s.pyavg1;
    *dy = (in_field - shown_y(p, in_between)) - s.py;
    return true;
}

void smooth_margins(const uint32_t **left, const uint32_t **right, const uint32_t **up, const uint32_t **down)
{
    if (!s.margins_ready && s.have) {
        /* Only the edges this frame's pictures can show: the playfield is
         * drawn right of its place (left edge shows) while the eased camera
         * trails a camera moving right, and the other way round; lines above
         * and below only while the screen moves vertically. */
        double a = s.camera.cam - s.camera.sub[0], b = s.camera.cam - s.camera.x;
        double lo = a < b ? a : b, hi = a > b ? a : b;
        if (hi > 0.001) {
            native_present_shifted_background(s.band, -SMOOTH_MARGIN, 0, s_left);
        }
        if (lo < -0.001) {
            native_present_shifted_background(s.band, SMOOTH_MARGIN, 0, s_right);
        }
        double ya = s.yc - shown_y(0, true), yb = s.yc - shown_y(1, true);
        if (ya > 0.001 || yb > 0.001) {
            native_present_shifted_background(s.band, 0, -SMOOTH_MARGIN_Y, s_up);
        }
        if (ya < -0.001 || yb < -0.001) {
            native_present_shifted_background(s.band, 0, SMOOTH_MARGIN_Y, s_down);
        }
        s.margins_ready = true;
    }
    *left = s_left;
    *right = s_right;
    *up = s_up;
    *down = s_down;
}

int smooth_content_y(void)
{
    return s.last_y;
}

void smooth_debug(bool *on, int *x, int *y, int *dx, int *dy, double *shift0, double *shift1)
{
    *on = s.have;
    *x = s.last_x;
    *y = s.last_y;
    *dx = s.vdx;
    *dy = s.vdy;
    *shift0 = s.have ? s.camera.cam - s.camera.sub[0] : 0;
    *shift1 = s.have ? s.camera.cam - s.camera.x : 0;
}

/* ---- objects ---- */

typedef struct {
    bool known, seen, world;
    int lost, n, head;
    int x, y;
    double hx[OBJ_AVG], hy[OBJ_AVG];
    double ax0, ax1, ay0, ay1;
} Obj;

static Obj s_obj[SMOOTH_SLOTS];

static double clamp_near(double v, double to, double lag)
{
    return v > to + lag ? to + lag : v < to - lag ? to - lag : v;
}

void smooth_objects(const int *slot, const bool *world, const int *x, const int *y, int count)
{
    for (int i = 0; i < SMOOTH_SLOTS; i++) {
        s_obj[i].seen = false;
    }
    for (int i = 0; i < count; i++) {
        if (slot[i] < 0 || slot[i] >= SMOOTH_SLOTS) {
            continue;
        }
        Obj *o = &s_obj[slot[i]];
        int dx = x[i] - o->x, dy = y[i] - o->y;
        if (o->world) {
            dx = dx > 1024 ? dx - 2048 : dx < -1024 ? dx + 2048 : dx;
        } else {
            dx = dx > 128 ? dx - 256 : dx < -128 ? dx + 256 : dx;
        }
        dy = dy > 128 ? dy - 256 : dy < -128 ? dy + 256 : dy;
        const int gap = o->lost + 1;
        if (!s.have || !o->known || o->world != world[i] || abs(dx) > CUT * gap || abs(dy) > CUT_Y * gap) {
            memset(o, 0, sizeof *o);
            o->known = true;
            o->world = world[i];
            o->x = x[i];
            o->y = y[i];
            o->n = 0;
        } else {
            o->x += dx;
            o->y += dy;
        }
        o->seen = true;
        o->lost = 0;
        o->hx[o->head] = o->x;
        o->hy[o->head] = o->y;
        o->head = (o->head + 1) % OBJ_AVG;
        if (o->n < OBJ_AVG) {
            o->n++;
        }
        double sx = 0, sy = 0;
        for (int k = 0; k < o->n; k++) {
            sx += o->hx[k];
            sy += o->hy[k];
        }
        const bool fresh = o->n == 1;
        o->ax0 = fresh ? o->x : o->ax1;
        o->ay0 = fresh ? o->y : o->ay1;
        o->ax1 = clamp_near(sx / o->n, o->x, OBJ_LAG);
        o->ay1 = clamp_near(sy / o->n, o->y, OBJ_LAG);
        o->ax0 = clamp_near(o->ax0, o->x, 2 * OBJ_LAG);
        o->ay0 = clamp_near(o->ay0, o->y, 2 * OBJ_LAG);
    }
    for (int i = 0; i < SMOOTH_SLOTS; i++) {
        Obj *o = &s_obj[i];
        if (o->known && !o->seen && ++o->lost > OBJ_LOST) {
            o->known = false;
        }
    }
}

bool smooth_object_shift(int slot, double p, bool in_between, double *dx, double *dy)
{
    *dx = *dy = 0;
    if (slot < 0 || slot >= SMOOTH_SLOTS || !s.have) {
        return false;
    }
    const Obj *o = &s_obj[slot];
    if (!o->known || !o->seen) {
        return false;
    }
    const double sx = in_between ? o->ax0 + (o->ax1 - o->ax0) * p : o->ax1;
    const double sy = in_between ? o->ay0 + (o->ay1 - o->ay0) * p : o->ay1;
    *dx = sx - o->x;
    *dy = sy - o->y;
    if (o->world) {
        double cx, cy;
        smooth_shift(p, in_between, &cx, &cy);
        *dx += cx;
    }
    return true;
}
