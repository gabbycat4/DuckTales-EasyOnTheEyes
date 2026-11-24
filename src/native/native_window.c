/*
 * native_window.c - the player build's window on the native runtime
 * (docs/native-runtime.md "The player build"): the recompiled program runs on
 * the native bus one 60 Hz frame at a time (native_run_frame), the native
 * renderer draws the picture (native_present.c) and the native synth
 * (native_audio.h) the sound. No part of NESRecomp's cycle host is linked.
 *
 * It is the program entry's host (host/ducktales_main.c calls
 * ducktales_host_main, as it calls NESRecomp's in the cycle build), and it
 * reaches the port through the same calls the cycle host's extras make
 * (host/ducktales_host.c), so the menu, ROM import, settings, bindings and
 * presentation in src/ui and the enhancements and sprite compositor in
 * src/game and src/host are the ones the cycle build runs:
 *
 *   settings      config.ini through NESRecomp's cyc_settings (the same
 *                 file and keys); written on F11 and at exit, then
 *                 reconciled by app_settings.c as after the cycle host's saves
 *   input         cyc_input_eval of the player's bindings each frame, then
 *                 app_ui_input; Escape (the Menu binding) opens the menu
 *                 through app_ui_event; F11 and Alt+Enter full screen, Tab
 *                 (held) fast-forward, F12 a screenshot of the picture
 *   menu          the open menu calls menu_tick each menu frame: the game
 *                 frames due at 60.0988 Hz run with no buttons pressed and
 *                 their sound is queued, so the game keeps running behind the
 *                 menu ([Game] MenuPausesGame stops it, app_ui.h)
 *   No slowdown   [Game] NoSlowdown: the runtime's lag mode
 *                 (NATIVE_LAG_NONE when on, NATIVE_LAG_FAITHFUL when off)
 *   picture       app_ui_present (ui/app_present.h) or the plain picture, in
 *                 a texture the size of the picture, logical size and integer
 *                 scaling as the cycle host keeps them
 *   pacing        each frame at its deadline (1 / 60.0988 s); VSync as
 *                 [Display] VSync says (FramePacing resolves it at launch),
 *                 off while fast-forwarding or --uncapped
 *   sound         native_audio_frame per frame, queued at 48 kHz with at
 *                 most 100 ms waiting; none while fast-forwarding. When late
 *                 frames let the queue fall below the device buffer plus one
 *                 frame, the frame is stretched to refill it, so the device
 *                 never runs dry
 *   scheduling    Windows: no power throttling (EcoQoS, timer coalescing),
 *                 the frame loop in the MMCSS "Games" class and a 1 ms timer,
 *                 so an unfocused window keeps its pace
 *   save states   F8 saves and F9 loads <exe dir>/saves/DuckTales.native-state:
 *                 CPU, bus, video state and the frame count (native_runtime.h)
 *
 * Options: ROM [--hidden] [--exit-after N] [--config FILE] [--uncapped]
 * [--present-out FILE [--present-every N]] and each enhancement and sprite
 * option (--jump-scale 150 ...). Off screen (no window opens):
 * --frames N [--input ROUTE] [--lag none|faithful] [--present-out FILE
 * [--present-every N]] writes the picture the window would show (at 1x, the
 * presentation settings at their defaults) after every Nth frame, named as
 * the cycle host's --present-out names them, for tests/native_player_test.py;
 * --wav-out FILE writes the synth's sound as 48 kHz mono 16-bit PCM, before
 * the window's volume and queue stretching.
 * --state-check F saves the machine after frame F, runs 300 frames, loads the
 * state and runs them again, and reports whether CPU RAM, the cycle count and
 * the video memories matched on every one. The summary line lists how often
 * each enhancement's hook acted.
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include "native/native_present.h"
#include "native/native_smooth.h"
#include "native/native_route.h"
#include "native/native_runtime.h"
#include "native/native_state.h"
#include "native/native_audio.h"

#include "game/enhancements.h"
#include "host/ducktales_host.h"
#include "host/ducktales_sprites.h"
#include "mods/app_mods.h"
#include "ui/app_present.h"
#include "ui/app_ui.h"

#include "cpu6502.h"
#include "cyc_input.h"
#include "cyc_overlay.h"
#include "cyc_png.h"
#include "cyc_recomp.h"
#include "cyc_render.h"
#include "cyc_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#endif

#define FRAME_HZ      60.0988
#define AUDIO_RATE    48000
#define STATE_MAGIC   "DTNSTATE"
#define STATE_VERSION 1u

int ducktales_host_main(int argc, char **argv);

/* ---- CYC_PACING_LOG=FILE: per frame, where the time went ----
 * One line per shown frame, times in ms:
 *   emu      native_run_frame + picture + sprite compositor + mods
 *   audio    the synth's samples queued
 *   wait     the hold until the frame's deadline
 *   draw     show(): the presentation picture, texture upload, copy
 *   present  SDL_RenderPresent
 *   at       when the picture was handed over, from the frame's deadline
 *   interval since the previous picture was handed over
 * and a summary at exit (mean / p95 / max, and pictures that came more than
 * 1.5 frame periods after the previous one). */
#define PACE_MAX 200000
static FILE *s_pace;
static float *s_pace_pic;
static long s_pace_pic_n, s_pace_skipped;
static double s_pace_step_ms;
static Uint64 s_pace_first, s_pace_last;
static Uint64 s_split[4]; /* CYC_PACING_LOG: machine, picture, sprites, mods */
static double s_prep_ms; /* defined with smooth_prepare */
static double s_fin_ms[4]; /* frame_finish: apart, Scrooge's layer, easing, total */
static double s_work_at[2]; /* the frame's work: start and end, ms from the previous frame's deadline */
static bool s_player_ok;
static Uint64 s_pace_prev;
static float *s_pace_emu, *s_pace_draw, *s_pace_int;
static long s_pace_n;

static void pace_open(void)
{
    const char *e = getenv("CYC_PACING_LOG");
    if (!e || !*e) {
        return;
    }
    s_pace = fopen(e, "w");
    if (s_pace) {
        s_pace_emu = (float *)calloc(PACE_MAX, sizeof(float));
        s_pace_draw = (float *)calloc(PACE_MAX, sizeof(float));
        s_pace_int = (float *)calloc(PACE_MAX, sizeof(float));
        s_pace_pic = (float *)calloc(4 * PACE_MAX, sizeof(float));
        fprintf(s_pace, "# native player pacing log; frame period %.3f ms\n", 1000.0 / FRAME_HZ);
    }
}

static void pace_line(long frame, double emu, double audio, double wait, double draw, double present, double at,
                      double interval, bool vsync)
{
    if (!s_pace) {
        return;
    }
    const double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
    fprintf(s_pace,
            "%ld frame: emu %.2f audio %.2f wait %.2f draw %.2f present %.2f at %+.2f interval %.2f vsync %d"
            " | machine %.2f picture %.2f sprites %.2f mods %.2f prep %.2f | finish apart %.2f layer %.2f ease %.2f"
            " total %.2f | work at %+.2f..%+.2f",
            frame, emu, audio, wait, draw, present, at, interval, (int)vsync, (double)s_split[0] * ms,
            (double)s_split[1] * ms, (double)s_split[2] * ms, (double)s_split[3] * ms, s_prep_ms, s_fin_ms[0],
            s_fin_ms[1], s_fin_ms[2], s_fin_ms[3], s_work_at[0], s_work_at[1]);
    bool on;
    int sx, sy, sdx, sdy;
    double sh0, sh1;
    smooth_debug(&on, &sx, &sy, &sdx, &sdy, &sh0, &sh1);
    if (on) {
        fprintf(s_pace, " | smooth x %d y %d dx %+d dy %+d shift %+.2f..%+.2f scrooge %d", sx, sy, sdx, sdy, sh0, sh1,
                (int)s_player_ok);
    } else {
        fprintf(s_pace, " | smooth off");
    }
    fputc('\n', s_pace);
    if (s_pace_emu && s_pace_n < PACE_MAX) {
        s_pace_emu[s_pace_n] = (float)emu;
        s_pace_draw[s_pace_n] = (float)draw;
        s_pace_int[s_pace_n] = (float)interval;
        s_pace_n++;
    }
}

