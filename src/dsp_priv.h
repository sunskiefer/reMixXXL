/* reMixXXL -- small DSP helpers shared by deck.c, mixer.c and fx.c (header-only, static inline).
 *
 * Biquads (RBJ cookbook), Bessel-4 lowpass design after Mixxx src/engine/filters/enginefilterbessel4.cpp (fidlib
 * "LpBe4": the -3 dB normalised 4th-order Bessel), Mixxx's knob scalers (ControlLogPotmeterBehavior and
 * ControlAudioTaperPotBehavior from src/control/controlbehavior.cpp) and a few maths bits.
 * GPL-2.0-or-later designs, used in a GPL-3 plugin. */
#pragma once
#include <math.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float rx_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double rx_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double rx_db2ratio(double db) { return pow(10.0, db / 20.0); }
static inline double rx_ratio2db(double r) { return 20.0 * log10(r); }
/* Mixxx util/math.h roundToFraction */
static inline double rx_round_to_fraction(double v, int denominator) {
    return round(v * denominator) / denominator;
}
/* Mixxx EngineFilterMoogLadder4::tanh_approx */
static inline float rx_tanh_approx(float x) { return x / (1 + x * x / (3 + x * x / 5)); }
/* a cheap soft clipper for the master: linear to +-0.9, then a smooth knee (never above 1.0) */
static inline float rx_softclip(float x) {
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    float t = (a - 0.9f) * 10.0f;              /* 0.. */
    float y = 0.9f + 0.1f * (1.0f - 1.0f / (1.0f + t));   /* approaches 1.0 */
    return x < 0 ? -y : y;
}

/* ----------------------------------------------------------------------------------------- biquad (transposed DF2) */
typedef struct { float b0, b1, b2, a1, a2; } rx_bq_coef_t;
typedef struct { float z1, z2; } rx_bq_state_t;

static inline float rx_bq_tick(const rx_bq_coef_t *c, rx_bq_state_t *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}
static inline void rx_bq_lowpass(rx_bq_coef_t *c, double fc, double q, double sr) {
    double w0 = 2 * M_PI * rx_clamp(fc, 1.0, sr * 0.499) / sr, cw = cos(w0), sw = sin(w0), al = sw / (2 * q);
    double a0 = 1 + al;
    c->b0 = (float)((1 - cw) / 2 / a0); c->b1 = (float)((1 - cw) / a0); c->b2 = c->b0;
    c->a1 = (float)(-2 * cw / a0); c->a2 = (float)((1 - al) / a0);
}
static inline void rx_bq_highpass(rx_bq_coef_t *c, double fc, double q, double sr) {
    double w0 = 2 * M_PI * rx_clamp(fc, 1.0, sr * 0.499) / sr, cw = cos(w0), sw = sin(w0), al = sw / (2 * q);
    double a0 = 1 + al;
    c->b0 = (float)((1 + cw) / 2 / a0); c->b1 = (float)(-(1 + cw) / a0); c->b2 = c->b0;
    c->a1 = (float)(-2 * cw / a0); c->a2 = (float)((1 - al) / a0);
}

/* ----------------------------------------------------------------------------------------- Bessel 4 lowpass (2 biquads)
 * The Bessel-4 polynomial s^4+10s^3+45s^2+105s+105 has its -3 dB point at w3 = 2.11392; dividing its poles by w3
 * puts -3 dB at w = 1: the pairs become (-0.99521 +- 1.25711j) and (-1.37007 +- 0.41025j). Each pair is a 2nd-order
 * lowpass section with w0 = |p| * 2 pi fc and Q = |p| / (2 |Re p|); the pass-band group delay is 0.33644 / fc, which
 * is Mixxx's kDelayFactor1. */
typedef struct { rx_bq_coef_t s[2]; } rx_bessel4_coef_t;
typedef struct { rx_bq_state_t s[2]; } rx_bessel4_state_t;
static inline void rx_bessel4_lowpass(rx_bessel4_coef_t *c, double fc, double sr) {
    rx_bq_lowpass(&c->s[0], fc * 1.6033575162, 0.8055382818, sr);
    rx_bq_lowpass(&c->s[1], fc * 1.4301715600, 0.5219345817, sr);
}
static inline float rx_bessel4_tick(const rx_bessel4_coef_t *c, rx_bessel4_state_t *s, float x) {
    return rx_bq_tick(&c->s[1], &s->s[1], rx_bq_tick(&c->s[0], &s->s[0], x));
}
/* after Mixxx EngineFilterBessel4Low::setFrequencyCornersForIntDelay: picks a corner near the wanted ratio
 * (fc / sr) whose pass-band group delay is an integer number of frames; returns that delay, writes the ratio */
