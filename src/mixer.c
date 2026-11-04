/* reMixXXL -- mixer.c: Mixxx's EngineMixer in C.
 *
 * Per channel: pregain (EnginePregain's knob; replay gain and the vinyl speed gain are applied in deck.c where the
 * rate is known) -> EQ (Bessel4 LV-Mix isolator, Mixxx's default: after src/effects/backends/builtin/
 * bessel4lvmixeqeffect.cpp + lvmixeqbase.h) -> quick effect (Mixxx's Filter effect on super1: after
 * src/effects/backends/builtin/filtereffect.cpp) -> the effect units enabled for the channel (fx.c) -> channel
 * fader (linear) and crossfader (after src/engine/enginexfader.cpp, "slow fade / fast cut" default) with the
 * channel's orientation -> sum -> master effects -> master gain -> soft limiter -> VU meters (after
 * src/engine/enginevumeter.cpp).
 *
 * No allocation, no locks, no I/O in mixer_render. GPL-2.0-or-later designs from Mixxx in a GPL-3 plugin. */
#include "remix.h"
#include "dsp_priv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------------------------------ the LV-Mix EQ
 * Lipshitz-Vanderkooy mix: the dry signal is the "high" band; subtracting a delay-matched Bessel lowpass at the
 * high crossover gives the high band, the difference between the two lowpasses the mid band, the low lowpass the
 * low band. Each lowpass corner is quantised so its pass-band group delay is an integer number of frames, and the
 * dry / mid paths are delayed to match (that is what makes the sum bit-perfect at unity).
 *   low  = LP1(x)               (corner ~246 Hz,  delay d1)
 *   mid  = LP2(z^-(d1-d2) x)    (corner ~2484 Hz, delay d2) ... delayed to d1
 *   high = z^-d1 x
 *   out  = (gLow - gMid) * low + (gMid - gHigh) * mid + gHigh * high  (LVMixEQEffectGroupState::processChannel) */
#define EQ_MAX_DELAY 3300
#define EQ_LO 246.0
#define EQ_HI 2484.0
typedef struct {
    rx_bessel4_coef_t c1, c2;
    rx_bessel4_state_t s1[2], s2[2];                 /* per L/R */
    float dline[2][EQ_MAX_DELAY];                    /* per L/R delay line for the dry/mid paths */
    int dpos, d1, d2;
    float old_low, old_mid, old_high;               /* last block's band gains (ramped) */
} lvmix_eq_t;

static void eq_design(lvmix_eq_t *e) {
    double r1, r2;
    e->d1 = rx_bessel4_int_delay_ratio(EQ_LO / RX_SR, EQ_MAX_DELAY, &r1);
    e->d2 = rx_bessel4_int_delay_ratio(EQ_HI / RX_SR, EQ_MAX_DELAY, &r2);
    rx_bessel4_lowpass(&e->c1, r1 * RX_SR, RX_SR);
    rx_bessel4_lowpass(&e->c2, r2 * RX_SR, RX_SR);
    e->old_low = 0; e->old_mid = 0; e->old_high = 1;  /* Mixxx's start state */
}
static void eq_reset(lvmix_eq_t *e) {
    memset(e->s1, 0, sizeof e->s1); memset(e->s2, 0, sizeof e->s2); memset(e->dline, 0, sizeof e->dline); e->dpos = 0;
}
/* in place on an interleaved stereo block; gains are the knob values (0..4, 1 = flat), kills force 0 */
static void eq_process(lvmix_eq_t *e, float *lr, int frames, double gl, double gm, double gh) {
    float fLow = (float)(gl - gm), fMid = (float)(gm - gh), fHigh = (float)gh;
    int need_low = fLow != 0 || e->old_low != 0, need_mid = fMid != 0 || e->old_mid != 0;
    float dl = e->old_low, dm = e->old_mid, dh = e->old_high;
    float il = (fLow - dl) / frames, im = (fMid - dm) / frames, ih = (fHigh - dh) / frames;
    int shift_mid = e->d1 - e->d2;   /* the mid path is delayed by d1 - d2 before its lowpass; the dry path by d1 */
    for (int i = 0; i < frames; i++) {
        dl += il; dm += im; dh += ih;
        for (int ch = 0; ch < 2; ch++) {
            float x = lr[2 * i + ch];
            float *line = e->dline[ch];
            line[e->dpos] = x;
            int p_hi = e->dpos - e->d1; if (p_hi < 0) p_hi += EQ_MAX_DELAY;
            int p_mid = e->dpos - shift_mid; if (p_mid < 0) p_mid += EQ_MAX_DELAY;
            float high = line[p_hi];
            float low = need_low ? rx_bessel4_tick(&e->c1, &e->s1[ch], x) : 0;
            float mid = need_mid ? rx_bessel4_tick(&e->c2, &e->s2[ch], line[p_mid]) : 0;
            lr[2 * i + ch] = low * dl + mid * dm + high * dh;
        }
        if (++e->dpos >= EQ_MAX_DELAY) e->dpos = 0;
    }
    e->old_low = fLow; e->old_mid = fMid; e->old_high = fHigh;
}