/* Every picture after the first: how long after the previous one it was
 * handed over, and how many in-between pictures were skipped. */
static void pace_picture(long frame, int i, double at, double interval, double draw)
{
    if (!s_pace) {
        return;
    }
    fprintf(s_pace, "%ld   picture %d at %+.2f interval %.2f draw %.2f\n", frame, i, at, interval, draw);
    if (s_pace_pic && s_pace_pic_n < 4 * PACE_MAX && frame > 120) {
        s_pace_pic[s_pace_pic_n++] = (float)interval;
    }
}

static void pace_skip(long frame, int count)
{
    if (s_pace) {
        fprintf(s_pace, "%ld   skipped %d in-between picture%s\n", frame, count, count == 1 ? "" : "s");
        if (frame > 120) {
            s_pace_skipped += count;
        }
    }
}

static int pace_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void pace_stat(const char *name, float *v, long n)
{
    if (n <= 0) {
        return;
    }
    double sum = 0, sq = 0;
    for (long i = 0; i < n; i++) {
        sum += v[i];
        sq += (double)v[i] * v[i];
    }
    double mean = sum / n, sd = sq / n - mean * mean;
    qsort(v, (size_t)n, sizeof *v, pace_cmp);
    fprintf(s_pace, "# %-8s mean %.3f sd %.3f p50 %.3f p95 %.3f p99 %.3f max %.3f\n", name, mean, sd > 0 ? sqrt(sd) : 0,
            v[n / 2], v[n * 95 / 100], v[n * 99 / 100], v[n - 1]);
}

static void pace_close(void)
{
    if (!s_pace) {
        return;
    }
    long n = s_pace_n > 120 ? s_pace_n - 120 : 0, late = 0;
    float *e = s_pace_emu + (s_pace_n - n), *d = s_pace_draw + (s_pace_n - n), *iv = s_pace_int + (s_pace_n - n);
    const float period = (float)(1000.0 / FRAME_HZ);
    for (long i = 0; i < n; i++) {
        late += iv[i] > 1.5f * period;
    }
    fprintf(s_pace, "# summary over %ld frames (first 120 left out); late pictures (> 1.5 periods): %ld\n", n, late);
    pace_stat("emu", e, n);
    pace_stat("draw", d, n);
    pace_stat("interval", iv, n);
    if (s_pace_pic_n) {
        long late_pics = 0;
        for (long i = 0; i < s_pace_pic_n; i++) {
            late_pics += s_pace_pic[i] > 1.5 * s_pace_step_ms;
        }
        double secs = (double)(s_pace_last - s_pace_first) / (double)SDL_GetPerformanceFrequency();
        fprintf(s_pace, "# in-between pictures: %ld shown, %ld skipped, %ld late (> 1.5 x %.2f ms)", s_pace_pic_n,
                s_pace_skipped, late_pics, s_pace_step_ms);
        if (secs > 0) {
            fprintf(s_pace, "; all pictures: %.1f a second", (double)(s_pace_pic_n + n) / secs);
        }
        fprintf(s_pace, "\n");
        pace_stat("pic int", s_pace_pic, s_pace_pic_n);
    }
    fclose(s_pace);
    free(s_pace_pic);
    s_pace = NULL;
    free(s_pace_emu);
    free(s_pace_draw);
    free(s_pace_int);
}

