/* reMixXXL -- fx.c: two effect units x three slots with Mixxx's chain semantics, in C.
 *
 * A unit (Mixxx EffectChainSlot "[EffectRack1_EffectUnitN]") has enabled, mix (dry/wet, Mixxx's default
 * DrySlashWet mode: out = in * (1 - mix) + wet * mix), super1 (sets every slot's meta knob), and the channels it is
 * inserted on (group_[ChannelN]_enable, group_[Master]_enable). A slot ("[EffectRack1_EffectUnitN_EffectM]") has
 * effect_idx (0 = NONE, the rest as RX_EFFECTS in remix_params.h), enabled, meta (0..1, drives the parameters the
 * effect links to it, after Mixxx EffectKnobParameterSlot) and parameter1..4 (each in Mixxx's 0..1 "parameter"
 * space, scaled to the effect's value by the manifest's scaler: linear / logarithmic / integral).
 *
 * The DSP of ECHO, FILTER, FLANGER, PHASER, BITCRUSH and MOOG LP is ported from Mixxx (src/effects/backends/builtin/
 * echoeffect.cpp, filtereffect.cpp, flangereffect.cpp, phasereffect.cpp, bitcrushereffect.cpp,
 * moogladder4filtereffect.cpp + src/engine/filters/enginefiltermoogladder4.h; GPL-2.0-or-later, in this GPL-3 plugin).
 * REVERB: Mixxx uses the CAPS Plate reverb (not ported); this is a Freeverb (Schroeder comb + allpass) reverb with
 * Mixxx's four parameters mapped onto it. LOOP ROLL is ours: a beat-length loop of the input repeated while on.
 *
 * Every effect that keeps state has its own state per (unit, slot, channel): 4 decks + master, allocated at create
 * (one 2 s stereo buffer per state shared by whichever effect is selected in the slot). An effect that is not
 * selected or not enabled costs nothing. No allocation, no locks, no I/O in fx_process_*. */
#include "remix.h"
#include "fx_priv.h"
#include "remix_params.h"
#include "dsp_priv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define FX_CHANNELS (RX_DECKS + 1)                /* 4 decks + master */
#define MASTER_CH RX_DECKS
#define BIG_FRAMES (2 * RX_SR)                    /* 2 s: Mixxx EchoGroupState::kMaxDelaySeconds */
#define MAX_PARAMS 4
#define BLOCK_MAX 256

enum { FX_NONE, FX_ECHO, FX_FILTER, FX_REVERB, FX_FLANGER, FX_PHASER, FX_BITCRUSH, FX_MOOG, FX_ROLL };
enum { SC_LIN, SC_LOG, SC_LOGINV, SC_INT };

typedef struct { const char *name; int scaler; double lo, def, hi; int link, inverse; double neutral; } pdef_t;
typedef struct { const char *name; int nparams; pdef_t p[MAX_PARAMS]; double meta_default; int add_dry_to_wet; } fxdef_t;

/* manifests, after the Mixxx effects' getManifest() (ranges: min, default, max) */
static const fxdef_t FXDEFS[RX_EFFECTS_N] = {
    {"NONE", 0, {{0}}, 0.5, 0},
    {"ECHO", 4, {{"TIME", SC_LIN, 0.0, 0.5, 2.0, RX_LINK_NONE, 0, 0},            /* beats (seconds without a tempo) */
                 {"FDBACK", SC_LIN, 0.0, 0.70794578, 1.0, RX_LINK_NONE, 0, 0},
                 {"PINGPONG", SC_LIN, 0.0, 0.0, 1.0, RX_LINK_NONE, 0, 0},
                 {"SEND", SC_LIN, 0.0, 0.70794578, 1.0, RX_LINK_LINKED, 0, 0}}, 0.5, 1},
    {"FILTER", 3, {{"LPF", SC_LOG, 13, 22050, 22050, RX_LINK_LEFT, 0, 1.0},
                   {"RES", SC_LOG, 0.4, 0.707106781, 4.0, RX_LINK_NONE, 0, 0},
                   {"HPF", SC_LOG, 13, 13, 22050, RX_LINK_RIGHT, 0, 0.0}}, 0.5, 0},
    {"REVERB", 4, {{"DECAY", SC_LIN, 0, 0.5, 1, RX_LINK_NONE, 0, 0},
                   {"BANDW", SC_LIN, 0, 1, 1, RX_LINK_NONE, 0, 0},
                   {"DAMP", SC_LIN, 0, 0, 1, RX_LINK_NONE, 0, 0},
                   {"SEND", SC_LIN, 0, 0, 1, RX_LINK_LINKED, 0, 0}}, 0.5, 1},
    {"FLANGER", 4, {{"SPEED", SC_LOGINV, 0.25, 8, 32, RX_LINK_NONE, 0, 0},          /* LFO period, beats */
                    {"WIDTH", SC_LOG, 0.0, 6.39, 12.78, RX_LINK_NONE, 0, 0},         /* ms */
                    {"MANUAL", SC_LOG, 0.22, 6.61, 13.0, RX_LINK_NONE, 0, 0},        /* ms */
                    {"REGEN", SC_LIN, 0.0, 0.25, 1.0, RX_LINK_NONE, 0, 0}}, 0.5, 0}, /* + mix: linked to meta */
    {"PHASER", 4, {{"PERIOD", SC_LIN, 0.0, 1.0, 4.0, RX_LINK_NONE, 0, 0},            /* beats */
                   {"FDBACK", SC_LIN, -1.0, 0.0, 1.0, RX_LINK_NONE, 0, 0},
                   {"RANGE", SC_LIN, 0.05, 1.0, 1.0, RX_LINK_NONE, 0, 0},
                   {"STAGES", SC_INT, 1, 7, 12, RX_LINK_NONE, 0, 0}}, 0.5, 0},       /* + depth: linked to meta */
    {"BITCRUSH", 2, {{"BITS", SC_LOG, 2, 16, 16, RX_LINK_LINKED, 1, 1.0},
                     {"DOWNSMP", SC_LOG, 0.02, 1.0, 1.0, RX_LINK_LINKED, 1, 1.0}}, 0.5, 0},
    {"MOOG LP", 3, {{"LPF", SC_LOG, 0.0003, 0.5, 0.5, RX_LINK_LEFT, 0, 1.0},         /* corner / sample rate */
                    {"RES", SC_LOG, 0.0, 1.0, 4.0, RX_LINK_NONE, 0, 0},
                    {"HPF", SC_LOG, 0.0003, 0.0003, 0.5, RX_LINK_RIGHT, 0, 0.0}}, 0.5, 0},
    {"LOOP ROLL", 2, {{"LENGTH", SC_INT, 0, 2, 4, RX_LINK_LINKED, 0, 0},            /* 1/8 1/4 1/2 1 2 beats */
                      {"DECAY", SC_LIN, 0, 1, 1, RX_LINK_NONE, 0, 0}}, 0.5, 0},
};
static const double ROLL_BEATS[5] = {0.125, 0.25, 0.5, 1, 2};