static inline int rx_bessel4_int_delay_ratio(double desired_ratio, int max_delay, double *ratio_out) {
    const double kDelayFactor1 = 0.336440447;   /* group delay at 1 Hz corner, 1 Hz sample rate */
    const double kDelayFactor2 = 1.1044845;
    static const double table[] = {
        0.500000000, 0.258899546, 0.154778862, 0.107833769, 0.082235025, 0.066314175, 0.055505336,
        0.047691446, 0.041813481, 0.037212241, 0.033519902, 0.030497945, 0.027964718 };
    double d = kDelayFactor1 / desired_ratio - kDelayFactor2 * desired_ratio;
    int idelay = (int)(d + 0.5);
    if (idelay < 0) idelay = 0; else if (idelay > max_delay) idelay = max_delay;
    if (idelay >= (int)(sizeof(table) / sizeof(table[0]))) {
        double h = idelay / kDelayFactor2 / 2;
        *ratio_out = -h + sqrt(h * h + kDelayFactor1 / kDelayFactor2);
    } else {
        *ratio_out = table[idelay];
    }
    return idelay;
}

/* ----------------------------------------------------------------------------------------- Mixxx knob scalers
 * All take the 0..1 "parameter" (what a MIDI knob or our params give) and return the effect's value. */
/* ControlLogPotmeterBehavior(min, max, -40 dB) */
static inline double rx_logpot(double p, double lo, double hi) {
    const double minDB = -40.0, minOffset = 0.01;   /* db2ratio(-40) */
    p = rx_clamp(p, 0, 1);
    double db = (1 - p) * minDB;
    double lin = (rx_db2ratio(db) - minOffset) / (1 - minOffset);
    return lo + lin * (hi - lo);
}
static inline double rx_logpot_inv(double p, double lo, double hi) { return rx_logpot(1 - p, lo, hi); }
/* ControlAudioTaperPotBehavior(minDB, maxDB, neutral): 0 at p=0, 1.0 at p=neutral, db2ratio(maxDB) at p=1 */
static inline double rx_audiotaper(double p, double minDB, double maxDB, double neutral) {
    double offset = rx_db2ratio(minDB);
    if (p <= 0) return 0;
    if (p < neutral) {
        double db = (p * minDB / (neutral * -1)) + minDB;
        return (rx_db2ratio(db) - offset) / (1 - offset);
    }
    if (p == neutral) return 1.0;
    if (p < 1.0) return rx_db2ratio((p - neutral) * maxDB / (1 - neutral));
    return rx_db2ratio(maxDB);
}
/* ControlEffectKnob::setBehaviour for ValueScaler::Logarithmic with the manifest's (min, max) */
static inline double rx_effect_log(double p, double lo, double hi) {
    if (lo == 0) {
        if (hi == 1.0) return rx_audiotaper(p, -20, 0, 1);
        if (hi > 1.0) return rx_audiotaper(p, -12, rx_ratio2db(hi), 0.5);
    }
    return rx_logpot(p, lo, hi);
}
static inline double rx_effect_lin(double p, double lo, double hi) { return lo + rx_clamp(p, 0, 1) * (hi - lo); }

/* Mixxx EffectKnobParameterSlot::onEffectMetaParameterChanged: how a slot's meta knob (0..1) drives a linked
 * parameter (parameter space 0..1). Returns -1 when the link type leaves the parameter alone. */
enum { RX_LINK_NONE, RX_LINK_LINKED, RX_LINK_LEFT, RX_LINK_RIGHT, RX_LINK_LEFTRIGHT };
static inline double rx_meta_link(double meta, int type, int inverse, double neutral) {
    double p = meta;
    switch (type) {
    case RX_LINK_LINKED:
        if (neutral > 0.0 && neutral < 1.0) {
            if (inverse) neutral = 1.0 - neutral;
            if (p <= 0.5) p = p / 0.5 * neutral;
            else p = (p - 0.5) / 0.5 * (1 - neutral) + neutral;
        }
        break;
    case RX_LINK_LEFT:  p = p >= 0.5 ? 1.0 : p * 2; break;
    case RX_LINK_RIGHT: p = p >= 0.5 ? (p - 0.5) * 2 : 0.0; break;
    case RX_LINK_LEFTRIGHT: p = p >= 0.5 ? (p - 0.5) * 2 : 1.0 - p * 2; break;
    default: return -1;
    }
    if (inverse) p = 1.0 - p;
    return p;
}

/* a linear per-block gain ramp (Mixxx SampleUtil::applyRampingGain) */
static inline void rx_apply_ramp(float *lr, int frames, float g0, float g1) {
    if (g0 == g1) {
        if (g0 == 1.0f) return;
        for (int i = 0; i < frames * 2; i++) lr[i] *= g0;
        return;
    }
    float d = (g1 - g0) / (float)frames, g = g0;
    for (int i = 0; i < frames; i++) { g += d; lr[2 * i] *= g; lr[2 * i + 1] *= g; }
}
