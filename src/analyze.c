/* reMixXXL -- analyze.c: waveform summary, overview, BPM + first beat, replay gain.  Runs on the loading thread.
 *
 * One pass over the int16 stereo track (DEC_CHUNK frames at a time, so the whole track is never copied to float):
 *
 * 1. Waveform summary -- Mixxx's AnalyzerWaveform (src/analyzer/analyzerwaveform.cpp):
 *      three bands from the stereo signal through EngineFilterBessel4Low(600 Hz), Bessel4Band(600..4000 Hz),
 *      Bessel4High(4000 Hz) (kLowMidFreqHz / kMidHighFreqHz), per stride the peak |sample| of each band and of the
 *      unfiltered signal (WaveformStride::store: "Take max value, not average"), stored as
 *      min(255, 255 * peak + 0.5) (m_postScaleConversion = 255).  Mixxx keeps left and right apart (the display
 *      draws L up, R down); rx_wave_col_t has one value per band, so a column holds max(L, R).
 *      Mixxx's stride is sampleRate / 441 frames (mainWaveformSampleRate = 441); ours is sampleRate /
 *      RX_WAVE_PX_PER_SEC (400 columns per second, 110.25 frames), the column index being floor(frame / stride),
 *      which is what Mixxx's "fmod(position, length) < 1" boundary amounts to.
 *      Mixxx's filters come from fidlib ("LpBe4", "BpBe4", "HpBe4"), which is not in the checkout, so the same
 *      filters are designed here: 4th-order Bessel prototype (-3 dB normalised poles), bilinear transform with
 *      frequency pre-warping, the band-pass by the s -> (s^2 + w0^2) / (B s) substitution (8th order, like
 *      EngineFilterIIR<8, IIR_BP>), unity gain at DC / Nyquist / the geometric band centre like fidlib.
 *
 * 2. Overview: RX_OVERVIEW_W columns over the whole track, each the max of the fine columns in its bin (Mixxx's
 *      summary waveform averages the strides, WaveformStride::averageStore; the overview strip here is meant to
 *      show transients, so it takes the max as the contract says).
 *
 * 3. BPM + first beat: the mono downmix (L + R) / 2 at the track rate goes to the Queen Mary beat tracker in
 *      analyze_qm.cpp (DetectionFunction COMPLEXSD + TempoTrackV2, exactly as analyzerqueenmarybeats.cpp), then
 *      the beat list is turned into a constant grid the way Mixxx does with the "fixed tempo assumption" on
 *      (BeatFactory::makePreferredBeats -> BeatUtils::retrieveConstRegions, makeConstBpm, adjustPhase; ported
 *      below from src/track/beatutils.cpp with the same constants).
 *
 * 4. Replay gain: not Mixxx's EBU R128 (libebur128 is not vendored).  A gated RMS: the mean square of 400 ms
 *      blocks whose level is above -70 dBFS is averaged, that loudness is brought to -14 dBFS RMS (about the
 *      -14 LUFS streaming norm; Mixxx's own target is the ReplayGain 89 dB = -18 LUFS, our mixer's headroom suits
 *      the hotter -14), the gain clamped to -20..+20 dB.  gain = 1 when the track is silent.
 *
 * Progress: 0..70 during the pass, 70..95 while the tempo tracker runs, 100 at the end. */

#include "remix.h"

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* analyze_qm.cpp (extern "C") */
void *rx_qm_begin(int sr);
int rx_qm_feed(void *h, const float *mono, int n);
int rx_qm_end(void *h, double *out_beats_sec, int max_beats);
void rx_qm_free(void *h);
int rx_qm_beats(const float *mono, int n, int sr, double *out_beats_sec, int max_beats);

#define AN_CHUNK 4096
#define AN_LOW_HZ 600.0                      /* analyzerwaveform.cpp kLowMidFreqHz */
#define AN_HIGH_HZ 4000.0                    /* kMidHighFreqHz */
#define AN_MAX_BEATS 16384                   /* 15 min at 240 BPM is 3600 beats; plenty */

/* ------------------------------------------------------------------------------------------------ IIR biquads */
typedef struct { double b0, b1, b2, a1, a2; double z1, z2; } biquad_t;   /* DF2 transposed, a0 = 1 */