void ducktales_host_apply_no_slowdown(bool on)
{
    native_set_lag_mode(on ? NATIVE_LAG_NONE : NATIVE_LAG_FAITHFUL);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static uint8_t s_frame_mute;

static void power_on(void)
{
    native_power_on();
    dt_sprites_forget();
    app_mods_reset();
    s_frame_mute = 0;
    ducktales_host_apply_no_slowdown(ducktales_host_no_slowdown());
    if (ducktales_host_no_slowdown()) {
        printf("[ducktales] No slowdown: the native runtime holds VBlank until each frame's logic is done\n");
    }
    fflush(stdout);
}

static void run_frame(uint8_t pad0, uint8_t pad1)
{
    native_set_controller(0, pad0);
    native_set_controller(1, pad1);
    uint8_t mute = app_mods_audio_mute();
    Uint64 t0 = SDL_GetPerformanceCounter();
    native_run_frame();
    Uint64 t1 = SDL_GetPerformanceCounter();
    native_present_frame(native_last_frame());
    Uint64 t2 = SDL_GetPerformanceCounter();
    dt_sprites_frame_end();
    Uint64 t3 = SDL_GetPerformanceCounter();
    s_frame_mute = mute;
    if (app_mods_audio_active()) {
        app_mods_audio_frame_end(native_cpu_ram());
    }
    app_mods_scripts_frame();
    Uint64 t4 = SDL_GetPerformanceCounter();
    s_split[0] = t1 - t0;
    s_split[1] = t2 - t1;
    s_split[2] = t3 - t2;
    s_split[3] = t4 - t3;
}

/* What the window shows (ui/app_present.h, or the plain picture) for a
 * drawable of out_w x out_h; 0 x 0 for the picture at 1x. */
static const uint32_t *picture(int *w, int *h)
{
    const uint32_t *p = app_ui_present(w, h);
    return p && *w > 0 && *h > 0 ? p : cyc_render_present(w, h);
}

static void numbered_path(char *out, size_t cap, const char *base, long frame)
{
    const char *dot = strrchr(base, '.');
    const char *slash = strrchr(base, '/'), *bslash = strrchr(base, '\\');
    if (dot && ((slash && dot < slash) || (bslash && dot < bslash))) {
        dot = NULL;
    }
    int stem = (int)(dot ? (size_t)(dot - base) : strlen(base));
    snprintf(out, cap, "%.*s_%05ld%s", stem, base, frame, dot ? dot : ".png");
}

/* ---- off screen: a route, the pictures to files ---- */

/* --music-under-effects / --overlap-effects for this run, else -1: the saved
 * [Audio] value (ducktales_host.h). */
static int s_voice_override[2] = { -1, -1 };

static void apply_voices(void)
{
    native_audio_voices(s_voice_override[0] >= 0 ? s_voice_override[0] != 0 : ducktales_host_music_under_effects(),
                        s_voice_override[1] >= 0 ? s_voice_override[1] != 0 : ducktales_host_overlap_effects());
}

/* The frame's sound: the synth and its extra voices with the channels the
 * mods took muted, then their files mixed in (mods/app_mods.h). */
static size_t frame_sound(int16_t *pcm, size_t cap, int rate)
{
    apply_voices();
    native_audio_set_mute(s_frame_mute);
    size_t n = native_audio_frame(native_last_frame(), pcm, cap);
    app_mods_audio_mix(pcm, n, rate);
    return n;
}

static uint64_t machine_hash(void)
{
    uint64_t h = 1469598103934665603ull;
    const NativeVideoMemory *m = native_video_memory();
    const uint8_t *parts[2] = { native_cpu_ram(), (const uint8_t *)m };
    const size_t sizes[2] = { 0x800, sizeof *m };
    for (int p = 0; p < 2; p++) {
        for (size_t i = 0; i < sizes[p]; i++) {
            h = (h ^ parts[p][i]) * 1099511628211ull;
        }
    }
    return h ^ native_cycle_count();
}

static int run_off_screen(long frames, const char *input, int lag, const char *present_out, long present_every,
                          long state_check, const char *wav_out)
{
    enum {
        CHECK_FRAMES = 300
    };

    static uint64_t first[CHECK_FRAMES];
    uint8_t *state = NULL;
    size_t state_size = 0;
    long check_mismatch = -1;
    NativeRoute replay;

    NativeRoute route;
    memset(&route, 0, sizeof route);
    if (input && !native_route_load(&route, input)) {
        return 2;
    }
    FILE *wav = wav_out ? fopen(wav_out, "wb") : NULL;
    if (wav_out && !wav) {
        fprintf(stderr, "cannot write %s\n", wav_out);
        return 2;
    }
    uint32_t wav_samples = 0;
    if (wav) {
        native_audio_reset(AUDIO_RATE);
        native_audio_wav_header(wav, AUDIO_RATE, 0);
    }
    power_on();
    if (lag >= 0) {
        native_set_lag_mode((NativeLagMode)lag);
    }
    for (long frame = 0; frame < frames; frame++) {
        uint8_t held[2];
        native_route_frame(&route, frame, held);
        run_frame(held[0], held[1]);
        if (wav) {
            static int16_t pcm[NATIVE_AUDIO_MAX_FRAME_SAMPLES];
            size_t n = frame_sound(pcm, NATIVE_AUDIO_MAX_FRAME_SAMPLES, AUDIO_RATE);
            fwrite(pcm, sizeof pcm[0], n, wav);
            wav_samples += (uint32_t)n;
        }
        if (state_check >= 0 && frame == state_check) {
            state_size = native_state_save(NULL, 0);
            state = (uint8_t *)malloc(state_size);
            if (state) {
                native_state_save(state, state_size);
            }
            replay = route;
        }
        if (state && frame > state_check && frame <= state_check + CHECK_FRAMES) {
            first[frame - state_check - 1] = machine_hash();
            if (frame == state_check + CHECK_FRAMES) {
                NativeRoute saved = route;
                bool loaded = native_state_load(state, state_size);
                route = replay;
                check_mismatch = loaded ? 0 : state_check;
                for (long f = state_check + 1; loaded && f <= state_check + CHECK_FRAMES; f++) {
                    uint8_t h2[2];
                    native_route_frame(&route, f, h2);
                    run_frame(h2[0], h2[1]);
                    if (machine_hash() != first[f - state_check - 1] && !check_mismatch) {
                        check_mismatch = f;
                    }
                }
                route = saved;
                printf("native player: state check after frame %ld: %s over %d frames\n", state_check,
                       !loaded          ? "the state did not load"
                       : check_mismatch ? "differs"
                                        : "identical",
                       CHECK_FRAMES);
                if (check_mismatch) {
                    printf("  first difference at frame %ld\n", check_mismatch);
                }
                free(state);
                state = NULL;
            }
        }
        if (present_out && present_every > 0 && frame % present_every == 0) {
            int w, h;
            const uint32_t *pic = picture(&w, &h);
            char path[1100];
            numbered_path(path, sizeof path, present_out, frame);
            if (!cyc_write_png(path, pic, w, h)) {
                fprintf(stderr, "cannot write %s\n", path);
            }
        }
    }
    if (wav) {
        native_audio_wav_header(wav, AUDIO_RATE, wav_samples);
        fclose(wav);
    }
    printf("native player: frames=%ld cycles=%llu fallback_instructions=%llu unmodeled_reads=%llu\n", frames,
           (unsigned long long)native_cycle_count(), (unsigned long long)native_fallback_instructions(),
           (unsigned long long)native_unmodeled_reads());
    printf("native player: hooks acted:");
    for (int e = 0; e < DT_ENH_COUNT; e++) {
        printf(" %s=%llu", dt_enh_info((DtEnh)e)->key, (unsigned long long)dt_enh_applied((DtEnh)e));
    }
    printf("\n");
    return check_mismatch > 0 ? 1 : 0;
}

/* ---- the window ---- */

static CycSettings s_set;
static char s_set_path[1100];
static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static int s_tex_w, s_tex_h;
static bool s_vsync_active;
static SDL_GameController *s_pads[CYC_INPUT_MAX_PADS];
static char s_pad_guid[CYC_INPUT_MAX_PADS][40];
static int s_pad_count;
static bool s_fullscreen_chord;
static char s_note_title[64], s_note_body[128];
static uint64_t s_note_until;

static void game_save(void *ctx, FILE *f)
{
    (void)ctx;
    ducktales_host_write_game_settings(f);
}

static void game_load(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    ducktales_host_load_game_setting(key, value);
}

static void save_settings(void)
{
    static const CycSettingsGame GAME = { NULL, NULL, game_save };
    if (!cyc_settings_save(&s_set, s_set_path, &GAME)) {
        fprintf(stderr, "cannot write %s\n", s_set_path);
    }
    app_ui_host_saved();
}

static void note(const char *title, const char *body)
{
    snprintf(s_note_title, sizeof s_note_title, "%s", title);
    snprintf(s_note_body, sizeof s_note_body, "%s", body ? body : "");
    s_note_until = SDL_GetTicks64() + 2500;
    printf("[ducktales] %s%s%s\n", title, body && *body ? ": " : "", body ? body : "");
    fflush(stdout);
}

static void state_path(char *out, size_t cap)
{
    char dir[1100];
    char *base = SDL_GetBasePath();
    snprintf(dir, sizeof dir, "%ssaves", base ? base : "");
    if (base) {
        SDL_free(base);
    }
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    snprintf(out, cap, "%s/DuckTales.native-state", dir);
}

static void save_state(void)
{
    char path[1200];
    state_path(path, sizeof path);
    size_t size = native_state_save(NULL, 0);
    uint8_t *blob = (uint8_t *)malloc(size);
    FILE *f = blob ? fopen(path, "wb") : NULL;
    bool ok = false;
    if (f) {
        uint32_t head[3] = { STATE_VERSION, cyc_native_prg_hash, (uint32_t)size };
        native_state_save(blob, size);
        ok = fwrite(STATE_MAGIC, 1, 8, f) == 8 && fwrite(head, sizeof head, 1, f) == 1 &&
             fwrite(blob, 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
    }
    free(blob);
    note(ok ? "STATE SAVED" : "STATE NOT SAVED", ok ? NULL : path);
}

static void load_state(void)
{
    char path[1200], why[160] = "";
    state_path(path, sizeof path);
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    uint32_t head[3];
    if (!data) {
        snprintf(why, sizeof why, "no saved state");
    } else if (size < 8 + sizeof head || memcmp(data, STATE_MAGIC, 8)) {
        snprintf(why, sizeof why, "not a state file");
    } else {
        memcpy(head, data + 8, sizeof head);
        if (head[0] != STATE_VERSION || head[1] != cyc_native_prg_hash || head[2] != size - 8 - sizeof head) {
            snprintf(why, sizeof why, "saved by another version");
        } else if (!native_state_load(data + 8 + sizeof head, size - 8 - sizeof head)) {
            snprintf(why, sizeof why, "the state does not fit this build");
        }
    }
    free(data);
    if (!why[0]) {
        dt_sprites_forget();
        app_mods_reset();
        s_frame_mute = 0;
    }
    note(why[0] ? "STATE NOT LOADED" : "STATE LOADED", why[0] ? why : NULL);
}

static void open_pads(void)
{
    for (int i = 0; i < s_pad_count; i++) {
        SDL_GameControllerClose(s_pads[i]);
    }
    s_pad_count = 0;
    for (int i = 0; i < SDL_NumJoysticks() && s_pad_count < CYC_INPUT_MAX_PADS; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (!c) {
            continue;
        }
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(c)), s_pad_guid[s_pad_count],
                                  sizeof s_pad_guid[0]);
        s_pads[s_pad_count++] = c;
    }
}

/* One frame of device state, as the cycle host reads it (cyc_sdl.c): the
 * Enter of an Alt+Enter stays the full-screen chord's until it is released. */
static void read_frame(CycInputFrame *f)
{
    static uint8_t keys[SDL_NUM_SCANCODES];
    memset(f, 0, sizeof *f);
    int n = 0;
    const Uint8 *real = SDL_GetKeyboardState(&n);
    for (int i = 0; i < SDL_NUM_SCANCODES; i++) {
        keys[i] = (uint8_t)(i < n && real[i]);
    }
    bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    bool enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
    if (!enter) {
        s_fullscreen_chord = false;
    } else if (alt) {
        s_fullscreen_chord = true;
    }
    if (s_fullscreen_chord && !alt) {
        keys[SDL_SCANCODE_RETURN] = keys[SDL_SCANCODE_KP_ENTER] = 0;
    }
    f->keys = keys;
    f->key_count = SDL_NUM_SCANCODES;
    f->pad_count = s_pad_count;
    for (int i = 0; i < s_pad_count; i++) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(s_pads[i], (SDL_GameControllerButton)b)) {
                f->pad_buttons[i] |= 1u << b;
            }
        }
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX && a < CYC_INPUT_PAD_AXES; a++) {
            f->pad_axes[i][a] = SDL_GameControllerGetAxis(s_pads[i], (SDL_GameControllerAxis)a);
        }
        f->pad_guid[i] = s_pad_guid[i];
    }
}

