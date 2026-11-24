/*
 * native_audio.c - the native runtime's synth (native_audio.h,
 * docs/audio.md "Native synth"). A frame is played as a run of spans between
 * events (a register write at its cycle offset, a frame-sequencer clock);
 * within a span every channel's parameters are fixed, so its contribution to
 * a sample is the integral of a periodic step sequence, taken in closed form:
 *
 *   pulse      8-step duty sequence, one step per 2 x (period + 1) cycles;
 *              silent below period 8 or when the sweep target overflows
 *   triangle   32-step sequence, one step per period + 1 cycles while the
 *              length and linear counters are non-zero; below period 2 it
 *              keeps stepping (ultrasonic) while its output holds
 *
 * Each sequencer keeps its step and the time left until its timer next
 * expires, so a period change takes effect at the next expiry, as on the
 * console, and the waveforms stay in phase with the APU's.
 *   noise      15-bit LFSR, one shift per table period (the only loop: at
 *              most one iteration per LFSR shift, never per cycle)
 *   DMC        the $4011 level only; the ROM never starts a sample
 *              (docs/audio.md, "DMC")
 *
 * Envelopes, the linear counter, length counters and sweeps are clocked by
 * the 4- or 5-step frame sequencer at its NTSC cycle positions, carried
 * across frames. Register side effects follow the 2A03: $4003/$4007 restart
 * the duty sequence and the envelope, $400B sets the linear counter's reload
 * flag, $400F restarts the noise envelope, $4015 clears disabled length
 * counters, $4017 restarts the sequencer.
 *
 * The extra voices (native_voices.h) get APUs of their own: the music under
 * an effect in apu[1], the second effect in apu[2]. Their register writes are
 * played where the driver's update starts (the frame's $4015 write), only
 * the channels a voice holds are heard, and their outputs join the console's
 * in the same mixer. With both voices off apu[0] is all there is.
 *
 * native_audio_set_mute drops channels from the mix only (mods that play a
 * file in place of the driver's channels): every channel keeps running.
 */
#include "native/native_audio.h"

#include "native/native_runtime.h"
#include "native/native_voices.h"

#include <string.h>

#define NA_CPU_HZ (21477272.0 / 12.0)

static const uint8_t length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint16_t noise_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint8_t duty_bits[4] = { 0x40, 0x60, 0x78, 0x9F };
static const uint16_t seq_points[2][4] = { { 7457, 14913, 22371, 29829 }, { 7457, 14913, 22371, 37281 } };
static const uint16_t seq_period[2] = { 29830, 37282 };

typedef struct {
    uint8_t start, divider, decay;
} NaEnvelope;

typedef struct {
    double sum[4];
    uint8_t reg[16], len[4], enabled;
    uint16_t pulse_period[2];
    uint8_t pulse_step[2];
    double pulse_wait[2];
    uint8_t sweep_divider[2], sweep_reload[2];
    NaEnvelope env[3];
    uint16_t tri_period;
    uint8_t tri_step, tri_out;
    double tri_wait;
    uint8_t tri_linear, tri_reload;
    uint16_t noise_lfsr;
    double noise_wait;
    uint8_t dmc_level;
} NaApu;

static struct {
    double cps, fill;
    double hp90_a, hp440_a, lp_a, hp90_in, hp90_out, hp440_in, hp440_out, lp_out;
    NaApu apu[3];
    int apus;        /* 1, or 3 while an extra voice is on */
    uint8_t live[3]; /* channels of each APU that are heard */
    double seq_pos;
    uint8_t five_step, seq_next;
    uint8_t tri_levels[32];
    uint16_t tri_prefix[33];
} s;

static bool s_music_under_effects, s_overlap_effects;
static unsigned s_channels = 0x0F;

static void apu_power_on(NaApu *a)
{
    memset(a, 0, sizeof *a);
    a->noise_lfsr = 1;
    a->noise_wait = noise_periods[0];
    a->pulse_wait[0] = a->pulse_wait[1] = 2;
    a->tri_wait = 1;
    a->tri_out = 15;
}