/* ------------------------------------------------------------------------------------------- the quick effect
 * Mixxx's FilterEffect on the QuickEffect super knob: below 0.5 a lowpass sweeping down (LinkedLeft: lpf corner from
 * 22050 Hz at 0.5 to 13 Hz at 0, logarithmic), above 0.5 a highpass sweeping up (LinkedRight: hpf from 13 Hz at 0.5 to
 * 22050 Hz at 1), Butterworth Q. Biquads (EngineFilterBiquad1Low/High) with per-block coefficient update. */
#define QE_MIN 13.0
#define QE_MAX 22050.0
typedef struct {
    rx_bq_coef_t lp, hp; rx_bq_state_t lps[2], hps[2];
    double lpf, hpf; int lp_on, hp_on;
} quick_fx_t;
static void qe_process(quick_fx_t *q, float *lr, int frames, double super) {
    double plp = rx_meta_link(super, RX_LINK_LEFT, 0, 1.0), php = rx_meta_link(super, RX_LINK_RIGHT, 0, 0.0);
    double lpf = rx_effect_log(plp, QE_MIN, QE_MAX), hpf = rx_effect_log(php, QE_MIN, QE_MAX);
    if (lpf >= QE_MAX * 0.999) lpf = QE_MAX; if (hpf <= QE_MIN * 1.001) hpf = QE_MIN;
    if (lpf != q->lpf) { q->lpf = lpf; rx_bq_lowpass(&q->lp, lpf, 0.707106781, RX_SR); }
    if (hpf != q->hpf) { q->hpf = hpf; rx_bq_highpass(&q->hp, hpf, 0.707106781, RX_SR); }
    int lp_on = lpf < QE_MAX, hp_on = hpf > QE_MIN;
    if (lp_on && !q->lp_on) memset(q->lps, 0, sizeof q->lps);
    if (hp_on && !q->hp_on) memset(q->hps, 0, sizeof q->hps);
    q->lp_on = lp_on; q->hp_on = hp_on;
    if (!lp_on && !hp_on) return;
    for (int i = 0; i < frames * 2; i++) {
        float x = lr[i]; int ch = i & 1;
        if (hp_on) x = rx_bq_tick(&q->hp, &q->hps[ch], x);
        if (lp_on) x = rx_bq_tick(&q->lp, &q->lps[ch], x);
        lr[i] = x;
    }
}

/* ------------------------------------------------------------------------------------------------------- VU */
typedef struct { float sum_l, sum_r; int n; float l, r; int peak_l, peak_r; } vu_t;
/* after Mixxx EngineVuMeter::process: mean |x| per ~1/30 s on a log scale, attack 1.0, decay 0.1 */
static void vu_process(vu_t *v, const float *lr, int frames) {
    float sl = 0, sr = 0; int clip_l = 0, clip_r = 0;
    for (int i = 0; i < frames; i++) {
        float a = fabsf(lr[2 * i]), b = fabsf(lr[2 * i + 1]);
        sl += a; sr += b; clip_l |= a > 1.0f; clip_r |= b > 1.0f;
    }
    v->sum_l += sl; v->sum_r += sr; v->n += frames;
    if (v->n > RX_SR / 30) {
        float nl = log10f(32767.0f * v->sum_l / (v->n * 1000.0f) + 1), nr = log10f(32767.0f * v->sum_r / (v->n * 1000.0f) + 1);
        v->l = nl > v->l ? nl : v->l - 0.1f * (v->l - nl);
        v->r = nr > v->r ? nr : v->r - 0.1f * (v->r - nr);
        v->l = rx_clampf(v->l, 0, 1); v->r = rx_clampf(v->r, 0, 1);
        v->sum_l = v->sum_r = 0; v->n = 0;
    }
    if (clip_l) v->peak_l = (int)(0.5 * RX_SR / RX_BLOCK); else if (v->peak_l > 0) v->peak_l--;
    if (clip_r) v->peak_r = (int)(0.5 * RX_SR / RX_BLOCK); else if (v->peak_r > 0) v->peak_r--;
}

