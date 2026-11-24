/*
 * native_audio.h - the native runtime's synth: the frame's APU register
 * writes (NativeFrame.apu_writes, native_state.h) in, the frame's samples
 * out (docs/audio.md, "Native synth").
 *
 * It keeps the four tone channels, the DMC's output level and the frame
 * sequencer as the program set them and renders each sample as the average
 * of the channels' outputs over that sample's span of CPU time, computed in
 * closed form between register writes and sequencer clocks. Nothing is
 * stepped per CPU cycle. The console's nonlinear mixer and the NES output
 * stage (90 Hz and 440 Hz high-pass, 14 kHz low-pass) follow, as in the
 * cycle host's APU. Nothing here is read back by the game.
 *
 * Owns no setting; the sample rate and the extra voices are the caller's.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "native/native_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for one frame at up to 96 kHz. */
#define NATIVE_AUDIO_MAX_FRAME_SAMPLES 2048

/* Power-on: every register 0, silence, samples at sample_rate Hz. */
void native_audio_reset(int sample_rate);
/* Samples per console second, as a factor of the sample rate (1.0 normally).
 * The window runs the console at 59.94 frames a second at 240 pictures, so
 * it asks for 60.0988 / 59.94 times as many samples per frame: the sound
 * stays in step with the slower frames, 0.26% lower in pitch. Kept across
 * native_audio_reset. */
void native_audio_set_speed(double factor);

/* The native player's extra voices (native_voices.h, docs/audio.md "Extra
 * voices"), both off until set; they read the driver's RAM and the ROM after
 * each frame and are mixed beside the console's channels. With both off the
 * output is the console's channels alone, sample for sample as before.
 * Kept across native_audio_reset. */
void native_audio_voices(bool music_under_effects, bool overlap_effects);

/* Which channels reach the mix: bit 0 pulse 1, 1 pulse 2, 2 triangle, 3
 * noise, the extra voices' copies included; all four by default. For
 * measurements (ducktales_native --audio-channels). */
void native_audio_channels(unsigned mask);

/* Plays one machine frame (native_last_frame()): its apu_writes[] at their
 * cycle offsets over its cycles. A frame the no-slowdown mode stretched
 * (held_cycles > 0) is played in cycles - held_cycles, its offsets scaled to
 * match, since it is still presented for one 60 Hz period. Writes the frame's
 * signed 16-bit mono samples to out (at most max) and returns how many:
 * sample_rate / 60.0988 per frame on average. */
size_t native_audio_frame(const NativeFrame *frame, int16_t *out, size_t max);

/* A 44-byte mono 16-bit PCM WAV header at the start of f, for --wav-out:
 * written first with 0 samples, again with the count when done. */
void native_audio_wav_header(FILE *f, int sample_rate, uint32_t samples);

/* Channels left out of the mix from now on (bit 0 pulse 1, 1 pulse 2, 2
 * triangle, 3 noise), the extra voices' copies included; they keep running,
 * only their sound is dropped (mods that play a file in place of them). */
void native_audio_set_mute(uint8_t mask);

#ifdef __cplusplus
}
#endif