static int s_rate = 48000;
static double s_speed = 1.0;

void native_audio_set_speed(double factor)
{
    s_speed = factor > 0.5 && factor < 2.0 ? factor : 1.0;
    s.cps = NA_CPU_HZ / (s_rate * s_speed);
}

void native_audio_reset(int sample_rate)
{
    memset(&s, 0, sizeof s);
    s_rate = sample_rate > 0 ? sample_rate : 48000;
    s.cps = NA_CPU_HZ / (s_rate * s_speed);
    double dt = 1.0 / sample_rate, pi2 = 2 * 3.14159265358979;
    double rc90 = 1.0 / (pi2 * 90), rc440 = 1.0 / (pi2 * 440), rc14k = 1.0 / (pi2 * 14000);
    s.hp90_a = rc90 / (rc90 + dt);
    s.hp440_a = rc440 / (rc440 + dt);
    s.lp_a = dt / (rc14k + dt);
    apu_power_on(&s.apu[0]);
    s.apus = 1;
    s.live[0] = 0x0F;
    native_voices_reset();
    for (int k = 0; k < 32; k++) {
        s.tri_levels[k] = (uint8_t)(k < 16 ? 15 - k : k - 16);
        s.tri_prefix[k + 1] = (uint16_t)(s.tri_prefix[k] + s.tri_levels[k]);
    }
}

void native_audio_voices(bool music_under_effects, bool overlap_effects)
{
    s_music_under_effects = music_under_effects;
    s_overlap_effects = overlap_effects;
}

void native_audio_channels(unsigned mask)
{
    s_channels = mask & 0x0F;
}

static int sweep_target(const NaApu *a, int ch)
{
    uint16_t period = a->pulse_period[ch];
    uint8_t r = a->reg[1 + 4 * ch];
    int change = period >> (r & 7);
    if (r & 0x08) {
        int target = period - change - (ch == 0 ? 1 : 0);
        return target < 0 ? 0 : target;
    }
    return period + change;
}

static uint8_t envelope_volume(const NaEnvelope *e, uint8_t reg)
{
    return (reg & 0x10) ? (reg & 15) : e->decay;
}

static void clock_envelope(NaEnvelope *e, uint8_t reg)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = reg & 15;
    } else if (e->divider == 0) {
        e->divider = reg & 15;
        if (e->decay) {
            e->decay--;
        } else if (reg & 0x20) {
            e->decay = 15;
        }
    } else {
        e->divider--;
    }
}

static void quarter_frame(void)
{
    for (int i = 0; i < s.apus; i++) {
        NaApu *a = &s.apu[i];
        clock_envelope(&a->env[0], a->reg[0]);
        clock_envelope(&a->env[1], a->reg[4]);
        clock_envelope(&a->env[2], a->reg[12]);
        if (a->tri_reload) {
            a->tri_linear = a->reg[8] & 0x7F;
        } else if (a->tri_linear) {
            a->tri_linear--;
        }
        if (!(a->reg[8] & 0x80)) {
            a->tri_reload = 0;
        }
    }
}

static void half_frame(void)
{
    static const uint8_t halt_bit[4] = { 0x20, 0x20, 0x80, 0x20 };
    static const uint8_t halt_reg[4] = { 0, 4, 8, 12 };
    for (int i = 0; i < s.apus; i++) {
        NaApu *a = &s.apu[i];
        for (int ch = 0; ch < 4; ch++) {
            if (a->len[ch] && !(a->reg[halt_reg[ch]] & halt_bit[ch])) {
                a->len[ch]--;
            }
        }
        for (int ch = 0; ch < 2; ch++) {
            uint8_t r = a->reg[1 + 4 * ch];
            int target = sweep_target(a, ch);
            if (a->sweep_divider[ch] == 0 && (r & 0x80) && (r & 7) && a->pulse_period[ch] >= 8 && target <= 0x7FF) {
                a->pulse_period[ch] = (uint16_t)target;
            }
            if (a->sweep_divider[ch] == 0 || a->sweep_reload[ch]) {
                a->sweep_divider[ch] = (r >> 4) & 7;
                a->sweep_reload[ch] = 0;
            } else {
                a->sweep_divider[ch]--;
            }
        }
    }
}