/* ---------------------------------------------------------------------------------------------------- the mixer */
typedef struct {
    ctl_id volume, pregain, orientation, pfl, mute;
    ctl_id eq[3], eq_kill[3], eq_enabled, qe_super1, qe_enabled;
    ctl_id fx_enable[RX_FX_UNITS];
    ctl_id vu_meter, vu_l, vu_r, peak, peak_l, peak_r;
    lvmix_eq_t eq_state;
    quick_fx_t qe;
    vu_t vu;
    float gain_prev, fader_prev;
    int silent_blocks;
} channel_t;

struct rx_mixer {
    channel_t ch[RX_DECKS];
    ctl_id crossfader, gain, head_mix, head_volume, balance, xf_curve, xf_calibration, xf_mode, xf_reverse;
    ctl_id m_vu, m_vu_l, m_vu_r, m_peak, m_peak_l, m_peak_r, audio_latency_usage;
    ctl_id fx_master[RX_FX_UNITS];
    vu_t vu;
    float master_prev;
    float ch_vol_view[RX_DECKS];
};

rx_mixer_t *mixer_create(void) {
    rx_mixer_t *m = calloc(1, sizeof *m);
    for (int i = 0; i < RX_DECKS; i++) {
        eq_design(&m->ch[i].eq_state); eq_reset(&m->ch[i].eq_state);
        m->ch[i].qe.lpf = QE_MAX; m->ch[i].qe.hpf = QE_MIN;
        m->ch[i].gain_prev = 1; m->ch[i].fader_prev = 1;
    }
    m->master_prev = 1;
    return m;
}
void mixer_destroy(rx_mixer_t *m) { free(m); }

void mixer_register_controls(rx_mixer_t *m) {
    char g[48], n[32];
    for (int i = 0; i < RX_DECKS; i++) {
        channel_t *c = &m->ch[i];
        snprintf(g, sizeof g, "[Channel%d]", i + 1);
        c->volume = ctl_register(g, "volume", CK_FLOAT, 0, 1, 1);
        c->pregain = ctl_register(g, "pregain", CK_FLOAT, 0, 4, 1);
        c->orientation = ctl_register(g, "orientation", CK_INT, 0, 2, 1);
        c->pfl = ctl_register(g, "pfl", CK_TOGGLE, 0, 1, 0);
        c->mute = ctl_register(g, "mute", CK_TOGGLE, 0, 1, 0);
        c->vu_meter = ctl_register(g, "vu_meter", CK_FLOAT, 0, 1, 0);
        c->vu_l = ctl_register(g, "vu_meter_left", CK_FLOAT, 0, 1, 0);
        c->vu_r = ctl_register(g, "vu_meter_right", CK_FLOAT, 0, 1, 0);
        c->peak = ctl_register(g, "peak_indicator", CK_TOGGLE, 0, 1, 0);
        c->peak_l = ctl_register(g, "peak_indicator_left", CK_TOGGLE, 0, 1, 0);
        c->peak_r = ctl_register(g, "peak_indicator_right", CK_TOGGLE, 0, 1, 0);
        snprintf(g, sizeof g, "[EqualizerRack1_[Channel%d]_Effect1]", i + 1);
        for (int b = 0; b < 3; b++) {
            snprintf(n, sizeof n, "parameter%d", b + 1); c->eq[b] = ctl_register(g, n, CK_FLOAT, 0, 4, 1);
            snprintf(n, sizeof n, "button_parameter%d", b + 1); c->eq_kill[b] = ctl_register(g, n, CK_TOGGLE, 0, 1, 0);
        }
        c->eq_enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 1);
        snprintf(g, sizeof g, "[QuickEffectRack1_[Channel%d]]", i + 1);
        c->qe_super1 = ctl_register(g, "super1", CK_FLOAT, 0, 1, 0.5);
        c->qe_enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 1);
        for (int u = 0; u < RX_FX_UNITS; u++) {
            snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d]", u + 1);
            snprintf(n, sizeof n, "group_[Channel%d]_enable", i + 1);
            c->fx_enable[u] = ctl_register(g, n, CK_TOGGLE, 0, 1, 0);
        }
    }
    m->crossfader = ctl_register("[Master]", "crossfader", CK_FLOAT, -1, 1, 0);
    m->gain = ctl_register("[Master]", "gain", CK_FLOAT, 0, 4, 1);
    m->head_mix = ctl_register("[Master]", "headMix", CK_FLOAT, -1, 1, 0);
    m->head_volume = ctl_register("[Master]", "headGain", CK_FLOAT, 0, 4, 1);
    m->balance = ctl_register("[Master]", "balance", CK_FLOAT, -1, 1, 0);
    m->xf_curve = ctl_register("[Mixer Profile]", "xFaderCurve", CK_FLOAT, 0.6, 1000, 1);     /* EngineXfader::kTransformDefault */
    m->xf_calibration = ctl_register("[Mixer Profile]", "xFaderCalibration", CK_FLOAT, 0, 1, 0);
    m->xf_mode = ctl_register("[Mixer Profile]", "xFaderMode", CK_INT, 0, 1, 0);               /* 0 = slow fade / fast cut (MIXXX_XFADER_ADDITIVE) */
    m->xf_reverse = ctl_register("[Mixer Profile]", "xFaderReverse", CK_TOGGLE, 0, 1, 0);
    m->m_vu = ctl_register("[Master]", "vu_meter", CK_FLOAT, 0, 1, 0);
    m->m_vu_l = ctl_register("[Master]", "vu_meter_left", CK_FLOAT, 0, 1, 0);
    m->m_vu_r = ctl_register("[Master]", "vu_meter_right", CK_FLOAT, 0, 1, 0);
    m->m_peak = ctl_register("[Master]", "peak_indicator", CK_TOGGLE, 0, 1, 0);
    m->m_peak_l = ctl_register("[Master]", "peak_indicator_left", CK_TOGGLE, 0, 1, 0);
    m->m_peak_r = ctl_register("[Master]", "peak_indicator_right", CK_TOGGLE, 0, 1, 0);
    m->audio_latency_usage = ctl_register("[Master]", "audio_latency_usage", CK_FLOAT, 0, 1, 0);
    for (int u = 0; u < RX_FX_UNITS; u++) {
        snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d]", u + 1);
        m->fx_master[u] = ctl_register(g, "group_[Master]_enable", CK_TOGGLE, 0, 1, 0);
    }
}