static void set_vsync(bool on)
{
    if (on == s_vsync_active) {
        return;
    }
    if (SDL_RenderSetVSync(s_ren, on ? 1 : 0) == 0) {
        s_vsync_active = on;
    }
}

static void apply_integer_scale(void)
{
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    SDL_bool want = s_set.integer_scale && ow >= s_tex_w && oh >= s_tex_h ? SDL_TRUE : SDL_FALSE;
    if (SDL_RenderGetIntegerScale(s_ren) != want) {
        SDL_RenderSetIntegerScale(s_ren, want);
    }
}

static bool ensure_texture(int w, int h)
{
    if (s_tex && w == s_tex_w && h == s_tex_h) {
        return true;
    }
    if (s_tex) {
        SDL_DestroyTexture(s_tex);
    }
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    s_tex_w = w;
    s_tex_h = h;
    if (!s_tex) {
        return false;
    }
    SDL_SetTextureScaleMode(s_tex, s_set.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderSetLogicalSize(s_ren, w, h);
    apply_integer_scale();
    return true;
}

/* The windowed size: the picture at 1x times the window scale, the largest
 * multiple that fits the display's usable area. */
static void window_size(int display, int *w, int *h)
{
    int pw, ph, scale = s_set.window_scale < 1 ? 1 : s_set.window_scale;
    picture(&pw, &ph);
    SDL_Rect area;
    if (display >= 0 && SDL_GetDisplayUsableBounds(display, &area) == 0) {
        while (scale > 1 && (pw * scale > area.w || ph * scale > area.h - 48)) {
            --scale;
        }
    }
    *w = pw * scale;
    *h = ph * scale;
}

static void apply_fullscreen(void)
{
    Uint32 fs = s_set.fullscreen == 1   ? SDL_WINDOW_FULLSCREEN_DESKTOP
                : s_set.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN
                                        : 0;
    if ((SDL_GetWindowFlags(s_win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) == fs) {
        return;
    }
    SDL_SetWindowFullscreen(s_win, fs);
    if (!fs) {
        int w, h;
        window_size(SDL_GetWindowDisplayIndex(s_win), &w, &h);
        SDL_SetWindowSize(s_win, w, h);
    }
}

/* ---- smooth scrolling (native_smooth.h) ----
 * Three layers a picture, built once a frame: the HUD rows as they are; the
 * playfield with SMOOTH_MARGIN columns more on each side and SMOOTH_MARGIN_Y
 * lines more above and below, moved by the eased camera; and Scrooge alone
 * (host/ducktales_sprites.h), moved by his own easing. The GPU places them
 * between pixels at the display's resolution. Only for nearest, square
 * pixels with No sprite flicker on: the other modes are resampled on the CPU. */
static SDL_Texture *s_hud_tex, *s_field_tex, *s_player_tex;
static int s_field_w, s_field_h, s_hud_w, s_hud_h, s_field_band;
static uint32_t *s_field_px;
static size_t s_field_cap;
static uint32_t s_player_px[256 * 240];
static bool s_player_ok, s_want_apart, s_apart;
/* Character glide: each object drawn apart, packed in one atlas texture. */
#define DT_OBJ_SLOTS_WINDOW 16
#define ATLAS_W 512
#define ATLAS_H 512
typedef struct {
    int slot, ax, ay, bx, by, bw, bh;
} ObjLayer;
static SDL_Texture *s_atlas_tex;
static uint32_t s_atlas[ATLAS_W * ATLAS_H];
static ObjLayer s_objs[DT_OBJ_SLOTS_WINDOW];
static int s_obj_n;
static int s_player_x, s_player_y;
static long s_layers_frame = -1;
/* Each nametable row of the playfield as it last looked while on screen
 * (Mega Man 2's lesson): during the screen changes the game writes the next
 * screen into the rows just off the picture, so rows the easing reveals
 * there are taken from here, when they were seen in the last few frames at
 * the same camera X. */
#define ROW_CACHE_W (256 + 2 * SMOOTH_MARGIN)
static uint32_t s_rows[240][ROW_CACHE_W];
static long s_row_frame[240];
static int s_row_x[240];
/* The last frame the playfield moved vertically: while a screen change is
 * under way (and a few frames after) the outermost EDGE_ROWS visible lines
 * at the top and bottom are also taken from the row memory when it has
 * them, since the game writes the next screen there too (the blue strip at
 * the bottom, Nov 24). */
#define EDGE_ROWS 8
static long s_vmove_frame = -1000;
static int s_vmove_dir;
/* What smooth_prepare built for s_layers_frame: picture size and geometry. */
static bool s_layers_ok;
static int s_lay_w, s_lay_h, s_lay_band, s_lay_top, s_lay_side;

/* Scrooge on his own layer while smoothing can draw him (set by the window
 * loop before each frame; the menu's frames turn it off). */
static void scrooge_apart(bool want)
{
    int band, x, y, known;
    bool on = want && dt_sprites_get() &&
              (native_present_playfield(&band, &x, &y) ||
               (smooth_active(&known) && native_present_playfield_merged(known, &x, &y)));
    if (on != s_apart) {
        s_apart = on;
        dt_sprites_set_scrooge_apart(on);
        dt_sprites_set_objects_apart(on);
        native_present_refresh();
    }
}

/* The objects drawn apart this frame into the atlas (shelves of rows), and
 * their motion into the easing. An object that does not fit is drawn
 * without glide: it is then left in the picture by the next frame. */
static void objects_frame(void)
{
    const DtObject *objs;
    int n = s_apart ? dt_sprites_objects(&objs) : 0;
    int slot[DT_OBJ_SLOTS_WINDOW], ox[DT_OBJ_SLOTS_WINDOW], oy[DT_OBJ_SLOTS_WINDOW];
    bool world[DT_OBJ_SLOTS_WINDOW];
    int count = 0, cx = 0, cy = 0, shelf = 0;
    s_obj_n = 0;
    for (int i = 0; i < n && s_obj_n < DT_OBJ_SLOTS_WINDOW; i++) {
        if (!objs[i].apart) {
            continue;
        }
        static uint32_t tmp[256 * 240];
        int bx, by, bw, bh;
        if (!dt_sprites_object_layer(&objs[i], tmp, 256 * 240, &bx, &by, &bw, &bh)) {
            continue;
        }
        if (cx + bw > ATLAS_W) {
            cx = 0;
            cy += shelf;
            shelf = 0;
        }
        if (cy + bh > ATLAS_H) {
            break;
        }
        for (int r = 0; r < bh; r++) {
            memcpy(&s_atlas[(cy + r) * ATLAS_W + cx], &tmp[r * bw], (size_t)bw * sizeof *tmp);
        }
        s_objs[s_obj_n++] = (ObjLayer){ objs[i].slot, cx, cy, bx, by, bw, bh };
        slot[count] = objs[i].slot;
        world[count] = objs[i].world;
        ox[count] = objs[i].x;
        oy[count] = objs[i].y;
        count++;
        cx += bw;
        shelf = bh > shelf ? bh : shelf;
    }
    smooth_objects(slot, world, ox, oy, count);
    if (s_obj_n && (s_atlas_tex || (s_atlas_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                                                                       SDL_TEXTUREACCESS_STREAMING, ATLAS_W, ATLAS_H)))) {
        SDL_SetTextureBlendMode(s_atlas_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(s_atlas_tex, s_set.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        SDL_Rect used = { 0, 0, ATLAS_W, cy + shelf };
        if (used.h > 0) {
            SDL_UpdateTexture(s_atlas_tex, &used, s_atlas, ATLAS_W * 4);
        }
    }
}

/* Draw the objects back to front, each at its eased place, inside clip. */
static void draw_objects(double phase, bool in_between, int top, int side, const SDL_Rect *clip)
{
    if (!s_obj_n || !s_atlas_tex) {
        return;
    }
    SDL_RenderSetClipRect(s_ren, clip);
    for (int i = s_obj_n - 1; i >= 0; i--) {
        const ObjLayer *o = &s_objs[i];
        double dx = 0, dy = 0;
        smooth_object_shift(o->slot, phase, in_between, &dx, &dy);
        SDL_Rect src = { o->ax, o->ay, o->bw, o->bh };
        SDL_FRect dst = { (float)(o->bx - side + dx), (float)(o->by - top + dy), (float)o->bw, (float)o->bh };
        SDL_RenderCopyF(s_ren, s_atlas_tex, &src, &dst);
    }
    SDL_RenderSetClipRect(s_ren, NULL);
}

static SDL_Texture *texture(SDL_Texture *t, int w, int h, int *tw, int *th, bool blend)
{
    if (t && *tw == w && *th == h) {
        return t;
    }
    if (t) {
        SDL_DestroyTexture(t);
    }
    t = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (t) {
        SDL_SetTextureScaleMode(t, s_set.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        SDL_SetTextureBlendMode(t, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    }
    *tw = w;
    *th = h;
    return t;
}

static bool smooth_layers(const uint32_t *pic, int w, int h, long frame, int *band_out, int *top_out,
                          int *side_out)
{
    AppPresentation p;
    app_present_get(&p);
    int band;
    if (p.pixels != APP_PIXELS_NEAREST || p.aspect != APP_ASPECT_SQUARE || !smooth_active(&band)) {
        return false;
    }
    const int top = p.overscan == APP_OVERSCAN_OFF ? 0 : 8, side = p.overscan == APP_OVERSCAN_ALL ? 8 : 0;
    const int M = SMOOTH_MARGIN, MY = SMOOTH_MARGIN_Y, bp = band - top;
    if (w != 256 - 2 * side || h != 240 - 2 * top || bp <= 0 || bp >= h) {
        return false;
    }
    const int W = w + 2 * M, fh = h - bp, FH = fh + 2 * MY;
    if (frame != s_layers_frame || s_field_band != bp || s_field_w != W || s_field_h != FH) {
        size_t need = (size_t)W * (size_t)FH;
        if (need > s_field_cap) {
            free(s_field_px);
            s_field_px = (uint32_t *)malloc(need * sizeof *s_field_px);
            s_field_cap = s_field_px ? need : 0;
            if (!s_field_px) {
                return false;
            }
        }
        int hw = s_hud_w, hh = s_hud_h;
        s_hud_tex = texture(s_hud_tex, w, bp, &hw, &hh, false);
        s_hud_w = hw;
        s_hud_h = hh;
        int fw = s_field_w, fhh = s_field_h;
        s_field_tex = texture(s_field_tex, W, FH, &fw, &fhh, false);
        s_field_w = fw;
        s_field_h = fhh;
        static int pw, ph;
        s_player_tex = texture(s_player_tex, 256, 240, &pw, &ph, true);
        if (!s_hud_tex || !s_field_tex || !s_player_tex) {
            return false;
        }
        const uint32_t *left, *right, *up, *down;
        smooth_margins(&left, &right, &up, &down);
        int rw, rh;
        const uint32_t *raw = cyc_render_present(&rw, &rh);
        bool on;
        int cam_x, cy, ddx, ddy;
        double sh0, sh1;
        smooth_debug(&on, &cam_x, &cy, &ddx, &ddy, &sh0, &sh1);
        const bool cache = W <= ROW_CACHE_W;
        if (ddy != 0) {
            s_vmove_frame = frame;
            s_vmove_dir = ddy > 0 ? 1 : -1;
        }
        const bool moving = frame - s_vmove_frame <= 4;
        /* Moving down, the rows coming in at the bottom have never been on
         * screen and are still being written: the background colour until
         * they are further in (an old TV's overscan hid them). */
        const uint32_t backdrop = cyc_render_color(0);
        for (int r = -MY; r < fh + MY; r++) {
            uint32_t *row = s_field_px + (size_t)(r + MY) * W;
            const int sy = band + r, content = ((cy + sy) % 240 + 240) % 240;
            if (r < 0 || r >= fh) {
                if (cache && s_row_frame[content] >= frame - 8 && s_row_frame[content] < frame &&
                    s_row_x[content] == cam_x) {
                    memcpy(row, s_rows[content], (size_t)W * sizeof *row);
                    continue;
                }
                const uint32_t *v = r < 0 ? up + (size_t)(sy + MY) * 256 : down + (size_t)(sy - MY) * 256;
                for (int e = 0; e < W; e++) {
                    int c = side - M + e;
                    row[e] = v[c < 0 ? 0 : c > 255 ? 255 : c];
                }
                continue;
            }
            const uint32_t *src = pic + (size_t)(bp + r) * w;
            if (moving && cache && r > 0 && (r <= EDGE_ROWS || r >= fh - EDGE_ROWS) &&
                s_row_frame[content] >= frame - 8 && s_row_frame[content] < frame && s_row_x[content] == cam_x) {
                memcpy(row, s_rows[content], (size_t)W * sizeof *row);
                continue;
            }
            if (frame - s_vmove_frame <= 1 && s_vmove_dir > 0 && r >= fh - EDGE_ROWS) {
                for (int e = 0; e < W; e++) {
                    row[e] = backdrop;
                }
                continue;
            }
            if (r == 0) {
                for (int e = 0; e < W; e++) {
                    row[e] = src[0];
                }
                memcpy(row + M, src, (size_t)w * sizeof *row);
                continue;
            }
            for (int e = 0; e < W; e++) {
                int c = side - M + e;
                uint32_t px;
                if (c < 0) {
                    px = left[sy * 256 + c + M];
                } else if (c >= 256) {
                    px = right[sy * 256 + c - M];
                } else if (c >= side && c < 256 - side) {
                    px = src[c - side];
                } else {
                    px = raw && rw == 256 ? raw[sy * 256 + c] : 0xFF000000u;
                }
                row[e] = px;
            }
            if (cache && r > 0) {
                memcpy(s_rows[content], row, (size_t)W * sizeof *row);
                s_row_frame[content] = frame;
                s_row_x[content] = cam_x;
            }
        }
        SDL_UpdateTexture(s_hud_tex, NULL, pic, w * 4);
        SDL_UpdateTexture(s_field_tex, NULL, s_field_px, W * 4);
        if (s_player_ok) {
            SDL_UpdateTexture(s_player_tex, NULL, s_player_px, 256 * 4);
        }
        s_field_band = bp;
        s_layers_frame = frame;
    }
    *band_out = bp;
    *top_out = top;
    *side_out = side;
    return true;
}

static void show_phase(double phase, bool in_between, long frame);

/* The frame's layers, built before its first picture is due so that every
 * picture is only drawn (CYC_PACING_LOG "prep"). */
static double s_prep_ms;

static void smooth_prepare(long frame)
{
    const Uint64 t0 = SDL_GetPerformanceCounter();
    int w, h, band, top, side;
    const uint32_t *pic = app_ui_present(&w, &h);
    if (!pic || w <= 0 || h <= 0) {
        pic = cyc_render_present(&w, &h);
    }
    s_layers_ok = pic && smooth_layers(pic, w, h, frame, &band, &top, &side);
    if (s_layers_ok) {
        s_lay_w = w;
        s_lay_h = h;
        s_lay_band = band;
        s_lay_top = top;
        s_lay_side = side;
    }
    s_prep_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void show(void)
{
    show_phase(0, false, -1);
}

static void draw_layers(double phase, bool in_between, int w, int h, int band, int top, int side);

static void draw_layers(double phase, bool in_between, int w, int h, int band, int top, int side)
{
    const int M = SMOOTH_MARGIN, MY = SMOOTH_MARGIN_Y;
    double sx, sy;
    smooth_shift(phase, in_between, &sx, &sy);
    sx = sx > M - 1 ? M - 1 : sx < 1 - M ? 1 - M : sx;
    sy = sy > MY - 1 ? MY - 1 : sy < 1 - MY ? 1 - MY : sy;
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    apply_integer_scale();
    SDL_Rect hud = { 0, 0, w, band };
    SDL_RenderCopy(s_ren, s_hud_tex, &hud, &hud);
    SDL_Rect clip = { 0, band, w, h - band };
    SDL_RenderSetClipRect(s_ren, &clip);
    SDL_FRect field = { (float)(sx - M), (float)(band - MY + sy), (float)s_field_w, (float)s_field_h };
    SDL_RenderCopyF(s_ren, s_field_tex, NULL, &field);
    SDL_RenderSetClipRect(s_ren, NULL);
    draw_objects(phase, in_between, top, side, &clip);
    double px, py;
    if (s_player_ok && smooth_scrooge_shift(phase, in_between, &px, &py)) {
        SDL_Rect all = { 0, 0, w, h };
        SDL_RenderSetClipRect(s_ren, &all);
        SDL_FRect player = { (float)(px - side), (float)(py - top), 256.0f, 240.0f };
        SDL_RenderCopyF(s_ren, s_player_tex, NULL, &player);
        SDL_RenderSetClipRect(s_ren, NULL);
    }
}

static void show_phase(double phase, bool in_between, long frame)
{
    if (frame >= 0 && frame == s_layers_frame && s_layers_ok && s_note_until <= SDL_GetTicks64() &&
        ensure_texture(s_lay_w, s_lay_h)) {
        draw_layers(phase, in_between, s_lay_w, s_lay_h, s_lay_band, s_lay_top, s_lay_side);
        return;
    }
    int ow = 0, oh = 0, w, h;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    const uint32_t *pic = app_ui_present(&w, &h);
    if (!pic || w <= 0 || h <= 0) {
        pic = cyc_render_present(&w, &h);
    }
    if (!ensure_texture(w, h)) {
        return;
    }
    if (s_note_until > SDL_GetTicks64()) {
        static uint32_t *copy;
        static size_t copy_cap;
        size_t need = (size_t)w * (size_t)h;
        if (need > copy_cap) {
            free(copy);
            copy = (uint32_t *)malloc(need * sizeof *copy);
            copy_cap = copy ? need : 0;
        }
        if (copy) {
            memcpy(copy, pic, need * sizeof *copy);
            cyc_overlay_toast(copy, w, h, s_note_title, s_note_body);
            pic = copy;
        }
    }
    int band, top, side;
    if (frame >= 0 && smooth_layers(pic, w, h, frame, &band, &top, &side)) {
        draw_layers(phase, in_between, w, h, band, top, side);
        return;
    }
    if (s_apart) {
        static uint32_t *with;
        static size_t with_cap;
        AppPresentation pp;
        app_present_get(&pp);
        const int top0 = pp.overscan == APP_OVERSCAN_OFF ? 0 : 8, side0 = pp.overscan == APP_OVERSCAN_ALL ? 8 : 0;
        size_t need = (size_t)w * (size_t)h;
        if (need > with_cap) {
            free(with);
            with = (uint32_t *)malloc(need * sizeof *with);
            with_cap = with ? need : 0;
        }
        if (with && w == 256 - 2 * side0 && h == 240 - 2 * top0) {
            memcpy(with, pic, need * sizeof *with);
            for (int i = s_obj_n - 1; i >= 0; i--) {
                const ObjLayer *o = &s_objs[i];
                for (int r = 0; r < o->bh; r++) {
                    for (int c = 0; c < o->bw; c++) {
                        int x = o->bx + c - side0, y = o->by + r - top0;
                        uint32_t px = s_atlas[(o->ay + r) * ATLAS_W + o->ax + c];
                        if ((px >> 24) && x >= 0 && x < w && y >= 0 && y < h) {
                            with[y * w + x] = px;
                        }
                    }
                }
            }
            for (int y = 0; s_player_ok && y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint32_t c = s_player_px[(y + top0) * 256 + x + side0];
                    if (c >> 24) {
                        with[y * w + x] = c;
                    }
                }
            }
            pic = with;
        }
    }
    SDL_UpdateTexture(s_tex, NULL, pic, w * 4);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    apply_integer_scale();
    SDL_Rect dst = { 0, 0, w, h };
    SDL_RenderCopy(s_ren, s_tex, NULL, &dst);
}

static void present_shot(const char *base, long frame)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!px) {
        return;
    }
    if (SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0) {
        char path[1100];
        numbered_path(path, sizeof path, base, frame);
        if (!cyc_write_png(path, px, w, h)) {
            fprintf(stderr, "cannot write %s\n", path);
        }
    }
    free(px);
}

#ifdef _WIN32
static HANDLE s_mmcss;

/* Windows lowers an unfocused process: power throttling (EcoQoS, E-cores on
 * hybrid CPUs), coarser timers and no foreground boost. The frame loop opts
 * out so frames stay on time and the audio queue fed. */
static void scheduling_begin(void)
{
    PROCESS_POWER_THROTTLING_STATE state = { 0 };
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
    timeBeginPeriod(1);
    DWORD task = 0;
    s_mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
}

static void scheduling_end(void)
{
    if (s_mmcss) {
        AvRevertMmThreadCharacteristics(s_mmcss);
    }
    s_mmcss = NULL;
    timeEndPeriod(1);
}
#else
static void scheduling_begin(void)
{
}

static void scheduling_end(void)
{
}
#endif

/* Stretches pcm[0..n) over m >= n samples by linear interpolation. */
static void stretch(int16_t *pcm, size_t n, size_t m)
{
    if (n < 2 || m <= n) {
        return;
    }
    for (size_t i = m; i-- > 0;) {
        double x = (double)i * (double)(n - 1) / (double)(m - 1);
        size_t j = (size_t)x;
        double frac = x - (double)j;
        int16_t a = pcm[j], b = pcm[j + 1 < n ? j + 1 : j];
        pcm[i] = (int16_t)(a + (b - a) * frac);
    }
}

static SDL_AudioDeviceID s_dev;
static SDL_AudioSpec s_have;

static void queue_audio(bool fast)
{
    int16_t pcm[4096];
    size_t n = frame_sound(pcm, sizeof pcm / sizeof pcm[0], s_dev ? s_have.freq : AUDIO_RATE);
    size_t queued = s_dev ? SDL_GetQueuedAudioSize(s_dev) / sizeof pcm[0] : 0;
    if (!s_dev || !n || fast || !s_set.audio_enabled || queued >= (size_t)s_have.freq / 10) {
        return;
    }
    size_t low = (size_t)s_have.samples + n;
    if (queued < low) {
        size_t m = n + (low - queued);
        if (m > 2 * n) {
            m = 2 * n;
        }
        stretch(pcm, n, m);
        n = m;
    }
    if (s_set.volume < 100) {
        for (size_t i = 0; i < n; i++) {
            pcm[i] = (int16_t)(pcm[i] * s_set.volume / 100);
        }
    }
    app_ui_audio(pcm, n);
    SDL_QueueAudio(s_dev, pcm, (Uint32)(n * sizeof pcm[0]));
}

static int menu_tick(void)
{
    static Uint64 next;
    const Uint64 freq = SDL_GetPerformanceFrequency(), frame_ticks = (Uint64)((double)freq / FRAME_HZ);
    const Uint64 now = SDL_GetPerformanceCounter();
    if (next == 0 || now > next + 4 * frame_ticks) {
        next = now;
    }
    int ran = 0;
    scrooge_apart(false);
    s_player_ok = false;
    while (next <= now) {
        run_frame(0, 0);
        queue_audio(false);
        next += frame_ticks;
        ran++;
    }
    return ran;
}

/* ---- the window loop's two halves (run_window) ---- */

typedef struct {
    long frame;
    bool unpaced;
    int rate;
    Uint64 frame_ticks, t_emu0, t_emu1, t_audio;
    AppPresentation pres;
} WorkResult;

typedef struct {
    bool running, pace_5994, uncapped, was_slow;
    bool prev[CYC_SC_COUNT];
    int shot;
    long frames_done;
    Uint64 freq;
    WorkResult out;
} Work;

/* Events, input and one game frame with its sound. */
static void work(Work *w)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) &&
            ((ev.key.keysym.mod & KMOD_ALT) || s_fullscreen_chord) &&
            (ev.key.keysym.scancode == SDL_SCANCODE_RETURN || ev.key.keysym.scancode == SDL_SCANCODE_KP_ENTER)) {
            continue;
        }
        app_ui_event(&ev);
        if (ev.type == SDL_QUIT) {
            w->running = false;
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_pads();
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F8) {
            save_state();
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F9) {
            load_state();
            if (s_dev) {
                SDL_ClearQueuedAudio(s_dev);
            }
        }
    }
    if (!w->running) {
        return;
    }
    CycInputFrame in;
    CycInputState st;
    read_frame(&in);
    cyc_input_eval(&s_set.bind, &in, &st);
    bool edge[CYC_SC_COUNT];
    for (int i = 0; i < CYC_SC_COUNT; i++) {
        edge[i] = st.shortcut[i] && !w->prev[i];
        w->prev[i] = st.shortcut[i];
    }
    if (edge[CYC_SC_FULLSCREEN]) {
        s_set.fullscreen = s_set.fullscreen ? 0 : 1;
        apply_fullscreen();
        save_settings();
    }
    if (edge[CYC_SC_SCREENSHOT]) {
        int pw, ph;
        const uint32_t *pic = picture(&pw, &ph);
        char name[64];
        snprintf(name, sizeof name, "cyc_shot_%04d.png", w->shot++);
        if (cyc_write_png(name, pic, pw, ph)) {
            printf("saved %s\n", name);
        }
    }
    uint8_t buttons[2] = { st.buttons[0], st.buttons[1] };
    app_ui_input(buttons);
    /* Pictures a second ([Game] PicturesPerSecond). At 240 the console
     * runs at 59.94 frames a second, so 4 pictures a frame come at
     * 239.76 Hz, under a 240 Hz display (4 x 60.0988 = 240.4 is over
     * it); the synth makes 0.26% more samples a frame to match.
     * CYC_PACE_5994=0 keeps the console's own rate. */
    WorkResult *r = &w->out;
    const bool fast = st.shortcut[CYC_SC_FAST_FORWARD];
    r->unpaced = fast || w->uncapped;
    app_present_get(&r->pres);
    r->rate = r->unpaced ? 60 : app_present_rate_hz(r->pres.rate);
    const bool slow = r->rate >= 240 && w->pace_5994;
    r->frame_ticks = (Uint64)((double)w->freq / (slow ? 59.94 : FRAME_HZ));
    native_audio_set_speed(slow ? FRAME_HZ / 59.94 : 1.0);
    if (slow != w->was_slow) {
        printf("[ducktales] window: console paced at %.4f frames a second\n", slow ? 59.94 : FRAME_HZ);
        fflush(stdout);
        w->was_slow = slow;
    }
    app_ui_frame_begin();
    r->t_emu0 = SDL_GetPerformanceCounter();
    run_frame(buttons[0], buttons[1]);
    r->frame = ++w->frames_done;
    r->t_emu1 = SDL_GetPerformanceCounter();
    queue_audio(fast);
    r->t_audio = SDL_GetPerformanceCounter();
}