static double cycle_sum(const uint8_t *levels, const uint16_t *prefix, unsigned n, unsigned from,
                        unsigned long long steps)
{
    unsigned long long to = from + steps;
    return (double)(to / n) * prefix[n] + prefix[to % n] - prefix[from];
}

static double sequence(const uint8_t *levels, const uint16_t *prefix, unsigned n, uint8_t *step, double *wait,
                       double length, int gated, double span)
{
    if (span < *wait) {
        *wait -= span;
        return levels[*step] * span;
    }
    double sum = levels[*step] * *wait;
    span -= *wait;
    unsigned first = gated ? (*step + 1u) % n : *step;
    unsigned long long whole = (unsigned long long)(span / length);
    double rest = span - (double)whole * length;
    if (gated) {
        sum += cycle_sum(levels, prefix, n, first, whole) * length;
        *step = (uint8_t)((first + whole) % n);
    } else {
        sum += levels[first] * (double)whole * length;
        *step = (uint8_t)first;
    }
    sum += levels[*step] * rest;
    *wait = length - rest;
    return sum;
}

static void accumulate_apu(NaApu *a, uint8_t live, double span)
{
    for (int ch = 0; ch < 2; ch++) {
        if (!(live >> ch & 1)) {
            continue;
        }
        uint8_t r = a->reg[4 * ch], levels[8];
        uint8_t volume = (a->len[ch] && a->pulse_period[ch] >= 8 && sweep_target(a, ch) <= 0x7FF)
                             ? envelope_volume(&a->env[ch], r)
                             : 0;
        for (int k = 0; k < 8; k++) {
            levels[k] = (uint8_t)(((duty_bits[r >> 6] >> (7 - k)) & 1) * volume);
        }
        uint16_t prefix[9] = { 0 };
        for (int k = 0; k < 8; k++) {
            prefix[k + 1] = (uint16_t)(prefix[k] + levels[k]);
        }
        a->sum[ch] += sequence(levels, prefix, 8, &a->pulse_step[ch], &a->pulse_wait[ch],
                               2.0 * (a->pulse_period[ch] + 1), 1, span);
    }
    if (live & 4) {
        double tri = sequence(s.tri_levels, s.tri_prefix, 32, &a->tri_step, &a->tri_wait, a->tri_period + 1.0,
                              a->len[2] && a->tri_linear, span);
        if (a->tri_period >= 2) {
            a->sum[2] += tri;
            a->tri_out = s.tri_levels[a->tri_step];
        } else {
            a->sum[2] += a->tri_out * span;
        }
    }
    if (!(live & 8)) {
        return;
    }
    uint8_t volume = a->len[3] ? envelope_volume(&a->env[2], a->reg[12]) : 0;
    unsigned tap = (a->reg[14] & 0x80) ? 6 : 1;
    double left = span;
    while (a->noise_wait <= left) {
        if (!(a->noise_lfsr & 1)) {
            a->sum[3] += volume * a->noise_wait;
        }
        left -= a->noise_wait;
        unsigned feedback = (a->noise_lfsr ^ (a->noise_lfsr >> tap)) & 1;
        a->noise_lfsr = (uint16_t)((a->noise_lfsr >> 1) | (feedback << 14));
        a->noise_wait = noise_periods[a->reg[14] & 15];
    }
    if (!(a->noise_lfsr & 1)) {
        a->sum[3] += volume * left;
    }
    a->noise_wait -= left;
}

static void accumulate(double span)
{
    for (int i = 0; i < s.apus; i++) {
        accumulate_apu(&s.apu[i], s.live[i], span);
    }
}