/* after Mixxx EngineXfader::getXfadeGains */
static void xfade_gains(double pos, double transform, double calibration, int constpwr, int reverse, float *g1, float *g2) {
    double left = pos, right = pos;
    if (constpwr) { pos *= calibration; left = pos - calibration; right = pos + calibration; }
    if (left < 0) { left = -left; *g2 = (float)(1.0 - pow(left, transform)); } else *g2 = 1.0f;
    if (right > 0) *g1 = (float)(1.0 - pow(right, transform)); else *g1 = 1.0f;
    if (*g1 < 0) *g1 = 0; if (*g2 < 0) *g2 = 0;
    if (constpwr) {
        if (*g1 > *g2) *g2 = 1 - *g1; else *g1 = 1 - *g2;
        float g = sqrtf(*g1 * *g1 + *g2 * *g2);
        if (g > 0) { *g1 /= g; *g2 /= g; }
    }
    if (reverse) { float t = *g1; *g1 = *g2; *g2 = t; }
}

void mixer_render(rx_mixer_t *m, float *deck_lr[RX_DECKS], float *out_lr, int frames, rx_fx_t *fx) {
    float xf_left, xf_right;
    double transform = ctl_get(m->xf_curve); if (transform < 0.6) transform = 0.6; if (transform > 1000) transform = 1000;
    int constpwr = ctl_get(m->xf_mode) >= 0.5;
    double calibration = pow(0.5, 1.0 / transform);     /* EngineXfader::getPowerCalibration */
    xfade_gains(ctl_get(m->crossfader), transform, calibration, constpwr, ctl_get(m->xf_reverse) >= 0.5, &xf_left, &xf_right);

    memset(out_lr, 0, sizeof(float) * 2 * (size_t)frames);
    for (int i = 0; i < RX_DECKS; i++) {
        channel_t *c = &m->ch[i];
        float *lr = deck_lr[i];
        /* skip silent channels entirely (a stopped deck): keeps the per-block cost low */
        int silent = 1;
        for (int k = 0; k < frames * 2; k += 7) if (lr[k] != 0) { silent = 0; break; }
        if (silent) c->silent_blocks++; else c->silent_blocks = 0;
        if (c->silent_blocks > 16 && ctl_get(c->fx_enable[0]) < 0.5 && ctl_get(c->fx_enable[1]) < 0.5) {
            /* nothing to hear and no effect tail to flush: skip the EQ/filter (their states have decayed) */
            c->fader_prev = m->ch_vol_view[i] = 0; eq_reset(&c->eq_state); c->eq_state.old_low = 0; c->eq_state.old_mid = 0; c->eq_state.old_high = 1;
            memset(c->qe.lps, 0, sizeof c->qe.lps); memset(c->qe.hps, 0, sizeof c->qe.hps);
            vu_process(&c->vu, lr, frames);
            continue;
        }
        /* pregain (EnginePregain's knob, ramped) */
        float g = (float)rx_clamp(ctl_get(c->pregain), 0, 4);
        rx_apply_ramp(lr, frames, c->gain_prev, g);
        c->gain_prev = g;
        /* EQ */
        if (ctl_get(c->eq_enabled) >= 0.5) {
            double gl = ctl_get(c->eq_kill[0]) >= 0.5 ? 0 : rx_clamp(ctl_get(c->eq[0]), 0, 4);
            double gm = ctl_get(c->eq_kill[1]) >= 0.5 ? 0 : rx_clamp(ctl_get(c->eq[1]), 0, 4);
            double gh = ctl_get(c->eq_kill[2]) >= 0.5 ? 0 : rx_clamp(ctl_get(c->eq[2]), 0, 4);
            if (gl != 1 || gm != 1 || gh != 1 || c->eq_state.old_low != 0 || c->eq_state.old_mid != 0 || c->eq_state.old_high != 1)
                eq_process(&c->eq_state, lr, frames, gl, gm, gh);
        }
        /* quick effect (filter on super1) */
        if (ctl_get(c->qe_enabled) >= 0.5) qe_process(&c->qe, lr, frames, ctl_get(c->qe_super1));
        /* effect units enabled on this channel */
        if (fx) fx_process_channel(fx, i, lr, frames);
        /* fader x crossfader (orientation: 0 left, 1 centre, 2 right) x mute */
        int o = (int)ctl_get(c->orientation);
        float xf = o == 0 ? xf_left : o == 2 ? xf_right : 1.0f;
        float fader = (float)rx_clamp(ctl_get(c->volume), 0, 1) * xf * (ctl_get(c->mute) >= 0.5 ? 0.0f : 1.0f);
        rx_apply_ramp(lr, frames, c->fader_prev, fader);
        c->fader_prev = fader;
        m->ch_vol_view[i] = fader;
        vu_process(&c->vu, lr, frames);
        for (int k = 0; k < frames * 2; k++) out_lr[k] += lr[k];
    }
    /* master effects, master gain, balance, limiter */
    if (fx) fx_process_master(fx, out_lr, frames);
    float mg = (float)rx_clamp(ctl_get(m->gain), 0, 4);
    rx_apply_ramp(out_lr, frames, m->master_prev, mg);
    m->master_prev = mg;
    float bal = (float)rx_clamp(ctl_get(m->balance), -1, 1);
    if (bal != 0) {
        float bl = bal > 0 ? 1 - bal : 1, br = bal < 0 ? 1 + bal : 1;
        for (int k = 0; k < frames; k++) { out_lr[2 * k] *= bl; out_lr[2 * k + 1] *= br; }
    }
    for (int k = 0; k < frames * 2; k++) out_lr[k] = rx_softclip(out_lr[k]);
    vu_process(&m->vu, out_lr, frames);
    /* readback controls (cheap: once every 4 blocks) */
    static int tick;
    if ((++tick & 3) == 0) {
        for (int i = 0; i < RX_DECKS; i++) {
            channel_t *c = &m->ch[i];
            ctl_set(c->vu_l, c->vu.l); ctl_set(c->vu_r, c->vu.r); ctl_set(c->vu_meter, (c->vu.l + c->vu.r) * 0.5f);
            ctl_set(c->peak_l, c->vu.peak_l > 0); ctl_set(c->peak_r, c->vu.peak_r > 0); ctl_set(c->peak, c->vu.peak_l > 0 || c->vu.peak_r > 0);
        }
        ctl_set(m->m_vu_l, m->vu.l); ctl_set(m->m_vu_r, m->vu.r); ctl_set(m->m_vu, (m->vu.l + m->vu.r) * 0.5f);
        ctl_set(m->m_peak_l, m->vu.peak_l > 0); ctl_set(m->m_peak_r, m->vu.peak_r > 0); ctl_set(m->m_peak, m->vu.peak_l > 0 || m->vu.peak_r > 0);
    }
}

void mixer_view(rx_mixer_t *m, rx_view_t *v) {
    v->master_vu_l = m->vu.l; v->master_vu_r = m->vu.r;
    v->xfader = ctl_get(m->crossfader);
    for (int i = 0; i < RX_DECKS; i++) {
        v->ch_vol[i] = (float)rx_clamp(ctl_get(m->ch[i].volume), 0, 1);
        v->deck[i].vu_l = m->ch[i].vu.l;           /* post-fader (the deck filled its pre-fader level before) */
        v->deck[i].vu_r = m->ch[i].vu.r;
    }
}