/* After the frame's work and before its first picture: Scrooge's layer, the
 * easing and the layers the pictures are drawn from. */
static void frame_finish(const WorkResult *r)
{
    const double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
    const Uint64 t0 = SDL_GetPerformanceCounter();
    smooth_set_level(r->pres.smooth);
    scrooge_apart(r->pres.smooth != APP_SMOOTH_OFF && r->pres.pixels == APP_PIXELS_NEAREST &&
                  r->pres.aspect == APP_ASPECT_SQUARE);
    const Uint64 t1 = SDL_GetPerformanceCounter();
    s_player_ok = s_apart && dt_sprites_scrooge_layer(s_player_px, &s_player_x, &s_player_y);
    const Uint64 t2 = SDL_GetPerformanceCounter();
    smooth_frame(s_player_ok, s_player_x, s_player_y);
    objects_frame();
    const Uint64 t3 = SDL_GetPerformanceCounter();
    smooth_prepare(r->frame);
    const Uint64 t4 = SDL_GetPerformanceCounter();
    s_fin_ms[0] = (double)(t1 - t0) * ms;
    s_fin_ms[1] = (double)(t2 - t1) * ms;
    s_fin_ms[2] = (double)(t3 - t2) * ms;
    s_fin_ms[3] = (double)(t4 - t0) * ms;
}