static uint8_t s_mute;

void native_audio_set_mute(uint8_t mask)
{
    s_mute = mask & 0x0F;
}

static int16_t emit(void)
{
    double sum[4];
    for (int ch = 0; ch < 4; ch++) {
        sum[ch] = (s_channels & ~s_mute) >> ch & 1 ? s.apu[0].sum[ch] : 0;
    }
    double p = (sum[0] + sum[1]) / s.fill, t = (3 * sum[2] + 2 * sum[3]) / s.fill + s.apu[0].dmc_level;
    for (int i = 1; i < s.apus; i++) {
        for (int ch = 0; ch < 4; ch++) {
            sum[ch] = (s_channels & ~s_mute) >> ch & 1 ? s.apu[i].sum[ch] : 0;
        }
        p += (sum[0] + sum[1]) / s.fill;
        t += (3 * sum[2] + 2 * sum[3]) / s.fill;
    }
    double level = (p > 0 ? 95.52 / (8128.0 / p + 100) : 0) + (t > 0 ? 163.67 / (24329.0 / t + 100) : 0);
    double hp = s.hp90_a * (s.hp90_out + level - s.hp90_in);
    s.hp90_in = level;
    s.hp90_out = hp;
    double hp2 = s.hp440_a * (s.hp440_out + hp - s.hp440_in);
    s.hp440_in = hp;
    s.hp440_out = hp2;
    s.lp_out += s.lp_a * (hp2 - s.lp_out);
    double v = s.lp_out * 30000.0;
    if (v > 32767) {
        v = 32767;
    }
    if (v < -32768) {
        v = -32768;
    }
    s.fill = 0;
    for (int i = 0; i < s.apus; i++) {
        memset(s.apu[i].sum, 0, sizeof s.apu[i].sum);
    }
    return (int16_t)v;
}

static void render(double span, int16_t *out, size_t max, size_t *n)
{
    while (span > 0) {
        double take = s.cps - s.fill;
        if (take > span) {
            take = span;
        }
        accumulate(take);
        s.fill += take;
        span -= take;
        if (s.fill >= s.cps - 1e-9) {
            int16_t v = emit();
            if (*n < max) {
                out[*n] = v;
            }
            ++*n;
        }
    }
}

static void advance(double span, int16_t *out, size_t max, size_t *n)
{
    while (span > 0) {
        double to_clock = seq_points[s.five_step][s.seq_next] - s.seq_pos;
        if (to_clock > span) {
            render(span, out, max, n);
            s.seq_pos += span;
            return;
        }
        render(to_clock, out, max, n);
        span -= to_clock;
        s.seq_pos = seq_points[s.five_step][s.seq_next];
        quarter_frame();
        if (s.seq_next == 1 || s.seq_next == 3) {
            half_frame();
        }
        if (++s.seq_next == 4) {
            s.seq_next = 0;
            s.seq_pos -= seq_period[s.five_step];
        }
    }
}

static void length_load(NaApu *a, int ch, uint8_t value)
{
    if (a->enabled >> ch & 1) {
        a->len[ch] = length_table[value >> 3];
    }
}