/* a parameter's value from its 0..1 control */
static double pvalue(const pdef_t *p, double ctl) {
    switch (p->scaler) {
    case SC_LOG: return rx_effect_log(ctl, p->lo, p->hi);
    case SC_LOGINV: return rx_logpot_inv(ctl, p->lo, p->hi);
    case SC_INT: return floor(rx_effect_lin(ctl, p->lo, p->hi) + 0.5);
    default: return rx_effect_lin(ctl, p->lo, p->hi);
    }
}
/* the 0..1 control for a value (to set defaults) */
static double pctl_of(const pdef_t *p, double v) {
    if (p->scaler == SC_LIN || p->scaler == SC_INT) return p->hi > p->lo ? (v - p->lo) / (p->hi - p->lo) : 0;
    /* search the monotonic scaler (cheap: once per effect change) */
    double lo = 0, hi = 1;
    int inv = p->scaler == SC_LOGINV;
    for (int i = 0; i < 40; i++) { double m = (lo + hi) / 2; double x = pvalue(p, m); if ((x < v) != inv) lo = m; else hi = m; }
    return (lo + hi) / 2;
}

/* -------------------------------------------------------------------------------------------- per-state DSP */
#define FLANGER_LEN 1344                           /* (kMaxDelayMs + 1) * 96 */
#define RV_COMBS 8
#define RV_ALLPASSES 4
static const int RV_COMB_LEN[RV_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int RV_AP_LEN[RV_ALLPASSES] = {556, 441, 341, 225};
#define RV_SPREAD 23

typedef struct {
    float m_azt1, m_azt2, m_azt3, m_azt4, m_az5, m_amf;
} moog_buf_t;
typedef struct {
    moog_buf_t b[2];
    float k2vg, kacr, post_gain;                      /* current */
    float k2vg_new, kacr_new, post_gain_new;          /* targets (ramped over the block) */
    double cutoff, resonance; int highpass;
} moog_t;

typedef struct {
    float *big;                                        /* BIG_FRAMES stereo frames */
    int loaded_type;
    /* echo */
    int write_pos, prev_delay_samples, ping_pong; float prev_send, prev_feedback;
    /* filter */
    rx_bq_coef_t lp, hp; rx_bq_state_t lps[2], hps[2]; double lpf, q, hpf;
    /* reverb */
    int rv_pos[2][RV_COMBS + RV_ALLPASSES]; float rv_store[2][RV_COMBS]; float rv_in_lp[2]; float rv_send_prev;
    /* flanger */
    int delay_pos; unsigned lfo_frames; double prev_period; float prev_mix, prev_regen, prev_width, prev_manual;
    /* phaser */
    float ph_in[2][12], ph_out[2][12], ph_phase[2], ph_depth_old, ph_l, ph_r;
    /* bitcrush */
    float acc, hold_l, hold_r;
    /* moog */
    moog_t moog_lp, moog_hp; double moog_lpf, moog_hpf, moog_res;
    /* roll */
    int roll_len, roll_pos, roll_filled; float roll_gain;
} fxstate_t;

static void state_reset(fxstate_t *s, int type) {
    float *big = s->big;
    memset(s, 0, sizeof *s);
    s->big = big; s->loaded_type = type;
    s->lpf = 22050; s->hpf = 13; s->q = 0.707106781;
    s->prev_manual = 6.61f; s->prev_period = -1;
    s->moog_lpf = 0.5; s->moog_hpf = 0.0003; s->moog_res = 1;
    s->moog_lp.cutoff = s->moog_hp.cutoff = -1;
    if (type == FX_ECHO || type == FX_REVERB || type == FX_FLANGER || type == FX_ROLL) memset(big, 0, sizeof(float) * 2 * BIG_FRAMES);
}

/* ---- ECHO: after Mixxx EchoEffect::processChannel */
static void echo_process(fxstate_t *s, const float *in, float *out, int frames, double beat_len, const double *pv) {
    double period = pv[0];
    float send_cur = (float)pv[3], fb_cur = (float)pv[1], pp = (float)pv[2];
    double delay_seconds;
    if (beat_len > 0) {   /* beats, quantised to 1/4 beat (Mixxx's "quantize" parameter defaults on) */
        period = rx_round_to_fraction(period, 4); if (period < 1 / 8.0) period = 1 / 8.0;
        delay_seconds = period * beat_len;
    } else { if (period < 1 / 8.0) period = 1 / 8.0; delay_seconds = period; }
    if (delay_seconds <= 0) delay_seconds = 1.0 / RX_SR;
    int delay_frames = (int)(delay_seconds * RX_SR); if (delay_frames > BIG_FRAMES) delay_frames = BIG_FRAMES;
    int delay_samples = delay_frames * 2, len = BIG_FRAMES * 2;
    float *buf = s->big;
    int prev_read = (s->write_pos + len - s->prev_delay_samples) % len;
    int read = (s->write_pos + len - delay_samples) % len;
    float send_d = (send_cur - s->prev_send) / frames, fb_d = (fb_cur - s->prev_feedback) / frames;
    float send = s->prev_send, fb = s->prev_feedback;
    for (int i = 0; i < frames * 2; i += 2) {
        send += send_d; fb += fb_d;
        float bl = buf[read], br = buf[read + 1];
        if (read != prev_read) {              /* crossfade from the old delay time */
            float frac = (float)i / (frames * 2);
            bl = bl * frac + buf[prev_read] * (1 - frac);
            br = br * frac + buf[prev_read + 1] * (1 - frac);
            prev_read = (prev_read + 2) % len;
        }
        read = (read + 2) % len;
        buf[s->write_pos] = rx_clampf(in[i] * send + bl * fb, -1, 1);
        buf[s->write_pos + 1] = rx_clampf(in[i + 1] * send + br * fb, -1, 1);
        if (s->ping_pong < delay_samples / 2) { out[i] = (bl + br * pp) / (1 + pp); out[i + 1] = br * (1 - pp); }
        else { out[i] = bl * (1 - pp); out[i + 1] = (br + bl * pp) / (1 + pp); }
        s->write_pos = (s->write_pos + 2) % len;
        if (++s->ping_pong >= delay_samples) s->ping_pong = 0;
    }
    s->prev_send = send_cur; s->prev_feedback = fb_cur; s->prev_delay_samples = delay_samples;
}

/* ---- FILTER: after Mixxx FilterEffect::processChannel (biquad LP then HP, Q limited where they overlap) */
static void filter_process(fxstate_t *s, const float *in, float *out, int frames, const double *pv) {
    double lpf = pv[0], q = pv[1], hpf = pv[2];
    if (lpf >= 22050 * 0.999) lpf = 22050; if (hpf <= 13 * 1.001) hpf = 13;
    if (lpf != s->lpf || q != s->q || hpf != s->hpf) {
        double ratio = hpf / lpf, cq = q;
        if (ratio < 1.414 && ratio >= 1) { ratio -= 1; double qmax = 2 + ratio * ratio * ratio * 29; if (cq > qmax) cq = qmax; }
        else if (ratio < 1 && ratio >= 0.7) { if (cq > 2.0) cq = 2.0; }
        else if (ratio < 0.7 && ratio > 0.1) { ratio -= 0.1; double qmax = 4 - 2 / 0.6 * ratio; if (cq > qmax) cq = qmax; }
        rx_bq_lowpass(&s->lp, lpf, cq, RX_SR); rx_bq_highpass(&s->hp, hpf, cq, RX_SR);
        if (s->lpf >= 22050 && lpf < 22050) memset(s->lps, 0, sizeof s->lps);
        if (s->hpf <= 13 && hpf > 13) memset(s->hps, 0, sizeof s->hps);
        s->lpf = lpf; s->q = q; s->hpf = hpf;
    }
    int lp_on = lpf < 22050, hp_on = hpf > 13;
    for (int i = 0; i < frames * 2; i++) {
        float x = in[i]; int ch = i & 1;
        if (hp_on) x = rx_bq_tick(&s->hp, &s->hps[ch], x);
        if (lp_on) x = rx_bq_tick(&s->lp, &s->lps[ch], x);
        out[i] = x;
    }
}

/* ---- REVERB: Freeverb (Jezar's public-domain design), Mixxx's parameters mapped: decay -> room size, damping ->
 * comb damping, bandwidth -> input one-pole lowpass, send -> input level (ramped, added to dry by the chain) */
static void reverb_process(fxstate_t *s, const float *in, float *out, int frames, const double *pv) {
    float decay = (float)pv[0], bw = (float)pv[1], damp = (float)pv[2], send_cur = (float)pv[3];
    float room = 0.70f + 0.28f * decay, dampc = 0.05f + 0.6f * damp;
    float bw_coef = 1.0f - expf(-2.0f * (float)M_PI * (800.0f + 17000.0f * bw) / RX_SR);
    float send = s->rv_send_prev, send_d = (send_cur - send) / frames;
    float *base = s->big;
    for (int i = 0; i < frames; i++) {
        send += send_d;
        for (int ch = 0; ch < 2; ch++) {
            float x = (in[2 * i] + in[2 * i + 1]) * 0.5f * send * 0.015f;
            s->rv_in_lp[ch] += bw_coef * (x - s->rv_in_lp[ch]);
            x = s->rv_in_lp[ch];
            float acc = 0; float *p = base + ch * (BIG_FRAMES);   /* each channel's lines in its own half of the buffer */
            int off = 0;
            for (int c = 0; c < RV_COMBS; c++) {
                int len = RV_COMB_LEN[c] + ch * RV_SPREAD;
                float *line = p + off; int *pos = &s->rv_pos[ch][c];
                float y = line[*pos];
                s->rv_store[ch][c] = y * (1 - dampc) + s->rv_store[ch][c] * dampc;
                line[*pos] = x + s->rv_store[ch][c] * room;
                if (++*pos >= len) *pos = 0;
                acc += y; off += len;
            }
            for (int a = 0; a < RV_ALLPASSES; a++) {
                int len = RV_AP_LEN[a] + ch * RV_SPREAD;
                float *line = p + off; int *pos = &s->rv_pos[ch][RV_COMBS + a];
                float b = line[*pos];
                line[*pos] = acc + b * 0.5f;
                acc = b - acc;
                if (++*pos >= len) *pos = 0;
                off += len;
            }
            out[2 * i + ch] = acc * 3.0f;
        }
    }
    s->rv_send_prev = send_cur;
}

/* ---- FLANGER: after Mixxx FlangerEffect::processChannel; mix is the slot's meta (Linked in Mixxx) */
static void flanger_process(fxstate_t *s, const float *in, float *out, int frames, double beat_len, const double *pv, double mix_param) {
    const double kMaxDelayMs = 13.0, kMinDelayMs = 0.22, kCenter = (kMaxDelayMs - kMinDelayMs) / 2 + kMinDelayMs, kMaxWidth = kMaxDelayMs - kMinDelayMs;
    const float kGainCorrection = 1.41253754f;
    double period = pv[0], lfo_frames;
    if (beat_len > 0) { period = rx_round_to_fraction(period, 2); if (period < 0.25) period = 0.25; lfo_frames = period * beat_len * RX_SR; }
    else { if (period < 0.25) period = 0.25; lfo_frames = period * RX_SR; }
    if (s->prev_period > 0) s->lfo_frames = (unsigned)(lfo_frames / s->prev_period * s->lfo_frames);
    s->prev_period = lfo_frames;
    float mix = (float)mix_param, regen = (float)pv[3];
    double width = pv[1], manual = pv[2];
    double max_manual = kCenter + (kMaxWidth - width) / 2, min_manual = kCenter - (kMaxWidth - width) / 2;
    manual = rx_clamp(manual, min_manual, max_manual);
    float mix_d = (mix - s->prev_mix) / frames, regen_d = (regen - s->prev_regen) / frames;
    double width_d = (width - s->prev_width) / frames, manual_d = (manual - s->prev_manual) / frames;
    float mixr = s->prev_mix, regenr = s->prev_regen; double widthr = s->prev_width, manualr = s->prev_manual;
    float *dl = s->big, *dr = s->big + FLANGER_LEN;
    for (int i = 0; i < frames * 2; i += 2) {
        mixr += mix_d; regenr += regen_d; widthr += width_d; manualr += manual_d;
        if (++s->lfo_frames >= (unsigned)lfo_frames) s->lfo_frames = 0;
        float pf = s->lfo_frames / (float)lfo_frames;
        double delay_ms = manualr + widthr / 2 * sin(M_PI * 2.0 * pf);
        double delay_frames = delay_ms * RX_SR / 1000;
        int fp = (s->delay_pos - (int)floor(delay_frames) + FLANGER_LEN) % FLANGER_LEN;
        int fn = (s->delay_pos - (int)ceil(delay_frames) + FLANGER_LEN) % FLANGER_LEN;
        float frac = (float)(delay_frames - floor(delay_frames));
        float dsl = dl[fp] + frac * (dl[fn] - dl[fp]), dsr = dr[fp] + frac * (dr[fn] - dr[fp]);
        dl[s->delay_pos] = rx_tanh_approx(in[i] + regenr * dsl);
        dr[s->delay_pos] = rx_tanh_approx(in[i + 1] + regenr * dsr);
        s->delay_pos = (s->delay_pos + 1) % FLANGER_LEN;
        float gain = 1 - mixr + kGainCorrection * mixr;
        out[i] = (in[i] + mixr * dsl) / gain;
        out[i + 1] = (in[i + 1] + mixr * dsr) / gain;
    }
    s->prev_mix = mix; s->prev_regen = regen; s->prev_width = (float)width; s->prev_manual = (float)manual;
}

/* ---- PHASER: after Mixxx PhaserEffect::processChannel; depth is the slot's meta (Linked, 0.5..1) */
static inline float phaser_sample(float input, float *oi, float *oo, float coef, int stages) {
    for (int j = 0; j < stages; j++) { oo[j] = coef * input + coef * oo[j] - oi[j]; oi[j] = input; input = oo[j]; }
    return input;
}
static void phaser_process(fxstate_t *s, const float *in, float *out, int frames, double beat_len, const double *pv, double meta) {
    const float kDoublePi = (float)(2.0 * M_PI);
    double period = pv[0], period_samples;
    if (beat_len > 0) { period = rx_round_to_fraction(period, 2); if (period < 0.25) period = 0.25; period_samples = period * beat_len * RX_SR; }
    else { if (period < 0.25) period = 0.25; period_samples = period * RX_SR; }
    float freq_skip = (float)(1.0 / period_samples * kDoublePi);
    float feedback = (float)pv[1], range = (float)pv[2]; int stages = (int)pv[3]; if (stages < 1) stages = 1; if (stages > 12) stages = 12;
    float depth = (float)(0.5 + 0.5 * rx_clamp(meta, 0, 1));
    float depth_delta = (depth - s->ph_depth_old) / frames, dep = s->ph_depth_old + depth_delta;
    float coef_l = 0, coef_r = 0; int counter = 0;
    for (int i = 0; i < frames * 2; i += 2) {
        s->ph_l = in[i] + tanhf(s->ph_l * feedback);
        s->ph_r = in[i + 1] + tanhf(s->ph_r * feedback);
        s->ph_phase[0] = fmodf(s->ph_phase[0] + freq_skip, kDoublePi);
        s->ph_phase[1] = fmodf(s->ph_phase[1] + freq_skip, kDoublePi);
        if ((counter++) % 32 == 0) {
            float d_l = 0.5f + 0.5f * sinf(s->ph_phase[0]), d_r = 0.5f + 0.5f * sinf(s->ph_phase[1]);
            float tl = tanhf(range * d_l / 2), tr = tanhf(range * d_r / 2);
            coef_l = (1 - tl) / (1 + tl); coef_r = (1 - tr) / (1 + tr);
        }
        s->ph_l = phaser_sample(s->ph_l, s->ph_in[0], s->ph_out[0], coef_l, stages);
        s->ph_r = phaser_sample(s->ph_r, s->ph_in[1], s->ph_out[1], coef_r, stages);
        float dd = dep + depth_delta * (i / 2);
        out[i] = in[i] * (1 - 0.5f * dd) + s->ph_l * dd * 0.5f;
        out[i + 1] = in[i + 1] * (1 - 0.5f * dd) + s->ph_r * dd * 0.5f;
    }
    s->ph_depth_old = depth;
}

/* ---- BITCRUSH: after Mixxx BitCrusherEffect::processChannel */
static void bitcrush_process(fxstate_t *s, const float *in, float *out, int frames, const double *pv) {
    float bit_depth = (float)pv[0], downsample = (float)pv[1];
    float scale = powf(2.0f, bit_depth) / 2, gc = (17 - bit_depth) / 8;
    for (int i = 0; i < frames * 2; i += 2) {
        s->acc += downsample;
        if (s->acc >= 1.0f) {
            s->acc -= 1.0f;
            if (bit_depth < 16) {
                s->hold_l = floorf(rx_clampf(in[i] * gc, -1, 1) * scale + 0.5f) / scale / gc;
                s->hold_r = floorf(rx_clampf(in[i + 1] * gc, -1, 1) * scale + 0.5f) / scale / gc;
            } else { s->hold_l = in[i]; s->hold_r = in[i + 1]; }
        }
        out[i] = s->hold_l; out[i + 1] = s->hold_r;
    }
}

/* ---- MOOG LP/HP: after Mixxx EngineFilterMoogLadderBase (LowPassOversampling / HighPassOversampling) */
#define MOOG_KVT 1.2f
static void moog_set(moog_t *m, double cutoff_hz, double resonance, int highpass) {
    const float v2 = 2 + MOOG_KVT;
    float kfc = (float)(cutoff_hz / RX_SR), kf = kfc / 2;     /* oversampling: half the filter rate */
    float kfcr = 1.8730f * (kfc * kfc * kfc) + 0.4955f * (kfc * kfc) - 0.6490f * kfc + 0.9988f;
    float x = -2.0f * (float)M_PI * kfcr * kf;
    m->k2vg_new = v2 * (1 - expf(x));
    m->kacr_new = (float)resonance * (-3.9364f * (kfc * kfc) + 1.8409f * kfc + 0.9968f);
    m->post_gain_new = highpass ? 1.0f : 1.0001784074555027f + (0.9331585678097162f * (float)resonance);
    m->highpass = highpass; m->cutoff = cutoff_hz; m->resonance = resonance;
}
static inline float moog_sample(moog_t *m, float input, moog_buf_t *b) {
    const float v2 = 2 + MOOG_KVT;
    float x1, az1, at1, az2, at2, az3, at3, az4, at4;
    for (int pass = 0; pass < 2; pass++) {       /* 2x oversampling: the same block twice */
        x1 = input - b->m_amf * m->kacr;
        az1 = b->m_azt1 + m->k2vg * rx_tanh_approx(x1 / v2);
        at1 = m->k2vg * rx_tanh_approx(az1 / v2); b->m_azt1 = az1 - at1;
        az2 = b->m_azt2 + at1; at2 = m->k2vg * rx_tanh_approx(az2 / v2); b->m_azt2 = az2 - at2;
        az3 = b->m_azt3 + at2; at3 = m->k2vg * rx_tanh_approx(az3 / v2); b->m_azt3 = az3 - at3;
        az4 = b->m_azt4 + at3; at4 = m->k2vg * rx_tanh_approx(az4 / v2); b->m_azt4 = az4 - at4;
        b->m_amf = (az4 + b->m_az5) / 2; b->m_az5 = az4;    /* 1/2-sample delay for phase compensation */
    }
    if (m->highpass) return (x1 - 3 * az3 + 2 * az4) * m->post_gain;
    return b->m_amf * m->post_gain;
}
static void moog_run(moog_t *m, const float *in, float *out, int frames) {
    float dk = (m->k2vg_new - m->k2vg) / frames, da = (m->kacr_new - m->kacr) / frames, dg = (m->post_gain_new - m->post_gain) / frames;
    for (int i = 0; i < frames * 2; i += 2) {
        m->k2vg += dk; m->kacr += da; m->post_gain += dg;
        out[i] = moog_sample(m, in[i], &m->b[0]);
        out[i + 1] = moog_sample(m, in[i + 1], &m->b[1]);
    }
    m->k2vg = m->k2vg_new; m->kacr = m->kacr_new; m->post_gain = m->post_gain_new;
}
static void moog_process(fxstate_t *s, const float *in, float *out, int frames, const double *pv) {
    double lpf = pv[0], res = pv[1], hpf = pv[2];
    if (lpf >= 0.5 * 0.999) lpf = 0.5; if (hpf <= 0.0003 * 1.001) hpf = 0.0003;
    if (lpf != s->moog_lpf || res != s->moog_res) { moog_set(&s->moog_lp, lpf * RX_SR, res, 0); if (s->moog_lp.k2vg == 0 && s->moog_lp.kacr == 0 && s->moog_lp.post_gain == 0) { s->moog_lp.k2vg = s->moog_lp.k2vg_new; s->moog_lp.kacr = s->moog_lp.kacr_new; s->moog_lp.post_gain = s->moog_lp.post_gain_new; } }
    if (hpf != s->moog_hpf || res != s->moog_res) { moog_set(&s->moog_hp, hpf * RX_SR, res, 1); if (s->moog_hp.k2vg == 0 && s->moog_hp.kacr == 0 && s->moog_hp.post_gain == 0) { s->moog_hp.k2vg = s->moog_hp.k2vg_new; s->moog_hp.kacr = s->moog_hp.kacr_new; s->moog_hp.post_gain = s->moog_hp.post_gain_new; } }
    int lp_on = lpf < 0.5, hp_on = hpf > 0.0003;
    if (lp_on && s->moog_lpf >= 0.5) memset(s->moog_lp.b, 0, sizeof s->moog_lp.b);
    if (hp_on && s->moog_hpf <= 0.0003) memset(s->moog_hp.b, 0, sizeof s->moog_hp.b);
    s->moog_lpf = lpf; s->moog_hpf = hpf; s->moog_res = res;
    if (hp_on && lp_on) { float tmp[BLOCK_MAX * 2]; moog_run(&s->moog_hp, in, tmp, frames); moog_run(&s->moog_lp, tmp, out, frames); }
    else if (hp_on) moog_run(&s->moog_hp, in, out, frames);
    else if (lp_on) moog_run(&s->moog_lp, in, out, frames);
    else memcpy(out, in, sizeof(float) * 2 * (size_t)frames);
}

/* ---- LOOP ROLL (ours): the first `length` beats of input after the slot comes on are captured and repeated */
static void roll_process(fxstate_t *s, const float *in, float *out, int frames, double beat_len, const double *pv) {
    int li = (int)pv[0]; if (li < 0) li = 0; if (li > 4) li = 4;
    double bl = beat_len > 0 ? beat_len : 0.5;      /* 120 bpm without a tempo */
    int len = (int)(ROLL_BEATS[li] * bl * RX_SR); if (len < 64) len = 64; if (len > BIG_FRAMES) len = BIG_FRAMES;
    float decay = (float)pv[1];
    if (len != s->roll_len) { s->roll_len = len; if (s->roll_pos >= len) s->roll_pos = 0; if (s->roll_filled > len) s->roll_filled = len; }
    float *buf = s->big;
    for (int i = 0; i < frames; i++) {
        if (s->roll_filled < s->roll_len) {        /* still capturing: pass through and record */
            buf[2 * s->roll_pos] = in[2 * i]; buf[2 * s->roll_pos + 1] = in[2 * i + 1];
            out[2 * i] = in[2 * i]; out[2 * i + 1] = in[2 * i + 1];
            s->roll_filled++;
            if (s->roll_filled == s->roll_len) s->roll_gain = 1.0f;
        } else {
            out[2 * i] = buf[2 * s->roll_pos] * s->roll_gain; out[2 * i + 1] = buf[2 * s->roll_pos + 1] * s->roll_gain;
        }
        if (++s->roll_pos >= s->roll_len) { s->roll_pos = 0; if (s->roll_filled >= s->roll_len) s->roll_gain *= decay; }
    }
}

/* ------------------------------------------------------------------------------------------------- the units */
typedef struct {
    ctl_id effect_idx, enabled, meta, param[MAX_PARAMS];
    int type;                      /* the effect loaded (controls follow effect_idx) */
    double last_meta;
    fxstate_t st[FX_CHANNELS];
} slot_t;
typedef struct {
    ctl_id enabled, mix, super1, group_enable[FX_CHANNELS], mix_mode;
    slot_t slot[RX_FX_SLOTS];
    double last_super;
    int ch_enabled[FX_CHANNELS];   /* the effective enable last block (for the disable fade / enable reset) */
    float ch_mix_old[FX_CHANNELS];
} unit_t;
struct rx_fx {
    unit_t unit[RX_FX_UNITS];
    double tempo[FX_CHANNELS];
    float *pool;
    int ticked;                    /* the per-block control tick ran (reset by fx_process_master) */
    float buf_a[BLOCK_MAX * 2], buf_b[BLOCK_MAX * 2];
};

rx_fx_t *fx_create(void) {
    rx_fx_t *f = calloc(1, sizeof *f);
    size_t per = (size_t)2 * BIG_FRAMES;
    f->pool = calloc(per * RX_FX_UNITS * RX_FX_SLOTS * FX_CHANNELS, sizeof(float));
    int k = 0;
    for (int u = 0; u < RX_FX_UNITS; u++) for (int s = 0; s < RX_FX_SLOTS; s++) for (int c = 0; c < FX_CHANNELS; c++) {
        f->unit[u].slot[s].st[c].big = f->pool + per * (size_t)k++;
        state_reset(&f->unit[u].slot[s].st[c], FX_NONE);
    }
    for (int c = 0; c < FX_CHANNELS; c++) f->tempo[c] = 0;
    return f;
}
void fx_destroy(rx_fx_t *f) { if (!f) return; free(f->pool); free(f); }
void fx_set_tempo(rx_fx_t *f, double bpm) { for (int c = 0; c < FX_CHANNELS; c++) f->tempo[c] = bpm; }
void fx_set_channel_tempo(rx_fx_t *f, int channel, double bpm) { if (channel >= 0 && channel < FX_CHANNELS) f->tempo[channel] = bpm; }

int fx_effect_count(void) { return RX_EFFECTS_N; }
const char *fx_effect_name(int i) { return (i >= 0 && i < RX_EFFECTS_N) ? FXDEFS[i].name : NULL; }
int fx_param_count(int effect) { return (effect >= 0 && effect < RX_EFFECTS_N) ? FXDEFS[effect].nparams : 0; }
const char *fx_param_name(int effect, int p) {
    if (effect < 0 || effect >= RX_EFFECTS_N || p < 0 || p >= FXDEFS[effect].nparams) return NULL;
    return FXDEFS[effect].p[p].name;
}

void fx_register_controls(rx_fx_t *f) {
    char g[64], n[48];
    for (int u = 0; u < RX_FX_UNITS; u++) {
        unit_t *un = &f->unit[u];
        snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d]", u + 1);
        un->enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 0);
        un->mix = ctl_register(g, "mix", CK_FLOAT, 0, 1, 1);
        un->super1 = ctl_register(g, "super1", CK_FLOAT, 0, 1, 0.5);
        un->mix_mode = ctl_register(g, "mix_mode", CK_INT, 0, 1, 0);           /* 0 DrySlashWet (default), 1 DryPlusWet */
        for (int c = 0; c < RX_DECKS; c++) { snprintf(n, sizeof n, "group_[Channel%d]_enable", c + 1); un->group_enable[c] = ctl_register(g, n, CK_TOGGLE, 0, 1, 0); }
        un->group_enable[MASTER_CH] = ctl_register(g, "group_[Master]_enable", CK_TOGGLE, 0, 1, 0);
        un->last_super = 0.5;
        for (int s = 0; s < RX_FX_SLOTS; s++) {
            slot_t *sl = &un->slot[s];
            snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d_Effect%d]", u + 1, s + 1);
            sl->effect_idx = ctl_register(g, "effect_idx", CK_INT, 0, RX_EFFECTS_N - 1, 0);
            sl->enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 1);
            sl->meta = ctl_register(g, "meta", CK_FLOAT, 0, 1, 0.5);
            for (int p = 0; p < MAX_PARAMS; p++) { snprintf(n, sizeof n, "parameter%d", p + 1); sl->param[p] = ctl_register(g, n, CK_FLOAT, 0, 1, 0.5); }
            ctl_register(g, "loaded", CK_TOGGLE, 0, 1, 0);
            ctl_register(g, "num_parameters", CK_INT, 0, MAX_PARAMS, 0);
            sl->type = FX_NONE; sl->last_meta = 0.5;
        }
    }
}