static int run_window(bool hidden, bool uncapped, long exit_after, const char *present_out, long present_every)
{
    if (hidden) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *title = cyc_native_display_name ? cyc_native_display_name : "DuckTales";
    int w, h;
    window_size(0, &w, &h);
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0));
    s_ren = s_win ? SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (s_win && !s_ren) {
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) {
        char message[600];
        snprintf(message, sizeof message, "Could not create the game window or renderer: %s", SDL_GetError());
        fprintf(stderr, "%s\n", message);
        if (!hidden) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cannot start game", message, NULL);
        }
        SDL_Quit();
        return 1;
    }
    set_vsync(s_set.vsync && !uncapped);
    apply_fullscreen();

    SDL_AudioSpec want = { 0 };
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &s_have, 0);
    s_dev = dev;
    native_audio_reset(dev ? s_have.freq : AUDIO_RATE);
    app_ui_set_menu_tick(menu_tick);
    if (dev) {
        SDL_PauseAudioDevice(dev, 0);
    } else {
        fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());
    }
    open_pads();
    scheduling_begin();
    pace_open();

    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next = SDL_GetPerformanceCounter(), shown = 0;
    const char *pace_env = getenv("CYC_PACE_5994");
    Work lw = { 0 };
    lw.running = true;
    lw.pace_5994 = !(pace_env && pace_env[0] == '0');
    lw.uncapped = uncapped;
    lw.freq = freq;
    long loops = 0;
    /* A frame's work (input, the game, its sound) and its finish (Scrooge's
     * layer, the easing, the layers) are apart: with more than one picture a
     * frame the next frame's work runs in the gap before this frame's last
     * picture, its finish after it, so each has a whole picture's time
     * (pacing240s3.log: 4.8 ms together, 4.17 between pictures). The
     * buttons are read one picture earlier for it. */
    work(&lw);
    while (lw.running) {
        WorkResult cur = lw.out;
        frame_finish(&cur);
        const long frame = cur.frame;
        bool next_done = false;
        set_vsync(s_set.vsync && !cur.unpaced && cur.rate <= 60);
        Uint64 now = SDL_GetPerformanceCounter();
        if (!cur.unpaced || now - shown >= cur.frame_ticks) {
            /* The frame's pictures, each held to its even slot of the
             * frame: next, next + step, ... An in-between picture whose slot
             * has passed by more than half a step is skipped (a late frame
             * catches up instead of pushing every later picture back). */
            const int count = cur.rate / 60;
            const Uint64 step = cur.frame_ticks / (Uint64)count;
            const double ms = 1000.0 / (double)freq;
            s_pace_step_ms = (double)step * ms;
            for (int i = 0; i < count; i++) {
                const Uint64 due = next + (Uint64)i * step;
                if (count > 1 && i == count - 1 && !next_done) {
                    const Uint64 a0 = SDL_GetPerformanceCounter();
                    work(&lw);
                    s_work_at[0] = (double)((long long)(a0 - (next + cur.frame_ticks))) * 1000.0 / (double)freq;
                    s_work_at[1] = (double)((long long)(SDL_GetPerformanceCounter() - (next + cur.frame_ticks))) *
                                   1000.0 / (double)freq;
                    next_done = true;
                    if (!lw.running) {
                        break;
                    }
                }
                now = SDL_GetPerformanceCounter();
                if (i > 0 && now > due + step / 2) {
                    pace_skip(frame, count - i);
                    break;
                }
                if (!cur.unpaced && now < due) {
                    Uint32 wait_ms = (Uint32)((due - now) * 1000 / freq);
                    if (wait_ms > 1) {
                        SDL_Delay(wait_ms - 1);
                    }
                    while (SDL_GetPerformanceCounter() < due) {
                    }
                }
                const Uint64 t_draw0 = SDL_GetPerformanceCounter();
                show_phase((double)i / count, count > 1, frame);
                if (i == 0 && present_out && present_every > 0 && loops % present_every == 0) {
                    present_shot(present_out, frame);
                }
                const Uint64 t_draw1 = SDL_GetPerformanceCounter();
                SDL_RenderPresent(s_ren);
                const Uint64 prev_shown = shown;
                shown = SDL_GetPerformanceCounter();
                if (s_pace && !cur.unpaced) {
                    if (!s_pace_first && frame > 120) {
                        s_pace_first = t_draw1;
                    }
                    s_pace_last = t_draw1;
                    if (i == 0) {
                        pace_line(frame, (double)(cur.t_emu1 - cur.t_emu0) * ms, (double)(cur.t_audio - cur.t_emu1) * ms,
                                  (double)(t_draw0 - cur.t_audio) * ms, (double)(t_draw1 - t_draw0) * ms,
                                  (double)(shown - t_draw1) * ms, (double)((long long)(t_draw1 - next)) * ms,
                                  prev_shown ? (double)(t_draw1 - s_pace_prev) * ms : 0.0, s_vsync_active);
                    } else {
                        pace_picture(frame, i, (double)((long long)(t_draw1 - next)) * ms,
                                     (double)(t_draw1 - s_pace_prev) * ms, (double)(t_draw1 - t_draw0) * ms);
                    }
                }
                s_pace_prev = t_draw1;
            }
        }
        loops++;
        next += cur.frame_ticks;
        now = SDL_GetPerformanceCounter();
        if (cur.unpaced || now > next + 4 * cur.frame_ticks) {
            next = now;
        }
        if (exit_after > 0 && frame >= exit_after) {
            lw.running = false;
        }
        if (lw.running && !next_done) {
            const Uint64 a0 = SDL_GetPerformanceCounter();
            work(&lw);
            s_work_at[0] = (double)((long long)(a0 - next)) * 1000.0 / (double)freq;
            s_work_at[1] = (double)((long long)(SDL_GetPerformanceCounter() - next)) * 1000.0 / (double)freq;
        }
    }
    pace_close();
    scheduling_end();
    app_ui_set_menu_tick(NULL);
    save_settings();
    if (dev) {
        SDL_CloseAudioDevice(dev);
        s_dev = 0;
    }
    if (s_tex) {
        SDL_DestroyTexture(s_tex);
    }
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}