static void reg_write(NaApu *a, uint16_t addr, uint8_t value)
{
    if (addr <= 0x400F) {
        a->reg[addr & 15] = value;
    }
    switch (addr) {
        case 0x4001:
        case 0x4005:
            a->sweep_reload[(addr - 0x4001) / 4] = 1;
            break;
        case 0x4002:
        case 0x4006: {
            uint16_t *p = &a->pulse_period[(addr - 0x4002) / 4];
            *p = (uint16_t)((*p & 0x700) | value);
            break;
        }
        case 0x4003:
        case 0x4007: {
            int ch = (addr - 0x4003) / 4;
            length_load(a, ch, value);
            a->pulse_period[ch] = (uint16_t)((a->pulse_period[ch] & 0xFF) | ((value & 7) << 8));
            a->pulse_step[ch] = 0;
            a->env[ch].start = 1;
            break;
        }
        case 0x400A:
            a->tri_period = (uint16_t)((a->tri_period & 0x700) | value);
            break;
        case 0x400B:
            length_load(a, 2, value);
            a->tri_period = (uint16_t)((a->tri_period & 0xFF) | ((value & 7) << 8));
            a->tri_reload = 1;
            break;
        case 0x400F:
            length_load(a, 3, value);
            a->env[2].start = 1;
            break;
        case 0x4011:
            a->dmc_level = value & 0x7F;
            break;
        case 0x4015:
            a->enabled = value & 0x0F;
            for (int ch = 0; ch < 4; ch++) {
                if (!(a->enabled >> ch & 1)) {
                    a->len[ch] = 0;
                }
            }
            break;
        case 0x4017:
            s.five_step = (value & 0x80) != 0;
            s.seq_pos = 0;
            s.seq_next = 0;
            if (s.five_step) {
                quarter_frame();
                half_frame();
            }
            break;
        default:
            break;
    }
}

/* The extra voices' writes for this frame, and which of their channels are
 * heard from now on. */
static void play_voices(const NativeVoiceFrame *v)
{
    for (int i = 0; i < 2; i++) {
        s.live[1 + i] = v->live[i];
        for (uint32_t k = 0; k < v->count[i]; k++) {
            reg_write(&s.apu[1 + i], v->writes[i][k].addr, v->writes[i][k].value);
        }
    }
}

size_t native_audio_frame(const NativeFrame *frame, int16_t *out, size_t max)
{
    double length = frame->cycles, now = 0;
    if (frame->held_cycles && frame->held_cycles < frame->cycles) {
        length -= frame->held_cycles;
    }
    double scale = frame->cycles ? length / frame->cycles : 1.0;
    size_t n = 0;
    static NativeVoiceFrame voices;
    bool extra = s_music_under_effects || s_overlap_effects;
    uint32_t voices_at = UINT32_MAX;
    if (extra) {
        if (s.apus == 1) {
            for (int i = 1; i < 3; i++) {
                apu_power_on(&s.apu[i]);
                reg_write(&s.apu[i], 0x4015, 0x0F);
                reg_write(&s.apu[i], 0x4001, 0x08);
                reg_write(&s.apu[i], 0x4005, 0x08);
                s.live[i] = 0;
            }
            s.apus = 3;
            native_voices_reset();
        }
        size_t prg_size;
        const uint8_t *prg = native_prg(&prg_size);
        native_voices_frame(frame->frame, native_cpu_ram(), prg, prg_size, s_music_under_effects, s_overlap_effects,
                            &voices);
        voices_at = 0;
        for (uint32_t i = 0; i < frame->apu_write_count; i++) {
            if (frame->apu_writes[i].addr == 0x4015) {
                voices_at = i;
                break;
            }
        }
    } else {
        s.apus = 1;
    }
    for (uint32_t i = 0; i < frame->apu_write_count; i++) {
        const NativeRegWrite *w = &frame->apu_writes[i];
        double at = w->cycle * scale;
        if (at > length) {
            at = length;
        }
        if (at > now) {
            advance(at - now, out, max, &n);
            now = at;
        }
        if (i == voices_at) {
            play_voices(&voices);
        }
        reg_write(&s.apu[0], w->addr, w->value);
    }
    if (voices_at == frame->apu_write_count) {
        play_voices(&voices);
    }
    if (length > now) {
        advance(length - now, out, max, &n);
    }
    return n < max ? n : max;
}

void native_audio_wav_header(FILE *f, int sample_rate, uint32_t samples)
{
    uint32_t rate = (uint32_t)sample_rate, bytes = samples * 2, riff = 36 + bytes, fmt = 16, byte_rate = rate * 2;
    uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    long at = ftell(f);
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f), fwrite(&channels, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&bytes, 4, 1, f);
    if (at > 44) {
        fseek(f, at, SEEK_SET);
    }
}
