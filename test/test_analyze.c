/* reMixXXL -- test/test_analyze.c: native (x86) check of decode.c and analyze.c logic.  Run: test/run_a1.sh
 *
 *  1. writes a 44.1 kHz stereo WAV: 120 BPM clicks (a 5 ms burst every 0.5 s on a quiet 50 Hz + 2 kHz bed),
 *     decodes it, checks the frame count and that the clicks survive.
 *  2. writes a 48 kHz mono WAV with a 1 kHz tone, decodes it: expects 44.1 kHz stereo, the right length, both
 *     channels equal, the tone level within 1 dB (the resampler's passband).
 *  3. analyses the click track: BPM 120 +- 1, beat0 near 0 (the clicks start at 0.1 s so beat0 ~ 0.1), waveform
 *     columns: a click column has all > 0, a quiet column lower; the low band dominates a 50 Hz-only stretch and
 *     the high band a 6 kHz-only stretch; the overview has RX_OVERVIEW_W columns with sane values; gain > 1 for a
 *     quiet track. */
#include "remix.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stubs for the parts of the plugin the two files log through */
void rx_log(const char *fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(cond, ...) do { if (cond) printf("  ok   " __VA_ARGS__); else { fails++; printf("  FAIL " __VA_ARGS__); } printf("\n"); } while (0)

static void wr32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void wr16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

static void write_wav(const char *path, const int16_t *pcm, int64_t frames, int ch, int sr) {
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)(frames * ch * 2);
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, (uint16_t)ch); wr32(f, (uint32_t)sr);
    wr32(f, (uint32_t)(sr * ch * 2)); wr16(f, (uint16_t)(ch * 2)); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, data);
    fwrite(pcm, 2, (size_t)(frames * ch), f);
    fclose(f);
}

static void progress(void *u, int pct) { int *last = u; if (pct < *last) { fails++; printf("  FAIL progress went backwards\n"); } *last = pct; }