/* the slot's control-side bookkeeping: effect changes load defaults, the meta knob drives linked parameters */
static void slot_tick(slot_t *sl) {
    int type = (int)ctl_get(sl->effect_idx); if (type < 0 || type >= RX_EFFECTS_N) type = 0;
    if (type != sl->type) {
        sl->type = type;
        const fxdef_t *d = &FXDEFS[type];
        for (int p = 0; p < MAX_PARAMS; p++) ctl_set(sl->param[p], p < d->nparams ? pctl_of(&d->p[p], d->p[p].def) : 0.5);
        ctl_set(sl->meta, d->meta_default); sl->last_meta = d->meta_default;
        for (int c = 0; c < FX_CHANNELS; c++) state_reset(&sl->st[c], type);
    }
    double meta = ctl_get(sl->meta);
    if (meta != sl->last_meta) {      /* EffectKnobParameterSlot::onEffectMetaParameterChanged for each linked parameter */
        sl->last_meta = meta;
        const fxdef_t *d = &FXDEFS[type];
        for (int p = 0; p < d->nparams; p++) {
            if (d->p[p].link == RX_LINK_NONE) continue;
            double v = rx_meta_link(meta, d->p[p].link, d->p[p].inverse, d->p[p].neutral);
            if (v >= 0) ctl_set(sl->param[p], v);
        }
    }
}

