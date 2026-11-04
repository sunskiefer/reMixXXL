/* reMixXXL -- native logic test for deck.c / mixer.c / fx.c (run by test/run_a2.sh).
 * A synthetic track (sine + clicks at 120 BPM) through the deck at rate 1, 1.1, reverse, keylock, a loop, a scratch,
 * then the mixer with EQ kills and an effect unit. Checks sanity: no NaN, no clipping explosion, loop stays within
 * bounds, rate changes pitch (zero crossings), keylock keeps it. */
#include "remix.h"
#include "deck.h"
#include "fx_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* ---- stubs for the parts not under test */
void rx_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void rx_log_open(const char *d) { (void)d; }
const char *rx_plugin_dir(void) { return "."; }
int decode_file(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user) { (void)path; (void)out; (void)max_frames; (void)progress; (void)user; return -1; }
void decode_free(rx_audio_t *a) { free(a->lr); a->lr = NULL; }
int analyze_audio(const int16_t *lr, int64_t frames, rx_analysis_t *out, void (*progress)(void *, int), void *user) { (void)lr; (void)frames; (void)out; (void)progress; (void)user; return -1; }
void analyze_free(rx_analysis_t *a) { free(a->cols); a->cols = NULL; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SEC 20
#define FREQ 440.0
static rx_audio_t *make_track(double freq) {
    rx_audio_t *a = calloc(1, sizeof *a);
    a->frames = SEC * RX_SR;
    a->lr = malloc((size_t)a->frames * 4);
    for (int64_t i = 0; i < a->frames; i++) {
        double t = (double)i / RX_SR;
        double v = 0.5 * sin(2 * M_PI * freq * t);
        int beat = ((int64_t)(t * 2) * RX_SR / 2);   /* clicks at 120 bpm */
        if (i - beat < 20) v += 0.4;
        int16_t s = (int16_t)(v * 32767);
        a->lr[2 * i] = s; a->lr[2 * i + 1] = s;
    }
    snprintf(a->title, sizeof a->title, "test tone");
    return a;
}
static rx_analysis_t *make_analysis(void) {
    rx_analysis_t *an = calloc(1, sizeof *an);
    an->bpm = 120; an->beat0 = 0; an->gain = 1; an->ncols = SEC * RX_WAVE_PX_PER_SEC;
    an->cols = calloc((size_t)an->ncols, sizeof *an->cols);
    return an;
}
static int finite_block(const float *b, int n) { for (int i = 0; i < n; i++) if (!isfinite(b[i])) return 0; return 1; }
static double peak_block(const float *b, int n) { double p = 0; for (int i = 0; i < n; i++) if (fabs(b[i]) > p) p = fabs(b[i]); return p; }
static double rms_block(const float *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += b[i] * b[i]; return sqrt(s / n); }

/* render `blocks` blocks; returns zero-crossing rate (Hz) of the left channel */
static double run(rx_deck_t *d, int blocks, float *acc, int *acc_n, double *peak) {
    static float buf[RX_BLOCK * 2];
    int zc = 0, total = 0; float prev = 0; double pk = 0;
    for (int b = 0; b < blocks; b++) {
        deck_tick_controls(d);
        deck_render(d, buf, RX_BLOCK);
        if (!finite_block(buf, RX_BLOCK * 2)) { fails++; printf("FAIL: NaN in block %d\n", b); break; }
        for (int i = 0; i < RX_BLOCK; i++) { float v = buf[2 * i]; if ((v >= 0) != (prev >= 0)) zc++; prev = v; total++; }
        double p = peak_block(buf, RX_BLOCK * 2); if (p > pk) pk = p;
        if (acc) { memcpy(acc + *acc_n * 2, buf, sizeof buf); *acc_n += RX_BLOCK; }
    }
    if (peak) *peak = pk;
    return total ? zc / 2.0 / ((double)total / RX_SR) : 0;
}

int main(void) {
    ctl_init();
    rx_deck_t *decks[RX_DECKS];
    for (int i = 0; i < RX_DECKS; i++) { char g[16]; snprintf(g, sizeof g, "[Channel%d]", i + 1); ctl_register(g, "track_step", CK_INT, -1, 1, 0); decks[i] = deck_create(i); deck_register_controls(decks[i]); }
    rx_mixer_t *mx = mixer_create(); mixer_register_controls(mx);
    rx_fx_t *fx = fx_create(); fx_register_controls(fx);
    rx_deck_t *d = decks[0];
    printf("controls registered: %d\n", ctl_count());
    CHECK(ctl_lookup("[Channel1]", "hotcue_1_activate") != CTL_NONE, "hotcue control exists");
    CHECK(ctl_lookup("[EqualizerRack1_[Channel1]_Effect1]", "parameter1") != CTL_NONE, "EQ control exists");
    CHECK(ctl_lookup("[QuickEffectRack1_[Channel1]]", "super1") != CTL_NONE, "quick effect control exists");
    CHECK(ctl_lookup("[Channel1]", "cues_dirty") != CTL_NONE && ctl_lookup("[Channel1]", "touch_zone") != CTL_NONE, "cues_dirty / touch_zone exist");

    /* ---- load */
    CHECK(deck_set_track(d, "/tmp/test.wav", make_track(FREQ), make_analysis()) == 0, "deck_set_track");
    deck_tick_controls(d);
    CHECK(ctl_get(ctl_lookup("[Channel1]", "track_loaded")) == 1, "track_loaded");
    CHECK(rx_wave_total_cols(0) == SEC * RX_WAVE_PX_PER_SEC, "wave columns visible to the screen");
    ctl_id play = ctl_lookup("[Channel1]", "play"), rate = ctl_lookup("[Channel1]", "rate"), rev = ctl_lookup("[Channel1]", "reverse");
    ctl_id keylock = ctl_lookup("[Channel1]", "keylock"), playpos = ctl_lookup("[Channel1]", "playposition");

    /* ---- rate 1 */
    double pk;
    ctl_set(play, 1);
    double f1 = run(d, 400, NULL, NULL, &pk);
    CHECK(fabs(f1 - FREQ) < 8, "rate 1: pitch %.1f Hz (expect %.0f), peak %.2f", f1, FREQ, pk);
    CHECK(pk <= 1.0, "rate 1: no clipping (peak %.2f)", pk);
    double pos1 = ctl_get(playpos) * SEC;
    CHECK(fabs(pos1 - 400.0 * RX_BLOCK / RX_SR) < 0.02, "rate 1: position %.3f s after 400 blocks", pos1);

    /* ---- rate +10 % (slider 1.0 at range 0.10) */
    ctl_set(rate, 1.0);
    run(d, 20, NULL, NULL, NULL);
    double f2 = run(d, 400, NULL, NULL, &pk);
    CHECK(fabs(f2 - FREQ * 1.1) < 8, "rate 1.1: pitch %.1f Hz (expect %.0f)", f2, FREQ * 1.1);
    CHECK(fabs(deck_current_rate(d) - 1.1) < 1e-6, "rate readback %.4f", deck_current_rate(d));

    /* ---- keylock at 1.1: pitch stays, tempo changes */
    ctl_set(keylock, 1);
    run(d, 20, NULL, NULL, NULL);
    double p0 = ctl_get(playpos) * SEC;
    double f3 = run(d, 400, NULL, NULL, &pk);
    double adv = ctl_get(playpos) * SEC - p0;
    CHECK(fabs(f3 - FREQ) < 12, "keylock 1.1: pitch %.1f Hz (expect ~%.0f)", f3, FREQ);
    CHECK(fabs(adv - 1.1 * 400.0 * RX_BLOCK / RX_SR) < 0.03, "keylock 1.1: advanced %.3f s (expect %.3f)", adv, 1.1 * 400.0 * RX_BLOCK / RX_SR);
    CHECK(pk <= 1.0, "keylock: no clipping (peak %.2f)", pk);
    ctl_set(keylock, 0); ctl_set(rate, 0);

    /* ---- reverse */
    p0 = ctl_get(playpos) * SEC;
    ctl_set(rev, 1);
    run(d, 200, NULL, NULL, &pk);
    double p1 = ctl_get(playpos) * SEC;
    CHECK(p1 < p0 - 0.4, "reverse: position went back %.3f -> %.3f", p0, p1);
    ctl_set(rev, 0);

    /* ---- loop: 4 beats = 2 s at 120 bpm */
    ctl_set(ctl_lookup("[Channel1]", "beatloop_4_activate"), 1);
    run(d, 1, NULL, NULL, NULL);
    rx_deck_view_t v; deck_view(d, &v);
    CHECK(v.loop_on && fabs((v.loop_out - v.loop_in) - 2.0) < 1e-3, "beatloop 4: loop %.3f..%.3f on=%d", v.loop_in, v.loop_out, v.loop_on);
    int inside = 1;
    for (int b = 0; b < 2000; b++) { run(d, 1, NULL, NULL, NULL); deck_view(d, &v); if (v.pos < v.loop_in - 1e-6 || v.pos >= v.loop_out + 1e-6) inside = 0; }
    CHECK(inside, "loop: position stayed inside for 5.8 s (pos %.3f)", v.pos);
    ctl_set(ctl_lookup("[Channel1]", "loop_halve"), 1); run(d, 1, NULL, NULL, NULL); deck_view(d, &v);
    CHECK(fabs((v.loop_out - v.loop_in) - 1.0) < 1e-3, "loop_halve: %.3f s", v.loop_out - v.loop_in);
    ctl_set(ctl_lookup("[Channel1]", "reloop_toggle"), 1); run(d, 1, NULL, NULL, NULL); deck_view(d, &v);
    CHECK(!v.loop_on, "reloop_toggle: loop off");

    /* ---- hot cue set / jump / clear-mode */
    double before = v.pos;
    ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 1); run(d, 1, NULL, NULL, NULL); ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 0); run(d, 1, NULL, NULL, NULL);
    deck_view(d, &v);
    CHECK(v.hotcue[0] >= 0 && fabs(v.hotcue[0] - round(before * 2) / 2) < 0.01, "hotcue 1 set (quantized) at %.3f (pos was %.3f)", v.hotcue[0], before);
    run(d, 300, NULL, NULL, NULL);
    ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 1); run(d, 1, NULL, NULL, NULL); ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 0); run(d, 1, NULL, NULL, NULL);
    deck_view(d, &v);
    CHECK(fabs(v.pos - v.hotcue[0]) < 0.02, "hotcue 1 jump while playing: pos %.3f", v.pos);
    CHECK(ctl_get(ctl_lookup("[Channel1]", "cues_dirty")) == 1, "cues_dirty raised");
    char cues[512]; deck_cues_export(d, cues, sizeof cues);
    CHECK(strstr(cues, "hot 1 ") != NULL, "cues export: %s", cues);
    ctl_set(ctl_lookup("[Channel1]", "hotcue_clear_mode"), 1);
    ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 1); run(d, 1, NULL, NULL, NULL); ctl_set(ctl_lookup("[Channel1]", "hotcue_1_activate"), 0); run(d, 1, NULL, NULL, NULL);
    deck_view(d, &v);
    CHECK(v.hotcue[0] < 0, "hotcue clear mode cleared it");
    deck_cues_import(d, "hot 3 4.5 2\ncue 1.0\n"); run(d, 2, NULL, NULL, NULL); deck_view(d, &v);
    CHECK(fabs(v.hotcue[2] - 4.5) < 1e-6 && v.hotcue_colour[2] == 2 && fabs(v.cue - 1.0) < 1e-6, "cues import: hot3 %.2f col %d cue %.2f", v.hotcue[2], v.hotcue_colour[2], v.cue);

    /* ---- cue_default (Mixxx mode): press while playing -> stop at cue */
    ctl_set(ctl_lookup("[Channel1]", "cue_default"), 1); run(d, 1, NULL, NULL, NULL); ctl_set(ctl_lookup("[Channel1]", "cue_default"), 0); run(d, 1, NULL, NULL, NULL);
    deck_view(d, &v);
    CHECK(!v.playing && fabs(v.pos - 1.0) < 0.01, "cue while playing: stopped at cue (pos %.3f playing %d)", v.pos, v.playing);
    /* press while stopped at cue -> preview while held, release -> back */
    ctl_set(ctl_lookup("[Channel1]", "cue_default"), 1); run(d, 100, NULL, NULL, NULL); deck_view(d, &v);
    CHECK(v.playing && v.pos > 1.1, "cue held at cue: previewing (pos %.3f)", v.pos);
    ctl_set(ctl_lookup("[Channel1]", "cue_default"), 0); run(d, 2, NULL, NULL, NULL); deck_view(d, &v);
    CHECK(!v.playing && fabs(v.pos - 1.0) < 0.01, "cue released: back at cue stopped (pos %.3f)", v.pos);

    /* ---- scratch (wheel): stopped deck, ticks forward -> moves forward */
    deck_scratch_enable(d, 1, 33.0 + 1.0 / 3.0, 1.0 / 8, 1.0 / 8 / 32);
    p0 = v.pos;
    for (int b = 0; b < 100; b++) { deck_scratch_tick(d, 4); run(d, 1, NULL, NULL, &pk); }
    deck_view(d, &v);
    double sc2 = ctl_get(ctl_lookup("[Channel1]", "scratch2"));
    CHECK(v.pos > p0 + 0.05 && sc2 > 0.1 && ctl_get(ctl_lookup("[Channel1]", "scratch2_enable")) == 1, "scratch: moved %.3f -> %.3f, scratch2 %.2f", p0, v.pos, sc2);
    CHECK(fabs(v.rate - sc2) < 1e-9, "scratch: platter follows scratch rate (%.3f)", v.rate);
    deck_scratch_enable(d, 0, 0, 0, 0);
    run(d, 450, NULL, NULL, NULL);
    CHECK(ctl_get(ctl_lookup("[Channel1]", "scratch2_enable")) == 0, "scratch disabled: ramp ended");

    /* ---- slip: play, enable slip, scratch backwards, disable -> position restored to where it would have been */
    ctl_set(play, 1); run(d, 10, NULL, NULL, NULL); deck_view(d, &v); p0 = v.pos;
    ctl_set(ctl_lookup("[Channel1]", "slip_enabled"), 1); run(d, 1, NULL, NULL, NULL);
    ctl_set(rev, 1); run(d, 100, NULL, NULL, NULL); ctl_set(rev, 0);
    ctl_set(ctl_lookup("[Channel1]", "slip_enabled"), 0); run(d, 1, NULL, NULL, NULL); deck_view(d, &v);
    double expect = p0 + 102.0 * RX_BLOCK / RX_SR;
    CHECK(fabs(v.pos - expect) < 0.02, "slip: restored to %.3f (expect %.3f)", v.pos, expect);

    /* ---- texts */
    char t[64]; ctl_text(ctl_lookup("[Channel1]", "bpm_text"), t, sizeof t);
    CHECK(!strcmp(t, "120.0"), "bpm_text '%s'", t);
    ctl_text(ctl_lookup("[Channel1]", "title_text"), t, sizeof t);
    CHECK(!strcmp(t, "test tone"), "title_text '%s'", t);

    /* ---- mixer: EQ kills. deck 2 gets a 60 Hz tone, deck 3 a 12 kHz tone */
    CHECK(deck_set_track(decks[1], "/tmp/low.wav", make_track(60), make_analysis()) == 0, "deck 2 load");
    CHECK(deck_set_track(decks[2], "/tmp/high.wav", make_track(12000), make_analysis()) == 0, "deck 3 load");
    ctl_set(play, 0);
    for (int i = 0; i < RX_DECKS; i++) deck_tick_controls(decks[i]);    /* adopt the loads (a load stops the deck) */
    ctl_set(ctl_lookup("[Channel2]", "play"), 1); ctl_set(ctl_lookup("[Channel3]", "play"), 1);
    static float db[RX_DECKS][RX_BLOCK * 2], out[RX_BLOCK * 2];
    float *dl[RX_DECKS];
    double flat2 = 0, flat3 = 0, kill2 = 0, kill3 = 0;
    /* one channel at a time (the mixer is stateful: one render per block) */
    for (int pass = 0; pass < 4; pass++) {
        int ch = pass & 1 ? 2 : 1;                     /* deck 2 (60 Hz) or deck 3 (12 kHz) */
        ctl_set(ctl_lookup("[Channel2]", "play"), ch == 1); ctl_set(ctl_lookup("[Channel3]", "play"), ch == 2);
        ctl_set(ctl_lookup("[EqualizerRack1_[Channel2]_Effect1]", "parameter1"), pass >= 2 ? 0 : 1);
        ctl_set(ctl_lookup("[EqualizerRack1_[Channel3]_Effect1]", "parameter3"), pass >= 2 ? 0 : 1);
        double acc = 0;
        for (int b = 0; b < 300; b++) {
            for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); dl[i] = db[i]; deck_render(decks[i], dl[i], RX_BLOCK); }
            mixer_render(mx, dl, out, RX_BLOCK, fx);
            if (!finite_block(out, RX_BLOCK * 2)) { fails++; printf("FAIL: NaN in mixer output\n"); break; }
            if (b > 150) acc += rms_block(out, RX_BLOCK * 2);
        }
        if (pass == 0) flat2 = acc; else if (pass == 1) flat3 = acc; else if (pass == 2) kill2 = acc; else kill3 = acc;
    }
    CHECK(flat2 > 0.05 * 149 && kill2 < flat2 * 0.08, "EQ low kill: 60 Hz rms %.4f -> %.4f", flat2 / 149, kill2 / 149);
    CHECK(flat3 > 0.05 * 149 && kill3 < flat3 * 0.08, "EQ high kill: 12 kHz rms %.4f -> %.4f", flat3 / 149, kill3 / 149);
    ctl_set(ctl_lookup("[EqualizerRack1_[Channel2]_Effect1]", "parameter1"), 1); ctl_set(ctl_lookup("[EqualizerRack1_[Channel3]_Effect1]", "parameter3"), 1);
    /* flat EQ passes unity (LV-mix is bit-perfect at unity apart from the delay): 1 kHz through deck 4 */
    CHECK(deck_set_track(decks[3], "/tmp/mid.wav", make_track(1000), make_analysis()) == 0, "deck 4 load");
    deck_tick_controls(decks[3]);
    ctl_set(ctl_lookup("[Channel4]", "play"), 1); ctl_set(ctl_lookup("[Channel2]", "play"), 0); ctl_set(ctl_lookup("[Channel3]", "play"), 0);
    ctl_set(ctl_lookup("[Channel2]", "volume"), 1); ctl_set(ctl_lookup("[Channel3]", "volume"), 1);
    double in_rms = 0, out_rms = 0, out_pk = 0;
    for (int b = 0; b < 400; b++) {
        for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); dl[i] = db[i]; deck_render(decks[i], dl[i], RX_BLOCK); }
        if (b > 200) in_rms += rms_block(db[3], RX_BLOCK * 2);
        mixer_render(mx, dl, out, RX_BLOCK, fx);
        if (b > 200) { out_rms += rms_block(out, RX_BLOCK * 2); double p = peak_block(out, RX_BLOCK * 2); if (p > out_pk) out_pk = p; }
    }
    CHECK(fabs(out_rms / in_rms - 1.0) < 0.05, "flat EQ + fader unity: rms ratio %.3f", out_rms / in_rms);
    CHECK(out_pk <= 1.0, "master peak %.3f <= 1", out_pk);
    /* crossfader hard left with deck 4 on the right -> silence */
    ctl_set(ctl_lookup("[Channel4]", "orientation"), 2); ctl_set(ctl_lookup("[Master]", "crossfader"), -1);
    double xr = 0;
    for (int b = 0; b < 50; b++) { for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); deck_render(decks[i], dl[i], RX_BLOCK); } mixer_render(mx, dl, out, RX_BLOCK, fx); if (b > 20) xr += rms_block(out, RX_BLOCK * 2); }
    CHECK(xr < 1e-4, "crossfader left kills a right-side channel (rms %.6f)", xr / 29);
    ctl_set(ctl_lookup("[Master]", "crossfader"), 0);
    /* quick effect: super1 = 0.1 (lowpass far down) attenuates the 1 kHz tone */
    ctl_set(ctl_lookup("[QuickEffectRack1_[Channel4]]", "super1"), 0.1);
    double qr = 0;
    for (int b = 0; b < 100; b++) { for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); deck_render(decks[i], dl[i], RX_BLOCK); } mixer_render(mx, dl, out, RX_BLOCK, fx); if (b > 50) qr += rms_block(out, RX_BLOCK * 2); }
    CHECK(qr / 49 < out_rms / 199 * 0.2, "quick effect lowpass at 0.1 attenuates 1 kHz (rms %.4f vs %.4f)", qr / 49, out_rms / 199);
    ctl_set(ctl_lookup("[QuickEffectRack1_[Channel4]]", "super1"), 0.5);

    /* ---- fx: every effect type through unit 1 on channel 4, then master; all finite, no explosion */
    fx_set_tempo(fx, 120);
    ctl_set(ctl_lookup("[EffectRack1_EffectUnit1]", "enabled"), 1);
    ctl_set(ctl_lookup("[EffectRack1_EffectUnit1]", "group_[Channel4]_enable"), 1);
    ctl_set(ctl_lookup("[EffectRack1_EffectUnit1]", "mix"), 1);
    for (int e = 1; e < fx_effect_count(); e++) {
        ctl_set(ctl_lookup("[EffectRack1_EffectUnit1_Effect1]", "effect_idx"), e);
        double fr = 0, fp = 0; int nan = 0;
        for (int b = 0; b < 300; b++) {
            for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); deck_render(decks[i], dl[i], RX_BLOCK); }
            if (b == 100) ctl_set(ctl_lookup("[EffectRack1_EffectUnit1_Effect1]", "meta"), 0.8);   /* sweep the meta knob */
            if (b == 200) ctl_set(ctl_lookup("[EffectRack1_EffectUnit1_Effect1]", "meta"), 0.2);
            mixer_render(mx, dl, out, RX_BLOCK, fx);
            if (!finite_block(out, RX_BLOCK * 2)) nan = 1;
            fr += rms_block(out, RX_BLOCK * 2); double p = peak_block(out, RX_BLOCK * 2); if (p > fp) fp = p;
        }
        CHECK(!nan && fp <= 1.0 && fr > 0, "fx %-9s: finite, peak %.3f, mean rms %.4f (p1 '%s')", fx_effect_name(e), fp, fr / 300, fx_param_name(e, 0));
        ctl_set(ctl_lookup("[EffectRack1_EffectUnit1_Effect1]", "meta"), 0.5);
    }
    /* echo tail: audible after the input stops */
    ctl_set(ctl_lookup("[EffectRack1_EffectUnit1_Effect1]", "effect_idx"), 1);
    for (int b = 0; b < 200; b++) { for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); deck_render(decks[i], dl[i], RX_BLOCK); } mixer_render(mx, dl, out, RX_BLOCK, fx); }
    ctl_set(ctl_lookup("[Channel4]", "play"), 0);
    double tail = 0;
    for (int b = 0; b < 100; b++) { for (int i = 0; i < RX_DECKS; i++) { deck_tick_controls(decks[i]); deck_render(decks[i], dl[i], RX_BLOCK); } mixer_render(mx, dl, out, RX_BLOCK, fx); if (b > 60) tail += rms_block(out, RX_BLOCK * 2); }
    CHECK(tail > 1e-3, "echo tail after stop (rms %.4f)", tail / 39);
    ctl_set(ctl_lookup("[EffectRack1_EffectUnit1]", "enabled"), 0);

    /* ---- sync: deck 1 at 120 synced to a "150 bpm" deck 2 (grid override) */
    deck_cues_import(decks[1], "grid 150 0\n"); ctl_set(ctl_lookup("[Channel2]", "play"), 1);
    ctl_set(ctl_lookup("[Channel1]", "rateRange_idx"), 3); run(d, 2, NULL, NULL, NULL);   /* WIDE so 150/120 fits */
    ctl_set(play, 1); run(d, 2, NULL, NULL, NULL);
    ctl_set(ctl_lookup("[Channel1]", "beatsync"), 1);
    for (int b = 0; b < 5; b++) { deck_tick_controls(decks[1]); deck_render(decks[1], db[1], RX_BLOCK); run(d, 1, NULL, NULL, NULL); }
    CHECK(fabs(ctl_get(ctl_lookup("[Channel1]", "bpm")) - 150) < 0.5, "beatsync: bpm %.2f (expect 150)", ctl_get(ctl_lookup("[Channel1]", "bpm")));

    /* ---- eject: audio thread detaches, loader reaps */
    ctl_set(ctl_lookup("[Channel1]", "eject"), 1); run(d, 2, NULL, NULL, NULL);
    CHECK(ctl_get(ctl_lookup("[Channel1]", "track_loaded")) == 0, "eject: track_loaded 0");
    deck_reap(d);
    CHECK(rx_wave_total_cols(0) == 0, "eject: no columns");

    for (int i = 0; i < RX_DECKS; i++) deck_destroy(decks[i]);
    mixer_destroy(mx); fx_destroy(fx);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails ? 1 : 0;
}