static bool game_option_name(const char *name)
{
    for (int e = 0; e < DT_ENH_COUNT; e++) {
        if (!strcmp(name, dt_enh_info((DtEnh)e)->option)) {
            return true;
        }
    }
    return !strcmp(name, DT_SPRITES_OPTION);
}

static int usage(const char *self)
{
    fprintf(stderr,
            "usage: %s ROM [--hidden] [--exit-after N] [--config FILE] [--uncapped]\n"
            "       [--present-out FILE [--present-every N]]\n"
            "       [--frames N [--input ROUTE] [--lag none|faithful] [--state-check FRAME] [--wav-out FILE]]\n"
            "       [--jump-scale N] [--no-damage on|off] [--infinite-time on|off] [--infinite-lives on|off]\n"
            "       [--start-lives 0-9|off] [--no-sprite-flicker on|off]\n"
            "       [--music-under-effects on|off] [--overlap-effects on|off]\n"
            "       [--mods on|off] [--mods-dir DIR] [--mods-order ID:on|off,...] [--mods-dump DIR]\n",
            self);
    return 2;
}

int ducktales_host_main(int argc, char **argv)
{
    const char *rom = NULL, *present_out = NULL, *input = NULL, *config = NULL, *wav_out = NULL;
    long exit_after = -1, present_every = 1, frames = -1, state_check = -1;
    int lag = -1;
    bool hidden = false, uncapped = false, mod_ok = false;

    struct {
        const char *name, *value;
    } game[16];

    int game_n = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--hidden")) {
            hidden = true;
        } else if (!strcmp(a, "--uncapped")) {
            uncapped = true;
        } else if (!strcmp(a, "--window-input") || !strcmp(a, "--virtual-pad")) {
            continue;
        } else if (!strcmp(a, "--exit-after") && v) {
            exit_after = atol(argv[++i]);
        } else if (!strcmp(a, "--config") && v) {
            config = argv[++i];
        } else if (!strcmp(a, "--present-out") && v) {
            present_out = argv[++i];
        } else if (!strcmp(a, "--present-every") && v) {
            present_every = atol(argv[++i]);
        } else if (!strcmp(a, "--frames") && v) {
            frames = atol(argv[++i]);
        } else if (!strcmp(a, "--input") && v) {
            input = argv[++i];
        } else if (!strcmp(a, "--state-check") && v) {
            state_check = atol(argv[++i]);
        } else if (!strcmp(a, "--wav-out") && v) {
            wav_out = argv[++i];
        } else if (!strncmp(a, "--mods", 6) && v && app_mods_option(a, v, &mod_ok)) {
            i++;
            if (!mod_ok) {
                fprintf(stderr, "%s %s: invalid value\n", a, v);
                return 2;
            }
        } else if ((!strcmp(a, "--music-under-effects") || !strcmp(a, "--overlap-effects")) && v &&
                   (!strcmp(v, "on") || !strcmp(v, "off"))) {
            s_voice_override[a[2] == 'o'] = !strcmp(argv[++i], "on");
        } else if (!strcmp(a, "--lag") && v && !strcmp(v, "none")) {
            lag = NATIVE_LAG_NONE, i++;
        } else if (!strcmp(a, "--lag") && v && !strcmp(v, "faithful")) {
            lag = NATIVE_LAG_FAITHFUL, i++;
        } else if (a[0] == '-' && v && game_n < 16 && game_option_name(a)) {
            game[game_n].name = a, game[game_n++].value = argv[++i];
        } else if (a[0] != '-' && !rom) {
            rom = a;
        } else {
            return usage(argv[0]);
        }
    }
    if (!rom) {
        return usage(argv[0]);
    }
    size_t size;
    uint8_t *image = read_file(rom, &size);
    char err[160];
    if (!image) {
        fprintf(stderr, "cannot read %s\n", rom);
        return 2;
    }
    if (!native_load_ines(image, size, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", rom, err);
        free(image);
        return 2;
    }
    bool off_screen = frames >= 0;
    if (!off_screen) {
        app_mods_use_player_folders(config ? config : cyc_settings_default_path());
    }
    size_t prg_at = 16 + ((image[6] & 4) ? 512 : 0), prg_size = (size_t)image[4] * 0x4000;
    app_mods_start(prg_at + prg_size <= size ? image + prg_at : NULL, prg_at + prg_size <= size ? prg_size : 0);
    free(image);
    if (!native_hooks_init()) {
        return 2;
    }
    if (!off_screen) {
        snprintf(s_set_path, sizeof s_set_path, "%s", config ? config : cyc_settings_default_path());
        static const CycSettingsGame GAME = { NULL, game_load, game_save };
        cyc_settings_default(&s_set);
        cyc_settings_load(&s_set, s_set_path, stderr, &GAME);
        ducktales_host_load_audio_settings(s_set_path);
        s_set.bind.shortcut_off |= 1u << CYC_SC_DISK;
    }
    for (int g = 0; g < game_n; g++) {
        bool ok = false;
        ducktales_host_game_option(game[g].name, game[g].value, &ok);
        if (!ok) {
            fprintf(stderr, "%s %s: invalid value\n", game[g].name, game[g].value);
            return 2;
        }
    }
    dt_sprites_mods_changed();
    if (off_screen) {
        return run_off_screen(frames, input, lag, present_out, present_every, state_check, wav_out);
    }
    power_on();
    return run_window(hidden, uncapped, exit_after, present_out, present_every);
}