static void unit_tick(unit_t *un) {
    double super = ctl_get(un->super1);
    if (super != un->last_super) { un->last_super = super; for (int s = 0; s < RX_FX_SLOTS; s++) ctl_set(un->slot[s].meta, super); }
    for (int s = 0; s < RX_FX_SLOTS; s++) slot_tick(&un->slot[s]);
}

/* one unit on one channel, in place. */
static void unit_process(rx_fx_t *f, int u, int ch, float *lr, int frames) {
    unit_t *un = &f->unit[u];
    if (frames > BLOCK_MAX) frames = BLOCK_MAX;
    int on = ctl_get(un->enabled) >= 0.5 && ctl_get(un->group_enable[ch]) >= 0.5;
    int was = un->ch_enabled[ch];
    if (!on && !was) return;
    if (on && !was) for (int s = 0; s < RX_FX_SLOTS; s++) state_reset(&un->slot[s].st[ch], un->slot[s].type);   /* Enabling: fresh state */
    un->ch_enabled[ch] = on;
    double beat_len = f->tempo[ch] > 0 ? 60.0 / f->tempo[ch] : 0;
    float *in = lr, *cur = lr, *tmp;
    int processed = 0;
    for (int s = 0; s < RX_FX_SLOTS; s++) {
        slot_t *sl = &un->slot[s];
        if (sl->type == FX_NONE || ctl_get(sl->enabled) < 0.5) continue;
        const fxdef_t *d = &FXDEFS[sl->type];
        double pv[MAX_PARAMS] = {0, 0, 0, 0};
        for (int p = 0; p < d->nparams; p++) pv[p] = pvalue(&d->p[p], ctl_get(sl->param[p]));
        double meta = ctl_get(sl->meta);
        fxstate_t *st = &sl->st[ch];
        tmp = (cur == f->buf_a) ? f->buf_b : f->buf_a;
        switch (sl->type) {
        case FX_ECHO: echo_process(st, cur, tmp, frames, beat_len, pv); break;
        case FX_FILTER: filter_process(st, cur, tmp, frames, pv); break;
        case FX_REVERB: reverb_process(st, cur, tmp, frames, pv); break;
        case FX_FLANGER: flanger_process(st, cur, tmp, frames, beat_len, pv, meta); break;
        case FX_PHASER: phaser_process(st, cur, tmp, frames, beat_len, pv, meta); break;
        case FX_BITCRUSH: bitcrush_process(st, cur, tmp, frames, pv); break;
        case FX_MOOG: moog_process(st, cur, tmp, frames, pv); break;
        case FX_ROLL: roll_process(st, cur, tmp, frames, beat_len, pv); break;
        default: continue;
        }
        if (d->add_dry_to_wet) for (int i = 0; i < frames * 2; i++) tmp[i] += cur[i];   /* EngineEffectChain: echo/reverb output + their input */
        cur = tmp; processed = 1;
    }
    float mix = on ? (float)rx_clamp(ctl_get(un->mix), 0, 1) : 0.0f;     /* Disabling: fade to dry over this block */
    float mix_old = un->ch_mix_old[ch];
    if (processed) {
        int plus = ctl_get(un->mix_mode) >= 0.5;
        float dm = (mix - mix_old) / frames, m = mix_old;
        for (int i = 0; i < frames; i++) {
            m += dm;
            float dry = plus ? 1.0f : 1.0f - m;
            lr[2 * i] = in[2 * i] * dry + cur[2 * i] * m;
            lr[2 * i + 1] = in[2 * i + 1] * dry + cur[2 * i + 1] * m;
        }
    }
    un->ch_mix_old[ch] = mix;
    if (!on) for (int s = 0; s < RX_FX_SLOTS; s++) state_reset(&un->slot[s].st[ch], un->slot[s].type);
}

void fx_process_channel(rx_fx_t *f, int channel, float *lr, int frames) {
    if (channel < 0 || channel >= RX_DECKS) return;
    if (!f->ticked) { f->ticked = 1; for (int u = 0; u < RX_FX_UNITS; u++) unit_tick(&f->unit[u]); }   /* once per block */
    for (int u = 0; u < RX_FX_UNITS; u++) unit_process(f, u, channel, lr, frames);
}
void fx_process_master(rx_fx_t *f, float *lr, int frames) {
    if (!f->ticked) for (int u = 0; u < RX_FX_UNITS; u++) unit_tick(&f->unit[u]);
    f->ticked = 0;
    for (int u = 0; u < RX_FX_UNITS; u++) unit_process(f, u, MASTER_CH, lr, frames);
}
