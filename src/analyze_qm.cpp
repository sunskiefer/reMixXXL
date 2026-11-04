/* reMixXXL -- analyze_qm.cpp: the Queen Mary beat tracker, as Mixxx runs it.
 *
 * This is a port of Mixxx's src/analyzer/plugins/analyzerqueenmarybeats.cpp (+ buffering_utils.cpp, the
 * DownmixAndOverlapHelper) onto qm-dsp's DetectionFunction and TempoTrackV2, behind a plain C interface so that
 * analyze.c (C) never sees C++.
 *
 *   step   = sr * 0.01161 s      (512 frames at 44.1 kHz, "kStepSecs": ~86 Hz detection-function rate)
 *   window = nextPowerOfTwo(sr / 50 Hz) = 1024 at 44.1 kHz ("kMaximumBinSizeHz")
 *   DF     = DF_COMPLEXSD (complex spectral difference), dbRise 3, no adaptive whitening
 *            (makeDetectionFunctionConfig: "the defaults for the VAMP beat tracker plugin we used in Mixxx 2.0")
 *   the first window is centred on the first sample (window/2 leading zeros), the end is padded with
 *   max(window - fill, window/2 - 1) zeros (DownmixAndOverlapHelper::initialize / finalize)
 *   the first 2 DF values are dropped and trailing zero DF values trimmed (AnalyzerQueenMaryBeats::finalize)
 *   TempoTrackV2(sr, step).calculateBeatPeriod(df) then calculateBeats(df, periods): beat positions in DF steps
 *   beat frame = beat * step + step / 2  ("the beat is detected between the two samples")
 *
 * Mixxx does not downsample for the beat tracker: it runs at the track's rate (44100 here).
 *
 * Two ways in:
 *   rx_qm_beats(mono, n, sr, out, max)                       one call, whole mono signal (the agreed interface)
 *   rx_qm_begin(sr) / rx_qm_feed(h, mono, n) / rx_qm_end(h, out, max) / rx_qm_free(h)
 *                                                            chunked, so analyze.c can stream the int16 track
 *                                                            through a small float buffer instead of holding a
 *                                                            15-minute mono copy (159 MB) next to the track
 * qm-dsp is compiled with exceptions on (FFTReal throws std::invalid_argument on odd sizes); every entry point here
 * is wrapped in try/catch and returns -1 on anything thrown, so nothing ever unwinds into C. */

#include <dsp/onsets/DetectionFunction.h>
#include <dsp/tempotracking/TempoTrackV2.h>
#include <maths/MathUtilities.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr float kStepSecs = 0.01161f;        /* analyzerqueenmarybeats.cpp */
constexpr int kMaximumBinSizeHz = 50;

struct QmState {
    int sr = 0;
    int step = 0;
    int window = 0;
    DetectionFunction *df = nullptr;
    std::vector<double> buf;                 /* the overlap window (DownmixAndOverlapHelper::m_buffer) */
    size_t write = 0;                        /* m_bufferWritePosition */
    std::vector<double> results;             /* one DF value per step (m_detectionResults) */
    bool failed = false;

    ~QmState() { delete df; }

    /* DownmixAndOverlapHelper::processInner for mono input (the downmix is done by the caller) */
    void feed(const float *mono, size_t n) {
        size_t read = 0;
        while (read < n) {
            size_t avail = n - read;
            size_t space = window - write;
            size_t take = std::min(avail, space);
            if (mono) for (size_t i = 0; i < take; i++) buf[write + i] = mono[read + i];
            else      for (size_t i = 0; i < take; i++) buf[write + i] = 0.0;
            write += take;
            read += take;
            if (write == (size_t)window) {
                results.push_back(df->processTimeDomain(buf.data()));
                /* slide by one step */
                for (size_t i = 0; i < (size_t)(window - step); i++) buf[i] = buf[i + step];
                write -= step;
            }
        }
    }
};

} // namespace

extern "C" {

void *rx_qm_begin(int sr) {
    try {
        if (sr < 8000) return nullptr;
        QmState *s = new QmState;
        s->sr = sr;
        s->step = (int)(sr * kStepSecs);
        s->window = MathUtilities::nextPowerOfTwo(sr / kMaximumBinSizeHz);
        DFConfig cfg;
        cfg.DFType = DF_COMPLEXSD;
        cfg.stepSize = s->step;
        cfg.frameLength = s->window;
        cfg.dbRise = 3;
        cfg.adaptiveWhitening = false;
        cfg.whiteningRelaxCoeff = -1;
        cfg.whiteningFloor = -1;
        s->df = new DetectionFunction(cfg);
        s->buf.assign(s->window, 0.0);
        s->write = s->window / 2;            /* centre the first frame in the first window */
        return s;
    } catch (...) {
        return nullptr;
    }
}

int rx_qm_feed(void *h, const float *mono, int n) {
    QmState *s = static_cast<QmState *>(h);
    if (!s || s->failed) return -1;
    if (n <= 0) return 0;
    try {
        s->feed(mono, (size_t)n);
        return 0;
    } catch (...) {
        s->failed = true;
        return -1;
    }
}

/* finishes the DF, runs the tempo tracker; returns the number of beats written (<= max_beats), or -1 */
int rx_qm_end(void *h, double *out_beats_sec, int max_beats) {
    QmState *s = static_cast<QmState *>(h);
    if (!s || s->failed) return -1;
    try {
        /* DownmixAndOverlapHelper::finalize: pad so the last samples get a full window */
        size_t to_fill = s->window - s->write;
        size_t pad = std::max(to_fill, (size_t)(s->window / 2 - 1));
        s->feed(nullptr, pad);

        /* AnalyzerQueenMaryBeats::finalize */
        size_t nonzero = s->results.size();
        while (nonzero > 0 && s->results[nonzero - 1] <= 0.0) --nonzero;
        size_t required = std::max((size_t)2, nonzero) - 2;
        std::vector<double> df;
        df.reserve(required);
        std::vector<int> beatPeriod(required / 128 + 1);
        for (size_t i = 2; i < nonzero; ++i) df.push_back(s->results[i]);   /* skip the first 2: noise onsets */
        if (df.empty()) return 0;

        TempoTrackV2 tt((float)s->sr, s->step);
        tt.calculateBeatPeriod(df, beatPeriod);
        std::vector<double> beats;
        tt.calculateBeats(df, beatPeriod, beats);

        int n = 0;
        for (size_t i = 0; i < beats.size() && n < max_beats; ++i) {
            double frame = beats[i] * s->step + s->step / 2;
            out_beats_sec[n++] = frame / s->sr;
        }
        return n;
    } catch (...) {
        return -1;
    }
}

void rx_qm_free(void *h) {
    delete static_cast<QmState *>(h);
}

/* the one-shot form: the whole mono signal in one go */
int rx_qm_beats(const float *mono, int n, int sr, double *out_beats_sec, int max_beats) {
    if (!mono || n <= 0 || !out_beats_sec || max_beats <= 0) return -1;
    void *h = rx_qm_begin(sr);
    if (!h) return -1;
    int rc = rx_qm_feed(h, mono, n);
    if (rc == 0) rc = rx_qm_end(h, out_beats_sec, max_beats);
    rx_qm_free(h);
    return rc;
}

} // extern "C"