typedef struct { biquad_t s[4]; int n; } iir_t;                           /* up to 4 sections (8th order) */

static inline double biquad_run(biquad_t *q, double x) {
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static inline double iir_run(iir_t *f, double x) {
    for (int i = 0; i < f->n; i++) x = biquad_run(&f->s[i], x);
    return x;
}

/* bilinear transform of the analog section (A s^2 + B s + C) / (D s^2 + E s + F), s = (1 - z^-1) / (1 + z^-1)
 * (the pre-warp constant K is already folded into the analog coefficients by the caller) */
static void bilinear(biquad_t *q, double A, double B, double C, double D, double E, double F) {
    double b0 = A + B + C, b1 = -2 * A + 2 * C, b2 = A - B + C;
    double a0 = D + E + F, a1 = -2 * D + 2 * F, a2 = D - E + F;
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
    q->a1 = a1 / a0; q->a2 = a2 / a0;
    q->z1 = q->z2 = 0;
}

/* |H(e^jw)| of the cascade */
static double iir_gain(const iir_t *f, double w) {
    double complex z = cexp(-I * w);
    double g = 1;
    for (int i = 0; i < f->n; i++) {
        const biquad_t *q = &f->s[i];
        double complex num = q->b0 + q->b1 * z + q->b2 * z * z;
        double complex den = 1.0 + q->a1 * z + q->a2 * z * z;
        g *= cabs(num / den);
    }
    return g;
}

static void iir_scale(iir_t *f, double k) { f->s[0].b0 *= k; f->s[0].b1 *= k; f->s[0].b2 *= k; }

/* 4th-order Bessel prototype, poles normalised for -3 dB at 1 rad/s (the standard table fidlib builds its
 * Bessel filters from): -1.3700678 +- 0.4102497j, -0.9952088 +- 1.2571057j */
static const double complex k_bessel4[2] = { -1.37006783055 + 0.41024971749 * I, -0.99520876435 + 1.25710573945 * I };

static void iir_bessel4_low(iir_t *f, double fs, double fc) {
    double K = tan(M_PI * fc / fs);
    f->n = 2;
    for (int i = 0; i < 2; i++) {
        double complex p = k_bessel4[i];
        double m2 = creal(p * conj(p));
        /* H(s) = K^2 |p|^2 / (s^2 - 2 Re(p) K s + K^2 |p|^2) */
        bilinear(&f->s[i], 0, 0, K * K * m2, 1, -2 * creal(p) * K, K * K * m2);
    }
    iir_scale(f, 1.0 / iir_gain(f, 0.0));
}

static void iir_bessel4_high(iir_t *f, double fs, double fc) {
    double K = tan(M_PI * fc / fs);
    f->n = 2;
    for (int i = 0; i < 2; i++) {
        double complex p = k_bessel4[i];
        double m2 = creal(p * conj(p));
        /* H(s) = H_lp(K / s) = |p|^2 s^2 / (|p|^2 s^2 - 2 Re(p) K s + K^2) */
        bilinear(&f->s[i], m2, 0, 0, m2, -2 * creal(p) * K, K * K);
    }
    iir_scale(f, 1.0 / iir_gain(f, M_PI));
}

static void iir_bessel4_band(iir_t *f, double fs, double f1, double f2) {
    double K1 = tan(M_PI * f1 / fs), K2 = tan(M_PI * f2 / fs);
    double w0sq = K1 * K2, B = K2 - K1;
    f->n = 4;
    int n = 0;
    for (int i = 0; i < 2; i++) {
        double complex p = k_bessel4[i];
        /* each prototype pole p maps to the two roots of s^2 - p B s + w0^2 = 0; the conjugate pole gives their
         * conjugates, so (r, r*) form real second-order sections with numerator B s */
        double complex disc = csqrt(p * p * B * B - 4.0 * w0sq);
        double complex r[2] = { (p * B + disc) / 2.0, (p * B - disc) / 2.0 };
        for (int k = 0; k < 2; k++) {
            double m2 = creal(r[k] * conj(r[k]));
            bilinear(&f->s[n++], 0, B, 0, 1, -2 * creal(r[k]), m2);
        }
    }
    double wc = 2 * M_PI * sqrt(f1 * f2) / fs;
    iir_scale(f, 1.0 / iir_gain(f, wc));
}

/* ------------------------------------------------------------------------------------------------ BeatUtils port */
/* src/track/beatutils.cpp, frames at RX_SR */
#define BU_MAX_SECS_PHASE_ERROR 0.025        /* kMaxSecsPhaseError */
#define BU_MAX_SECS_PHASE_ERROR_SUM 0.1      /* kMaxSecsPhaseErrorSum */
#define BU_MAX_OUTLIERS 1                    /* kMaxOutliersCount */
#define BU_MIN_REGION_BEATS 16               /* kMinRegionBeatCount */

typedef struct { double first_beat, beat_length; } const_region_t;

/* BeatUtils::retrieveConstRegions: returns the number of regions written (the last one has beat_length 0) */
static int bu_const_regions(const double *beats, int nbeats, double sr, const_region_t *out, int max_out) {
    if (nbeats < 2) return 0;
    const double max_phase_error = BU_MAX_SECS_PHASE_ERROR * sr;
    const double max_phase_error_sum = BU_MAX_SECS_PHASE_ERROR_SUM * sr;
    int left = 0, right = nbeats - 1, n = 0;
    while (left < nbeats - 1) {
        double mean = (beats[right] - beats[left]) / (right - left);
        int outliers = 0;
        double ironed = beats[left], err_sum = 0;
        int i = left + 1;
        for (; i <= right; ++i) {
            ironed += mean;
            double err = ironed - beats[i];
            err_sum += err;
            if (fabs(err) > max_phase_error) {
                outliers++;
                if (outliers > BU_MAX_OUTLIERS || i == left + 1) break;   /* the first beat must not be an outlier */
            }
            if (fabs(err_sum) > max_phase_error_sum) break;              /* drifting: mean not optimal */
        }
        if (i > right) {
            double border_err = 0;
            if (right > left + 2) {
                double first_len = beats[left + 1] - beats[left];
                double last_len = beats[right] - beats[right - 1];
                border_err = fabs(first_len + last_len - 2 * mean);
            }
            if (border_err < max_phase_error / 2) {
                if (n < max_out - 1) { out[n].first_beat = beats[left]; out[n].beat_length = mean; n++; }
                left = right;
                right = nbeats - 1;
                continue;
            }
        }
        right--;
    }
    if (n < max_out) { out[n].first_beat = beats[nbeats - 1]; out[n].beat_length = 0; n++; }
    return n;
}

static int bu_try_snap(double lo, double centre, double hi, double fraction, double *out) {
    double s = round(centre * fraction) / fraction;
    if (s > lo && s < hi) { *out = s; return 1; }
    return 0;
}

/* BeatUtils::roundBpmWithinRange */
static double bu_round_bpm(double lo, double centre, double hi) {
    double s;
    if (bu_try_snap(lo, centre, hi, 1.0, &s)) return s;
    if (centre < 85.0 && bu_try_snap(lo, centre, hi, 2.0, &s)) return s;
    if (centre > 127.0 && bu_try_snap(lo, centre, hi, 2.0 / 3.0, &s)) return s;
    if (bu_try_snap(lo, centre, hi, 3.0, &s)) return s;
    if (bu_try_snap(lo, centre, hi, 12.0, &s)) return s;
    return centre;
}

/* BeatUtils::makeConstBpm: returns the BPM (0 if none) and the first beat (frames) */
static double bu_make_const_bpm(const const_region_t *reg, int nreg, double sr, double *first_beat) {
    if (nreg < 1) return 0;
    int mid = 0;
    double longest_len = 0, longest_beat = 0;
    for (int i = 0; i < nreg - 1; ++i) {
        double len = reg[i + 1].first_beat - reg[i].first_beat;
        if (len > longest_len) { longest_len = len; longest_beat = reg[i].beat_length; mid = i; }
    }
    if (longest_len == 0 || longest_beat <= 0) return 0;

    int longest_n = (int)(longest_len / longest_beat + 0.5);
    double longest_min = longest_beat - (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;
    double longest_max = longest_beat + (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;
    int start = mid;

    /* a region at the start with similar tempo and phase */
    for (int i = 0; i < mid; ++i) {
        double len = reg[i + 1].first_beat - reg[i].first_beat;
        int nb = (int)(len / reg[i].beat_length + 0.5);
        if (nb < BU_MIN_REGION_BEATS) continue;
        double this_min = reg[i].beat_length - (BU_MAX_SECS_PHASE_ERROR * sr) / nb;
        double this_max = reg[i].beat_length + (BU_MAX_SECS_PHASE_ERROR * sr) / nb;
        if (longest_beat > this_min && longest_beat < this_max) {
            double new_len = reg[mid + 1].first_beat - reg[i].first_beat;
            double bl_min = fmax(longest_min, this_min), bl_max = fmin(longest_max, this_max);
            int max_n = (int)round(new_len / bl_min), min_n = (int)round(new_len / bl_max);
            if (min_n != max_n) continue;
            double new_beat = new_len / min_n;
            if (new_beat > longest_min && new_beat < longest_max) {
                longest_len = new_len; longest_beat = new_beat; longest_n = min_n;
                longest_min = longest_beat - (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;
                longest_max = longest_beat + (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;
                start = i;
                break;
            }
        }
    }
    /* a region at the end with similar tempo and phase */
    for (int i = nreg - 2; i > mid; --i) {
        double len = reg[i + 1].first_beat - reg[i].first_beat;
        int nb = (int)(len / reg[i].beat_length + 0.5);
        if (nb < BU_MIN_REGION_BEATS) continue;
        double this_min = reg[i].beat_length - (BU_MAX_SECS_PHASE_ERROR * sr) / nb;
        double this_max = reg[i].beat_length + (BU_MAX_SECS_PHASE_ERROR * sr) / nb;
        if (longest_beat > this_min && longest_beat < this_max) {
            double new_len = reg[i + 1].first_beat - reg[start].first_beat;
            double bl_min = fmax(longest_min, this_min), bl_max = fmin(longest_max, this_max);
            int max_n = (int)round(new_len / bl_min), min_n = (int)round(new_len / bl_max);
            if (min_n != max_n) continue;
            double new_beat = new_len / min_n;
            if (new_beat > longest_min && new_beat < longest_max) {
                longest_len = new_len; longest_beat = new_beat; longest_n = min_n;
                break;
            }
        }
    }
    longest_min = longest_beat - (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;
    longest_max = longest_beat + (BU_MAX_SECS_PHASE_ERROR * sr) / longest_n;

    double min_bpm = 60.0 * sr / longest_max;
    double max_bpm = 60.0 * sr / longest_min;
    double centre_bpm = 60.0 * sr / longest_beat;
    double bpm = bu_round_bpm(min_bpm, centre_bpm, max_bpm);
    if (!(bpm > 0)) return 0;
    if (first_beat) {
        /* "Move the first beat as close to the start of the track as we can" */
        double rounded_len = 60.0 * sr / bpm;
        *first_beat = fmod(reg[start].first_beat, rounded_len);
    }
    return bpm;
}

/* BeatUtils::adjustPhase: the average offset of the detected beats from the grid, within the tolerance */
static double bu_adjust_phase(double first_beat, double bpm, double sr, const double *beats, int nbeats) {
    if (!(bpm > 0)) return first_beat;
    double beat_len = 60.0 * sr / bpm;
    double start_off = fmod(first_beat, beat_len);
    double adjust = 0, count = 0;
    for (int i = 0; i < nbeats; i++) {
        double off = fmod(beats[i] - start_off, beat_len);
        if (off > beat_len / 2) off -= beat_len;
        if (fabs(off) < BU_MAX_SECS_PHASE_ERROR * sr) { adjust += off; count++; }
    }
    if (count > 0) adjust /= count; else adjust = 0;
    return first_beat + adjust;
}

/* the whole Mixxx path from a beat list (seconds) to (bpm, beat0 seconds); BeatFactory::makePreferredBeats with
 * fixedTempo = true, falling back to calculateAverageBpm for very short lists as BeatUtils::calculateBpm does */
static void beats_to_grid(const double *beats_sec, int nbeats, double *bpm_out, double *beat0_out) {
    *bpm_out = 0; *beat0_out = 0;
    if (nbeats < 2) return;
    const double sr = RX_SR;
    double *beats = malloc((size_t)nbeats * sizeof(double));
    if (!beats) return;
    for (int i = 0; i < nbeats; i++) beats[i] = beats_sec[i] * sr;

    if (nbeats < BU_MIN_REGION_BEATS) {      /* BeatUtils::calculateAverageBpm */
        double frames = beats[nbeats - 1] - beats[0];
        if (frames > 0) {
            double bpm = 60.0 * (nbeats - 1) * sr / frames;
            *bpm_out = bpm;
            *beat0_out = fmod(beats[0], 60.0 * sr / bpm) / sr;
        }
        free(beats);
        return;
    }

    const_region_t *reg = malloc((size_t)(nbeats + 1) * sizeof(const_region_t));
    if (!reg) { free(beats); return; }
    int nreg = bu_const_regions(beats, nbeats, sr, reg, nbeats + 1);
    double first = 0;
    double bpm = nreg ? bu_make_const_bpm(reg, nreg, sr, &first) : 0;
    if (bpm > 0) {
        first = bu_adjust_phase(first, bpm, sr, beats, nbeats);
        if (first < 0) first += 60.0 * sr / bpm;
        *bpm_out = bpm;
        *beat0_out = first / sr;
    }
    free(reg);
    free(beats);
}

/* ------------------------------------------------------------------------------------------------ the analysis */
static inline uint8_t to_u8(float peak) {   /* WaveformStride::store */
    double v = 255.0 * peak + 0.5;
    return (uint8_t)(v > 255.0 ? 255.0 : v);
}

int analyze_audio(const int16_t *lr, int64_t frames, rx_analysis_t *out,
                  void (*progress)(void *, int), void *user) {
    if (!lr || frames <= 0 || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    out->gain = 1.0f;

    const double stride = (double)RX_SR / RX_WAVE_PX_PER_SEC;   /* 110.25 frames per column */
    int ncols = (int)ceil((double)frames / stride);
    if (ncols > RX_WAVE_MAX_COLS) ncols = RX_WAVE_MAX_COLS;
    if (ncols < 1) ncols = 1;
    out->cols = calloc((size_t)ncols, sizeof(rx_wave_col_t));
    if (!out->cols) return -ENOMEM;
    out->ncols = ncols;

    /* the three band filters, one instance per channel (Mixxx runs the interleaved stereo through one
     * EngineFilterIIR whose generated code keeps separate state per channel) */
    iir_t low[2], mid[2], high[2];
    for (int c = 0; c < 2; c++) {
        iir_bessel4_low(&low[c], RX_SR, AN_LOW_HZ);
        iir_bessel4_band(&mid[c], RX_SR, AN_LOW_HZ, AN_HIGH_HZ);
        iir_bessel4_high(&high[c], RX_SR, AN_HIGH_HZ);
    }

    float *mono = malloc(AN_CHUNK * sizeof(float));
    double *beats = malloc(AN_MAX_BEATS * sizeof(double));
    if (!mono || !beats) { free(mono); free(beats); analyze_free(out); return -ENOMEM; }
    void *qm = rx_qm_begin(RX_SR);           /* NULL = no beat tracker (bpm stays 0) */

    /* replay gain: gated block RMS */
    const int64_t gate_block = RX_SR * 2 / 5;     /* 400 ms */
    double block_sq = 0; int64_t block_n = 0;
    double gated_sum = 0; int64_t gated_blocks = 0;
    const double gate_thresh = pow(10.0, -70.0 / 10.0);   /* -70 dBFS mean square */

    float pk_all = 0, pk_low = 0, pk_mid = 0, pk_high = 0;
    int col = 0;
    int64_t col_end = (int64_t)ceil(stride);  /* first frame of the next column */
    int last_pct = -1;
    const float s16 = 1.0f / 32768.0f;

    for (int64_t pos = 0; pos < frames; pos += AN_CHUNK) {
        int n = (int)((frames - pos) < AN_CHUNK ? (frames - pos) : AN_CHUNK);
        const int16_t *in = lr + pos * 2;
        for (int i = 0; i < n; i++) {
            float l = in[i * 2] * s16, r = in[i * 2 + 1] * s16;
            float al = fabsf(l), ar = fabsf(r);
            float fl = (float)fabs(iir_run(&low[0], l)), fr = (float)fabs(iir_run(&low[1], r));
            float ml = (float)fabs(iir_run(&mid[0], l)), mr = (float)fabs(iir_run(&mid[1], r));
            float hl = (float)fabs(iir_run(&high[0], l)), hr = (float)fabs(iir_run(&high[1], r));
            if (al > pk_all) pk_all = al; if (ar > pk_all) pk_all = ar;
            if (fl > pk_low) pk_low = fl; if (fr > pk_low) pk_low = fr;
            if (ml > pk_mid) pk_mid = ml; if (mr > pk_mid) pk_mid = mr;
            if (hl > pk_high) pk_high = hl; if (hr > pk_high) pk_high = hr;
            mono[i] = (l + r) * 0.5f;                        /* DownmixAndOverlapHelper's downmix */

            block_sq += (double)l * l + (double)r * r;
            if (++block_n == gate_block) {
                double ms = block_sq / (2.0 * block_n);
                if (ms > gate_thresh) { gated_sum += ms; gated_blocks++; }
                block_sq = 0; block_n = 0;
            }

            int64_t frame = pos + i + 1;
            if (frame >= col_end) {                          /* close the column */
                if (col < ncols) {
                    out->cols[col].all = to_u8(pk_all);
                    out->cols[col].low = to_u8(pk_low);
                    out->cols[col].mid = to_u8(pk_mid);
                    out->cols[col].high = to_u8(pk_high);
                }
                col++;
                col_end = (int64_t)ceil((col + 1) * stride);
                pk_all = pk_low = pk_mid = pk_high = 0;
            }
        }
        if (qm && rx_qm_feed(qm, mono, n) != 0) { rx_qm_free(qm); qm = NULL; }
        int pct = (int)((pos + n) * 70 / frames);
        if (progress && pct != last_pct) { last_pct = pct; progress(user, pct); }
    }
    if (col < ncols) {                                       /* the partial last column */
        out->cols[col].all = to_u8(pk_all);
        out->cols[col].low = to_u8(pk_low);
        out->cols[col].mid = to_u8(pk_mid);
        out->cols[col].high = to_u8(pk_high);
    }
    if (block_n > RX_SR / 10) {                              /* a last partial block of >= 100 ms counts */
        double ms = block_sq / (2.0 * block_n);
        if (ms > gate_thresh) { gated_sum += ms; gated_blocks++; }
    }

    /* overview: max of the fine columns per bin */
    for (int b = 0; b < RX_OVERVIEW_W; b++) {
        int c0 = (int)((int64_t)b * ncols / RX_OVERVIEW_W);
        int c1 = (int)((int64_t)(b + 1) * ncols / RX_OVERVIEW_W);
        if (c1 <= c0) c1 = c0 + 1;
        if (c1 > ncols) c1 = ncols;
        rx_wave_col_t m = { 0, 0, 0, 0 };
        for (int c = c0; c < c1; c++) {
            const rx_wave_col_t *x = &out->cols[c];
            if (x->low > m.low) m.low = x->low;
            if (x->mid > m.mid) m.mid = x->mid;
            if (x->high > m.high) m.high = x->high;
            if (x->all > m.all) m.all = x->all;
        }
        out->overview[b] = m;
    }

    /* replay gain */
    if (gated_blocks > 0) {
        double rms_db = 10.0 * log10(gated_sum / gated_blocks);
        double gain_db = -14.0 - rms_db;
        if (gain_db > 20) gain_db = 20;
        if (gain_db < -20) gain_db = -20;
        out->gain = (float)pow(10.0, gain_db / 20.0);
    }

    /* BPM + first beat */
    if (progress) progress(user, 70);
    if (qm) {
        int nb = rx_qm_end(qm, beats, AN_MAX_BEATS);
        rx_qm_free(qm);
        if (nb > 1) beats_to_grid(beats, nb, &out->bpm, &out->beat0);
        else rx_log("analyze: beat tracker returned %d beats", nb);
    } else {
        rx_log("analyze: beat tracker unavailable, bpm unknown");
    }
    if (progress) progress(user, 95);

    free(mono);
    free(beats);
    if (progress) progress(user, 100);
    return 0;
}

void analyze_free(rx_analysis_t *a) {
    if (!a) return;
    free(a->cols);
    a->cols = NULL;
    a->ncols = 0;
}