int main(void) {
    const int sr = RX_SR;
    const double secs = 40.0;                 /* enough beats for the const-region logic (>= 16 beats) */
    const int64_t frames = (int64_t)(secs * sr);
    const double bpm = 120.0, beat_len = 60.0 / bpm, first = 0.1;

    /* ---- 1. the click track */
    int16_t *pcm = calloc((size_t)frames * 2, 2);
    for (int64_t i = 0; i < frames; i++) {
        double t = (double)i / sr;
        double bed;
        if (t < 10) bed = 0.05 * sin(2 * M_PI * 50 * t);                 /* 0..10 s: low only */
        else if (t < 20) bed = 0.05 * sin(2 * M_PI * 6000 * t);          /* 10..20 s: high only */
        else bed = 0.03 * sin(2 * M_PI * 50 * t) + 0.03 * sin(2 * M_PI * 2000 * t);
        double since = fmod(t - first + 1000 * beat_len, beat_len);
        double click = 0;
        if (t >= first && since < 0.005) click = 0.9 * (1.0 - since / 0.005) * sin(2 * M_PI * 1500 * since);
        double v = bed + click;
        if (v > 0.999) v = 0.999; if (v < -0.999) v = -0.999;
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)lrint(v * 32767);
    }
    write_wav("/tmp/rx_click.wav", pcm, frames, 2, sr);

    printf("decode 44.1k stereo wav\n");
    rx_audio_t a; int last = -1;
    int rc = decode_file("/tmp/rx_click.wav", &a, RX_MAX_TRACK_SEC * RX_SR, progress, &last);
    CHECK(rc == frames, "frames %d == %lld", rc, (long long)frames);
    CHECK(a.frames == frames && a.lr, "buffer frames %lld", (long long)a.frames);
    CHECK(strcmp(a.title, "rx_click") == 0 && a.artist[0] == 0, "title '%s' from the stem, artist ''", a.title);
    CHECK(last == 100, "progress reached 100");
    if (a.lr) {
        int64_t k = (int64_t)((first + 0.0002) * sr);
        CHECK(abs(a.lr[k * 2] - pcm[k * 2]) <= 1, "sample at first click identical (%d vs %d)", a.lr[k * 2], pcm[k * 2]);
    }
    CHECK(decode_is_audio_file("x.MP3") && decode_is_audio_file("y.flac") && !decode_is_audio_file("z.ogg"), "extension check");

    /* ---- 2. resampling: 48k mono 1 kHz tone, 3 s */
    const int sr2 = 48000; const int64_t fr2 = sr2 * 3;
    int16_t *mono = malloc((size_t)fr2 * 2);
    for (int64_t i = 0; i < fr2; i++) mono[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 1000 * i / sr2));
    write_wav("/tmp/rx_tone48.wav", mono, fr2, 1, sr2);
    printf("decode 48k mono wav (resample to 44.1k stereo)\n");
    rx_audio_t b;
    rc = decode_file("/tmp/rx_tone48.wav", &b, RX_MAX_TRACK_SEC * RX_SR, NULL, NULL);
    int64_t expect = (int64_t)((double)fr2 * RX_SR / sr2);
    CHECK(rc > 0 && llabs(b.frames - expect) <= 20, "frames %lld ~ %lld", (long long)b.frames, (long long)expect);
    if (b.lr) {
        double sq = 0; int64_t n0 = RX_SR / 2, n1 = b.frames - RX_SR / 2; int eq = 1;
        for (int64_t i = n0; i < n1; i++) { sq += (double)b.lr[i * 2] * b.lr[i * 2]; if (b.lr[i * 2] != b.lr[i * 2 + 1]) eq = 0; }
        double rms = sqrt(sq / (n1 - n0)) / 32767, want = 0.5 / sqrt(2);
        double db = 20 * log10(rms / want);
        CHECK(fabs(db) < 1.0, "tone level after resampling %.2f dB (want within 1 dB)", db);
        CHECK(eq, "mono duplicated to both channels");
        /* 1 kHz should still be 1 kHz: count zero crossings over 2 s */
        int zc = 0; for (int64_t i = n0 + 1; i < n1; i++) if ((b.lr[i * 2] >= 0) != (b.lr[(i - 1) * 2] >= 0)) zc++;
        double hz = zc / 2.0 / ((double)(n1 - n0) / RX_SR);
        CHECK(fabs(hz - 1000) < 5, "tone frequency %.1f Hz", hz);
    }
    decode_free(&b);

    /* ---- 3. analysis of the click track */
    printf("analyze click track (%g s, %g BPM)\n", secs, bpm);
    rx_analysis_t an; last = -1;
    rc = analyze_audio(a.lr, a.frames, &an, progress, &last);
    CHECK(rc == 0, "analyze_audio rc %d", rc);
    CHECK(last == 100, "progress reached 100");
    CHECK(an.ncols == (int)ceil(secs * RX_WAVE_PX_PER_SEC), "ncols %d", an.ncols);
    CHECK(fabs(an.bpm - bpm) <= 1.0, "bpm %.3f (want %g +- 1)", an.bpm, bpm);
    {   /* beat0 must sit on the click grid (mod beat length) within 15 ms */
        double off = fmod(an.beat0 - first + 100 * beat_len, beat_len);
        if (off > beat_len / 2) off -= beat_len;
        CHECK(fabs(off) < 0.015, "beat0 %.4f s, %.1f ms off the click grid", an.beat0, off * 1000);
    }
    if (an.cols) {
        int cc = (int)((first + 0.001) * RX_WAVE_PX_PER_SEC);      /* a click column */
        int cq = (int)((first + 0.25) * RX_WAVE_PX_PER_SEC);       /* between clicks, 50 Hz bed only */
        CHECK(an.cols[cc].all > 150 && an.cols[cq].all < 40, "click column all=%d, quiet column all=%d", an.cols[cc].all, an.cols[cq].all);
        CHECK(an.cols[cq].low > an.cols[cq].mid && an.cols[cq].low > an.cols[cq].high,
              "50 Hz bed: low %d > mid %d, high %d", an.cols[cq].low, an.cols[cq].mid, an.cols[cq].high);
        int ch = (int)((15.0 + 0.25) * RX_WAVE_PX_PER_SEC);        /* 6 kHz bed only */
        CHECK(an.cols[ch].high > an.cols[ch].mid && an.cols[ch].high > an.cols[ch].low,
              "6 kHz bed: high %d > mid %d, low %d", an.cols[ch].high, an.cols[ch].mid, an.cols[ch].low);
        int cm = (int)((25.0 + 0.25) * RX_WAVE_PX_PER_SEC);        /* 50 Hz + 2 kHz */
        CHECK(an.cols[cm].mid > an.cols[cm].high, "2 kHz: mid %d > high %d (low %d)", an.cols[cm].mid, an.cols[cm].high, an.cols[cm].low);
        /* bed amplitude 0.05 -> 255*0.05 = 13 in the band that holds it (unity passband); one 2.5 ms column
         * cannot hold a whole 50 Hz peak (20 ms period), so take the max over a 20 ms span of columns */
        int lowmax = 0;
        for (int i = cq; i < cq + 8; i++) if (an.cols[i].low > lowmax) lowmax = an.cols[i].low;
        CHECK(lowmax >= 11 && lowmax <= 15, "low band passband gain ~ unity (%d ~ 13)", lowmax);
        int ovmax = 0, ovnz = 0;
        for (int i = 0; i < RX_OVERVIEW_W; i++) { if (an.overview[i].all > ovmax) ovmax = an.overview[i].all; if (an.overview[i].all) ovnz++; }
        CHECK(ovmax > 150 && ovnz == RX_OVERVIEW_W, "overview max %d, %d/%d non-empty bins", ovmax, ovnz, RX_OVERVIEW_W);
    }
    CHECK(an.gain > 1.0f && an.gain < 12.0f, "replay gain %.2f (quiet track: > 1)", an.gain);
    analyze_free(&an);
    decode_free(&a);

    /* ---- 4. a short track (< 16 beats): the average-BPM fallback */
    {
        const int64_t fr3 = sr * 5;
        int16_t *p3 = calloc((size_t)fr3 * 2, 2);
        for (int64_t i = 0; i < fr3; i++) {
            double t = (double)i / sr, since = fmod(t, 60.0 / 128.0);
            double v = since < 0.005 ? 0.9 * sin(2 * M_PI * 1500 * since) : 0;
            p3[i * 2] = p3[i * 2 + 1] = (int16_t)lrint(v * 32767);
        }
        rx_analysis_t a3;
        rc = analyze_audio(p3, fr3, &a3, NULL, NULL);
        printf("short 128 BPM track (5 s)\n");
        CHECK(rc == 0 && a3.bpm > 0 && fabs(a3.bpm - 128) <= 2.0, "bpm %.2f (want 128 +- 2)", a3.bpm);
        analyze_free(&a3);
        free(p3);
    }

    free(pcm); free(mono);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
