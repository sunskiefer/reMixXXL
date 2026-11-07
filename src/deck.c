/* reMixXXL -- deck.c: one deck = Mixxx's EngineBuffer and its controls (RateControl, PositionScratchController,
 * LoopingControl, CueControl, BpmControl's sync, SlipModeState), in C, for the Force's audio thread.
 *
 * Threads:
 *   audio thread   deck_tick_controls() then deck_render() once per block (engine.c's render, under the wrapper's
 *                  lock). Never blocks, never mallocs, never frees.
 *   loader thread  deck_load() / deck_set_track() / deck_eject() / deck_reap() (engine.c's worker): decode + analyse,
 *                  then hand the track over through an atomic "pending" slot; the audio thread adopts it at its next
 *                  tick and drops the old one into a "retire" ring the loader frees later.
 *   screen thread  rx_wave_columns() / rx_overview(): copies columns of the live analysis under view_lock, which the
 *                  loader also takes while freeing a retired track (the audio thread never takes it).
 *
 * All designs marked "after Mixxx <file>" are ports of GPL-2.0-or-later code from Mixxx (src/...), used here in a
 * GPL-3 plugin. */
#define _GNU_SOURCE
#include "remix.h"
#include "deck.h"
#include "remix_params.h"
#include "dsp_priv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------------------- constants */
#define GROUP_LEN 24
#define MIN_SEEK_SPEED 0.010               /* Mixxx enginebufferscale.h */
#define RATE_SEARCH 4.0                    /* Mixxx RateControl::slotControlFastForward: rateSearch = +-4 */
#define WHEEL_MULT 40.0                    /* RateControl::kWheelMultiplier */
#define PAUSED_JOG_MULT 18.0               /* RateControl::kPausedJogMultiplier */
#define JOG_SENS 0.1                       /* RateControl::getJogFactor jogSensitivity */
#define JOG_FILTER_LEN 25                  /* Rotary(25) */
#define TEMP_COARSE 4.0                    /* Mixxx preference defaults (percent) */
#define TEMP_FINE 2.0
#define PERM_COARSE 0.5
#define PERM_FINE 0.05
#define HOLD_AUTORELEASE_BLOCKS 520        /* ~1.5 s: a press with no release (MPC params) is released by itself */
#define RETIRE_RING 4
#define OLA_HOP 882                        /* keylock OLA: 20 ms hop, 40 ms Hann grains */
#define OLA_GRAIN (2 * OLA_HOP)
#define TEXT_EVERY 8                       /* blocks between readback-text refreshes */
#define N_LOOP_SIZES ((int)(sizeof RX_LOOP_SIZES / sizeof RX_LOOP_SIZES[0]))

/* ---------------------------------------------------------------------------------------------------- the track */
typedef struct {
    rx_audio_t *au;
    rx_analysis_t *an;
    char path[RX_PATH];
} track_t;

static void track_free(track_t *t) {
    if (!t) return;
    if (t->au) { decode_free(t->au); free(t->au); }
    if (t->an) { analyze_free(t->an); free(t->an); }
    free(t);
}

/* ------------------------------------------------------------------------------ Mixxx PositionScratchController
 * after Mixxx src/engine/positionscratchcontroller.cpp: a PD velocity controller that chases a "scratch_position"
 * (a distance the user has dragged, in engine sample units) with an IIR-smoothed error. Runs once per block. */
typedef struct {
    double vc_last_error, vc_p, vc_d;     /* VelocityController */
    double ii_factor, ii_last_rate;       /* RateIIFilter */
    int is_scratching, inertia_enabled;
    double prev_sample_pos;
    double seek_sample_pos; int seek_valid;
    double sample_pos_delta_sum, scratch_target_delta, scratch_start_pos;
    double rate, move_delay, scratch_pos_sample_time;
    int buffer_size;
    double dt, calls_per_dt, calls_to_stop, p, d, f;
} psc_t;

static void psc_update_filter_params(psc_t *s, double sample_rate) {
    const double kDefaultSampleInterval = 0.016, kTimeToStop = 1.0;
    s->dt = (double)s->buffer_size / sample_rate / 2;
    s->calls_per_dt = ceil(kDefaultSampleInterval / s->dt);
    s->calls_to_stop = s->dt / kTimeToStop;
    s->p = 0.3; s->d = s->p / -2; s->f = 0.4;
    if (s->dt > kDefaultSampleInterval * 2) s->f = 1;
    s->vc_p = s->p; s->vc_d = s->d;
    s->ii_factor = s->f;
}
static double psc_ii_filter(psc_t *s, double rate) {
    if (fabs(rate) - fabs(s->ii_last_rate) > -0.1) s->ii_last_rate = s->ii_last_rate * (1 - s->ii_factor) + rate * s->ii_factor;
    else s->ii_last_rate = rate;     /* do not filter strong decelerations to avoid overshooting */
    return s->ii_last_rate;
}
static double psc_vc_observation(psc_t *s, double error) {
    s->vc_last_error = s->vc_p * error + s->vc_d * (error - s->vc_last_error);
    return s->vc_last_error;
}
static void psc_process(psc_t *s, int scratch_enable, double scratch_pos, double current_sample_pos, double release_rate,
                        int buffer_size, double base_sample_rate, int wrapped_around, double trigger_pos, double target_pos) {
    const double kDefaultSampleInterval = 0.016, kMoveDelayMax = 0.04, kThrowThreshold = 2.5, kMaxVelocity = 100;
    if (buffer_size != s->buffer_size) { s->buffer_size = buffer_size; psc_update_filter_params(s, RX_SR); }
    if (s->is_scratching) {
        if (s->inertia_enabled) {
            double decay_threshold = fabs(release_rate);
            if (decay_threshold < MIN_SEEK_SPEED) decay_threshold = MIN_SEEK_SPEED;
            double k = pow(decay_threshold / kMaxVelocity, s->calls_to_stop);
            s->rate *= k;
            if (fabs(s->rate) < decay_threshold || scratch_enable) { s->inertia_enabled = 0; s->is_scratching = 0; }
        } else if (scratch_enable) {
            s->inertia_enabled = 0;
            double sample_delta = current_sample_pos - s->prev_sample_pos;
            if (wrapped_around > 0) sample_delta += (trigger_pos - target_pos) * wrapped_around;
            s->sample_pos_delta_sum += sample_delta / (buffer_size * base_sample_rate);
            s->scratch_pos_sample_time += s->dt;
            if (s->scratch_pos_sample_time >= kDefaultSampleInterval) {
                s->scratch_pos_sample_time = 0;
                double scratch_target_delta = (scratch_pos - s->scratch_start_pos) / (buffer_size * base_sample_rate);
                int calc_rate = 1;
                if (s->scratch_target_delta == scratch_target_delta) {
                    s->move_delay += s->dt * s->calls_per_dt;
                    if (s->move_delay < kMoveDelayMax) calc_rate = 0;
                    else {
                        s->vc_p = s->p; s->vc_d = 0;
                        if (scratch_target_delta == 0) { s->vc_last_error = 0; s->ii_last_rate = 0; s->sample_pos_delta_sum = 0; }
                    }
                } else { s->move_delay = 0; s->scratch_target_delta = scratch_target_delta; }
                if (calc_rate) {
                    double err = psc_ii_filter(s, scratch_target_delta - s->sample_pos_delta_sum);
                    s->rate = psc_vc_observation(s, err) / s->calls_per_dt;
                    if (fabs(s->rate) < MIN_SEEK_SPEED) s->rate = 0;
                }
            }
        } else {
            if (fabs(s->rate) > kThrowThreshold) s->inertia_enabled = 1; else s->is_scratching = 0;
        }
    } else if (scratch_enable) {
        s->is_scratching = 1; s->inertia_enabled = 0; s->move_delay = 0;
        s->rate = release_rate;
        s->sample_pos_delta_sum = -(release_rate / s->p) * s->calls_per_dt;
        s->vc_last_error = -s->sample_pos_delta_sum;
        s->ii_last_rate = -s->sample_pos_delta_sum;
        s->vc_p = s->p; s->vc_d = s->d;
        s->scratch_start_pos = scratch_pos;
        s->scratch_pos_sample_time = 0;
    }
    if (s->seek_valid) { current_sample_pos = s->seek_sample_pos; s->seek_valid = 0; }
    s->prev_sample_pos = current_sample_pos;
}

/* ------------------------------------------------------------------------ Mixxx ControllerEngine scratch (wheel)
 * after Mixxx src/controllers/scripting/legacy/controllerscriptinterfacelegacy.cpp (scratchEnable/Tick/Process/
 * Disable) and src/util/alphabetafilter.h (xwax's pitch filter). Mixxx runs it on a 1 ms timer; here it runs once
 * per audio block with dt = block length, which the filter's maths is written in terms of. */
typedef struct { int inited; double dt, x, v, alpha, beta; } abf_t;
static void abf_init(abf_t *f, double dt, double v, double alpha, double beta) {
    f->inited = 1; f->dt = dt; f->x = 0; f->v = v; f->alpha = alpha; f->beta = beta;
}
static void abf_observation(abf_t *f, double dx) {
    if (!f->inited) return;
    double px = f->x + f->v * f->dt, pv = f->v, res = dx - px;
    f->x = px + res * f->alpha;
    f->v = pv + res * f->beta / f->dt;
    f->x -= dx;
}

/* ---------------------------------------------------------------------------------------------------- controls */
typedef struct {
    ctl_id play, start, end, start_play, start_stop, play_stutter, web_seek, jog_disp, jog_k[4], bpm_h, bpm_t, bpm_u;
    ctl_id cue_default, cue_set, cue_gotoandstop, cue_gotoandplay, cue_preview, cue_point, cue_indicator;
    ctl_id beatsync, beatsync_phase, beatsync_tempo, sync_enabled, sync_master, sync_mode;
    ctl_id slip_enabled, reverse, reverseroll, keylock, pitch, pitch_adjust, quantize, vinyl_mode;
    ctl_id hc_activate[RX_HOTCUES], hc_set[RX_HOTCUES], hc_clear[RX_HOTCUES], hc_goto[RX_HOTCUES], hc_gotoandplay[RX_HOTCUES],
           hc_gotoandstop[RX_HOTCUES], hc_position[RX_HOTCUES], hc_enabled[RX_HOTCUES], hc_color[RX_HOTCUES];
    ctl_id hotcue_clear_mode;
    ctl_id loop_in, loop_out, loop_exit, reloop_toggle, reloop_andstop, loop_halve, loop_double, loop_enabled,
           loop_start_position, loop_end_position, beatloop_size, beatloop_size_idx, beatjump_size, beatjump_size_idx,
           beatjump_forward, beatjump_backward, loop_move;
    ctl_id bl_activate[N_LOOP_SIZES], bl_toggle[N_LOOP_SIZES], blr_activate[N_LOOP_SIZES], bj_fwd[N_LOOP_SIZES], bj_back[N_LOOP_SIZES];
    ctl_id back, fwd, back_tap, fwd_tap, track_prev, track_next, eject;
    ctl_id rate, rateRange, rateRange_idx, rateRange_next, rate_dir, rate_ratio, rate_set_default,
           rate_temp_up, rate_temp_down, rate_temp_up_small, rate_temp_down_small,
           rate_perm_up, rate_perm_down, rate_perm_up_small, rate_perm_down_small, rateSearch;
    ctl_id jog, jog_feel, wheel, scratch2, scratch2_enable, scratch2_indicates_scratching, scratch_position, scratch_position_enable;
    ctl_id playposition, bpm, file_bpm, local_bpm, beat_distance, duration, track_samples, track_samplerate, track_loaded, beat_active;
    ctl_id pfl, volume, pregain, orientation, key, key_sync, cues_dirty, touch_zone, track_step;
    ctl_id bpm_text, time_text, title_text, key_text, rate_text;
    ctl_id eq[3], eq_kill[3], eq_enabled, qe_super1, qe_enabled;
} deck_ctls_t;

/* ---------------------------------------------------------------------------------------------------- the deck */
struct rx_deck {
    int index;
    char group[GROUP_LEN];
    deck_ctls_t c;

    /* track handover (see the file comment) */
    track_t *cur;                      /* audio thread writes, everyone reads (atomic) */
    track_t *pending;                  /* loader -> audio thread (atomic exchange) */
    track_t *retire[RETIRE_RING];      /* audio thread -> loader (SPSC ring) */
    unsigned ret_w, ret_r;
    pthread_mutex_t view_lock;         /* screen copies / loader frees */
    char path[RX_PATH];                /* the loaded track's path (loader's copy for deck_track_path) */

    /* playback state (audio thread) */
    double pos;                        /* play position, frames */
    int seek_pending; double seek_target, render_pos;   /* render_pos: where the audio actually is */
    double rate_old;                   /* rate used by the last block (ramp start) */
    double rate_now;                   /* rate of the current block (readback) */
    int scratching, reverse_now, silent_blocks;
    double gain_prev;                  /* replay gain * vinyl speed gain, last block */
    int wrap_count; double wrap_trigger, wrap_target;    /* loop wraps in the last block (for the scratch controller) */
    double platter;                    /* turns */
    double jog_prev, jog_idle, jp[4]; int jog_scr, ext_jog, jp_init; int64_t ext_seek_fp, ext_platter_fp;   /* the jog ring as a knob: last value seen, seconds since the finger last moved it, scratch on */
    int blocks;

    /* rate control */
    double temp_rate; int temp_started;
    double jog_hist[JOG_FILTER_LEN]; int jog_head;
    psc_t psc;
    /* wheel scratch (deck_scratch_*) */
    abf_t abf;
    int sc_req_enable, sc_active;      /* request from the wheel thread (atomic), state on the audio thread */
    double sc_rpm, sc_alpha, sc_beta;
    int32_t sc_ticks_fp;               /* accumulated ticks * 1024 (atomic add from the wheel thread) */
    double sc_dx;                      /* seconds per tick */
    int sc_ramp; double sc_ramp_to; int sc_ramp_blocks;
    int sc_was_playing;

    /* keylock OLA */
    struct { double rpos; int age; } grain[2];
    int ola_phase, ola_active;
    float win[OLA_GRAIN];

    /* loops (frames; -1 none) */
    double loop_in, loop_out; int loop_on, loop_on_before_slip, loop_is_beatloop;
    double beatloop_size, beatjump_size;
    int roll_held;                     /* a beatlooproll_X_activate is held */
    /* slip */
    int slip_on; double slip_pos, slip_rate;
    /* cues */
    double cue;                        /* frames, -1 none */
    double hotcue[RX_HOTCUES]; int hotcue_colour[RX_HOTCUES];
    int previewing;                    /* -1 none, 0..7 hotcue, 8 main cue */
    int cue_held, hc_held[RX_HOTCUES]; int cue_hold_blocks, hc_hold_blocks[RX_HOTCUES];
    int hc_prev_val[RX_HOTCUES], cue_prev_val, cue_preview_prev, play_prev;
    /* beat grid */
    double bpm, beat0;                 /* file bpm, first beat (seconds); from analysis or the "grid" override */
    double grid_bpm_override, grid_beat0_override; int grid_override;
    double key;                        /* Mixxx key index 1..24, 0 unknown */
    /* sync */
    double last_sync_adjust; int reset_sync_adjust; double user_offset;
    /* search */
    int back_held, fwd_held;
    /* staged cue import (loader -> audio): a fixed buffer and a flag; the loader waits while a previous one is unread */
    char import_buf[4096];
    int import_flag;
    /* texts */
    char t_bpm[24], t_time[24], t_title[96], t_key[16], t_rate[16];
    float vu_l, vu_r;                  /* pre-fader VU (Mixxx EngineVuMeter style, filled per block) */
    float vu_sum_l, vu_sum_r; int vu_n;
    double host_bpm; int host_playing;
};

static rx_deck_t *g_decks[RX_DECKS];

/* ------------------------------------------------------------------------------------------------- small helpers */
static inline track_t *cur_track(rx_deck_t *d) { return __atomic_load_n(&d->cur, __ATOMIC_ACQUIRE); }
static inline double frames_of(track_t *t) { return t && t->au ? (double)t->au->frames : 0; }
static inline double beat_len_frames(rx_deck_t *d) { return d->bpm > 0 ? 60.0 / d->bpm * RX_SR : 0; }
static inline double beat0_frames(rx_deck_t *d) { return d->beat0 * RX_SR; }
static inline double ctlv(ctl_id id) { return ctl_get(id); }
static inline int ctlb(ctl_id id) { return ctl_get(id) >= 0.5; }
/* a trigger: 1 if set, and reset */
static inline int take(ctl_id id) { if (ctl_get(id) >= 0.5) { ctl_set(id, 0); return 1; } return 0; }
static void set_text_if(ctl_id id, char *cache, int n, const char *s) {
    if (strncmp(cache, s, (size_t)n) != 0) { snprintf(cache, (size_t)n, "%s", s); ctl_set_text(id, cache); }
}
static void cues_dirty(rx_deck_t *d) { ctl_set(d->c.cues_dirty, 1); }

/* the beat nearest / before / after a frame position (requires a grid) */
static double nearest_beat(rx_deck_t *d, double pos) {
    double bl = beat_len_frames(d); if (bl <= 0) return pos;
    return beat0_frames(d) + round((pos - beat0_frames(d)) / bl) * bl;
}
static double prev_beat(rx_deck_t *d, double pos) {
    double bl = beat_len_frames(d); if (bl <= 0) return pos;
    double b = beat0_frames(d) + floor((pos - beat0_frames(d)) / bl + 1e-9) * bl;
    /* Mixxx uses the previous beat for a beatloop start; snap up when we are within 1/16 beat of the next one */
    if (pos - b > bl * (15.0 / 16.0)) b += bl;
    return b;
}
static double quantized(rx_deck_t *d, double pos) { return ctlb(d->c.quantize) ? nearest_beat(d, pos) : pos; }
static double clamp_pos(rx_deck_t *d, double p) {
    double n = frames_of(cur_track(d)); if (n <= 0) return 0;
    return p < 0 ? 0 : (p > n - 1 ? n - 1 : p);
}
/* a seek. As in Mixxx (EngineBuffer::processSeek runs after the scaler), it lands at the end of the block being
 * rendered, so a stop's ramp-down does not move the position off the cue. The position reads as the target at
 * once (the tick code and the view see it), the audio of the current block still comes from the old position. */
static void seek(rx_deck_t *d, double p) {
    d->seek_target = clamp_pos(d, p); d->seek_pending = 1;
    d->pos = d->seek_target;
}
static void apply_seek(rx_deck_t *d) {
    if (!d->seek_pending) return;
    d->seek_pending = 0;
    d->pos = clamp_pos(d, d->seek_target);
    d->psc.seek_sample_pos = d->pos * 2; d->psc.seek_valid = 1;     /* RateControl::notifySeek */
    d->ola_active = 0;
}
static void stop_deck(rx_deck_t *d) { ctl_set(d->c.play, 0); }

/* ------------------------------------------------------------------------------------------------ registration */
rx_deck_t *deck_create(int index) {
    rx_deck_t *d = calloc(1, sizeof *d);
    d->index = index;
    snprintf(d->group, sizeof d->group, "[Channel%d]", index + 1);
    pthread_mutex_init(&d->view_lock, NULL);
    d->loop_in = d->loop_out = -1; d->cue = -1;
    for (int i = 0; i < RX_HOTCUES; i++) d->hotcue[i] = -1, d->hotcue_colour[i] = i % 8;
    d->previewing = -1;
    d->beatloop_size = 4; d->beatjump_size = 4;
    d->gain_prev = 1.0;
    d->last_sync_adjust = 1.0;
    for (int i = 0; i < OLA_GRAIN; i++) d->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)OLA_GRAIN);
    d->psc.buffer_size = RX_BLOCK * 2; psc_update_filter_params(&d->psc, RX_SR);
    if (index >= 0 && index < RX_DECKS) g_decks[index] = d;
    return d;
}

static ctl_id reg(rx_deck_t *d, const char *name, int kind, double lo, double hi, double def) {
    return ctl_register(d->group, name, kind, lo, hi, def);
}
/* Mixxx names its beatloop controls "beatloop_0.125_activate", "beatloop_4_activate", "beatjump_0.5_forward"... */
static void size_token(double s, char *buf, int n) {
    if (s >= 1) snprintf(buf, (size_t)n, "%d", (int)s);
    else snprintf(buf, (size_t)n, "%g", s);
}

void deck_register_controls(rx_deck_t *d) {
    deck_ctls_t *c = &d->c;
    char n[64], tok[16];
    c->play = reg(d, "play", CK_TOGGLE, 0, 1, 0);
    c->start = reg(d, "start", CK_TRIGGER, 0, 1, 0);
    c->bpm_h = reg(d, "bpm_h", CK_FLOAT, 0, 1, 0); c->bpm_t = reg(d, "bpm_t", CK_FLOAT, 0, 1, 0); c->bpm_u = reg(d, "bpm_u", CK_FLOAT, 0, 1, 0);   /* ours: the jog centre's BPM number, a digit a picture: 0..9 = 0..0.9, 1.0 = blank */
    c->jog_k[0] = reg(d, "jog_l", CK_FLOAT, 0, 1, 0.5); c->jog_k[1] = reg(d, "jog_r", CK_FLOAT, 0, 1, 0.5);
    c->jog_k[2] = reg(d, "jog_t", CK_FLOAT, 0, 1, 0.5); c->jog_k[3] = reg(d, "jog_b", CK_FLOAT, 0, 1, 0.5);    /* ours: touch strips round the jog ring, endless: moved by a finger, put back to 0.5 by the engine */
    c->jog_disp = reg(d, "jog_disp", CK_FLOAT, 0, 1, 0);                   /* ours: the jog ring picture's film-strip frame (RX_JOG_FRAMES over 15 degrees of the 24-dimple ring), turned with the platter */
    c->web_seek = reg(d, "web_seek", CK_FLOAT, -1, 100000, -1);          /* ours: the companion page taps a spot on the waveform: seconds, -1 = none */
    c->end = reg(d, "end", CK_TRIGGER, 0, 1, 0);
    c->start_play = reg(d, "start_play", CK_TRIGGER, 0, 1, 0);
    c->start_stop = reg(d, "start_stop", CK_TRIGGER, 0, 1, 0);
    c->play_stutter = reg(d, "play_stutter", CK_TRIGGER, 0, 1, 0);
    c->cue_default = reg(d, "cue_default", CK_TRIGGER, 0, 1, 0);
    c->cue_set = reg(d, "cue_set", CK_TRIGGER, 0, 1, 0);
    c->cue_gotoandstop = reg(d, "cue_gotoandstop", CK_TRIGGER, 0, 1, 0);
    c->cue_gotoandplay = reg(d, "cue_gotoandplay", CK_TRIGGER, 0, 1, 0);
    c->cue_preview = reg(d, "cue_preview", CK_TOGGLE, 0, 1, 0);
    c->cue_point = reg(d, "cue_point", CK_FLOAT, -1, 1e12, -1);
    c->cue_indicator = reg(d, "cue_indicator", CK_TOGGLE, 0, 1, 0);
    c->beatsync = reg(d, "beatsync", CK_TRIGGER, 0, 1, 0);
    c->beatsync_phase = reg(d, "beatsync_phase", CK_TRIGGER, 0, 1, 0);
    c->beatsync_tempo = reg(d, "beatsync_tempo", CK_TRIGGER, 0, 1, 0);
    c->sync_enabled = reg(d, "sync_enabled", CK_TOGGLE, 0, 1, 0);
    c->sync_master = reg(d, "sync_master", CK_TOGGLE, 0, 1, 0);
    ctl_register(d->group, "sync_leader", CK_TOGGLE, 0, 1, 0);      /* alias name in newer Mixxx (separate value; we read sync_master) */
    c->sync_mode = reg(d, "sync_mode", CK_INT, 0, 2, 0);
    c->slip_enabled = reg(d, "slip_enabled", CK_TOGGLE, 0, 1, 0);
    c->reverse = reg(d, "reverse", CK_TOGGLE, 0, 1, 0);
    c->reverseroll = reg(d, "reverseroll", CK_TOGGLE, 0, 1, 0);
    c->keylock = reg(d, "keylock", CK_TOGGLE, 0, 1, 0);
    c->pitch = reg(d, "pitch", CK_FLOAT, -6, 6, 0);
    c->pitch_adjust = reg(d, "pitch_adjust", CK_FLOAT, -3, 3, 0);
    c->quantize = reg(d, "quantize", CK_TOGGLE, 0, 1, 1);
    c->vinyl_mode = reg(d, "vinyl_mode", CK_TOGGLE, 0, 1, 1);
    for (int i = 0; i < RX_HOTCUES; i++) {
        snprintf(n, sizeof n, "hotcue_%d_activate", i + 1); c->hc_activate[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_set", i + 1); c->hc_set[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_clear", i + 1); c->hc_clear[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_goto", i + 1); c->hc_goto[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_gotoandplay", i + 1); c->hc_gotoandplay[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_gotoandstop", i + 1); c->hc_gotoandstop[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_position", i + 1); c->hc_position[i] = reg(d, n, CK_FLOAT, -1, 1e12, -1);
        snprintf(n, sizeof n, "hotcue_%d_enabled", i + 1); c->hc_enabled[i] = reg(d, n, CK_TOGGLE, 0, 1, 0);
        snprintf(n, sizeof n, "hotcue_%d_color", i + 1); c->hc_color[i] = reg(d, n, CK_INT, 0, 7, i % 8);
    }
    c->hotcue_clear_mode = reg(d, "hotcue_clear_mode", CK_TOGGLE, 0, 1, 0);
    c->loop_in = reg(d, "loop_in", CK_TRIGGER, 0, 1, 0);
    c->loop_out = reg(d, "loop_out", CK_TRIGGER, 0, 1, 0);
    c->loop_exit = reg(d, "loop_exit", CK_TRIGGER, 0, 1, 0);
    c->reloop_toggle = reg(d, "reloop_toggle", CK_TRIGGER, 0, 1, 0);
    c->reloop_andstop = reg(d, "reloop_andstop", CK_TRIGGER, 0, 1, 0);
    c->loop_halve = reg(d, "loop_halve", CK_TRIGGER, 0, 1, 0);
    c->loop_double = reg(d, "loop_double", CK_TRIGGER, 0, 1, 0);
    c->loop_enabled = reg(d, "loop_enabled", CK_TOGGLE, 0, 1, 0);
    c->loop_start_position = reg(d, "loop_start_position", CK_FLOAT, -1, 1e12, -1);
    c->loop_end_position = reg(d, "loop_end_position", CK_FLOAT, -1, 1e12, -1);
    c->beatloop_size = reg(d, "beatloop_size", CK_FLOAT, 0.03125, 512, 4);
    c->beatloop_size_idx = reg(d, "beatloop_size_idx", CK_INT, 0, N_LOOP_SIZES - 1, 5);
    c->beatjump_size = reg(d, "beatjump_size", CK_FLOAT, 0.03125, 512, 4);
    c->beatjump_size_idx = reg(d, "beatjump_size_idx", CK_INT, 0, N_LOOP_SIZES - 1, 5);
    c->beatjump_forward = reg(d, "beatjump_forward", CK_TRIGGER, 0, 1, 0);
    c->beatjump_backward = reg(d, "beatjump_backward", CK_TRIGGER, 0, 1, 0);
    c->loop_move = reg(d, "loop_move", CK_FLOAT, -512, 512, 0);
    for (int i = 0; i < N_LOOP_SIZES; i++) {
        size_token(RX_LOOP_SIZES[i], tok, sizeof tok);
        snprintf(n, sizeof n, "beatloop_%s_activate", tok); c->bl_activate[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "beatloop_%s_toggle", tok); c->bl_toggle[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "beatlooproll_%s_activate", tok); c->blr_activate[i] = reg(d, n, CK_TOGGLE, 0, 1, 0);
        snprintf(n, sizeof n, "beatjump_%s_forward", tok); c->bj_fwd[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "beatjump_%s_backward", tok); c->bj_back[i] = reg(d, n, CK_TRIGGER, 0, 1, 0);
    }
    c->back = reg(d, "back", CK_TOGGLE, 0, 1, 0);
    c->fwd = reg(d, "fwd", CK_TOGGLE, 0, 1, 0);
    c->back_tap = reg(d, "back_tap", CK_TRIGGER, 0, 1, 0);
    c->fwd_tap = reg(d, "fwd_tap", CK_TRIGGER, 0, 1, 0);
    c->track_prev = reg(d, "track_prev", CK_TRIGGER, 0, 1, 0);
    c->track_next = reg(d, "track_next", CK_TRIGGER, 0, 1, 0);
    c->eject = reg(d, "eject", CK_TRIGGER, 0, 1, 0);
    c->rate = reg(d, "rate", CK_FLOAT, -1, 1, 0);
    c->rateRange = reg(d, "rateRange", CK_FLOAT, 0.01, 4.0, 0.10);
    c->rateRange_idx = reg(d, "rateRange_idx", CK_INT, 0, 3, 1);
    c->rateRange_next = reg(d, "rateRange_next", CK_TRIGGER, 0, 1, 0);
    c->rate_dir = reg(d, "rate_dir", CK_FLOAT, -1, 1, -1);      /* Mixxx default: the slider up = slower (the + at the bottom, like a CDJ) */
    c->rate_ratio = reg(d, "rate_ratio", CK_FLOAT, 0, 5, 1);
    c->rate_set_default = reg(d, "rate_set_default", CK_TRIGGER, 0, 1, 0);
    c->rate_temp_up = reg(d, "rate_temp_up", CK_TOGGLE, 0, 1, 0);
    c->rate_temp_down = reg(d, "rate_temp_down", CK_TOGGLE, 0, 1, 0);
    c->rate_temp_up_small = reg(d, "rate_temp_up_small", CK_TOGGLE, 0, 1, 0);
    c->rate_temp_down_small = reg(d, "rate_temp_down_small", CK_TOGGLE, 0, 1, 0);
    c->rate_perm_up = reg(d, "rate_perm_up", CK_TRIGGER, 0, 1, 0);
    c->rate_perm_down = reg(d, "rate_perm_down", CK_TRIGGER, 0, 1, 0);
    c->rate_perm_up_small = reg(d, "rate_perm_up_small", CK_TRIGGER, 0, 1, 0);
    c->rate_perm_down_small = reg(d, "rate_perm_down_small", CK_TRIGGER, 0, 1, 0);
    c->rateSearch = reg(d, "rateSearch", CK_FLOAT, -300, 300, 0);
    c->jog = reg(d, "jog", CK_FLOAT, -1e6, 1e6, 0);
    c->jog_feel = reg(d, "jog_feel", CK_FLOAT, 0, 1, 0.5);
    c->wheel = reg(d, "wheel", CK_FLOAT, -1e6, 1e6, 0);
    c->scratch2 = reg(d, "scratch2", CK_FLOAT, -1e6, 1e6, 0);
    c->scratch2_enable = reg(d, "scratch2_enable", CK_TOGGLE, 0, 1, 0);
    c->scratch2_indicates_scratching = reg(d, "scratch2_indicates_scratching", CK_TOGGLE, 0, 1, 1);
    c->scratch_position = reg(d, "scratch_position", CK_FLOAT, -1e12, 1e12, 0);
    c->scratch_position_enable = reg(d, "scratch_position_enable", CK_TOGGLE, 0, 1, 0);
    c->playposition = reg(d, "playposition", CK_FLOAT, 0, 1, 0);
    c->bpm = reg(d, "bpm", CK_FLOAT, 0, 999, 0);
    c->file_bpm = reg(d, "file_bpm", CK_FLOAT, 0, 999, 0);
    c->local_bpm = reg(d, "local_bpm", CK_FLOAT, 0, 999, 0);
    c->beat_distance = reg(d, "beat_distance", CK_FLOAT, 0, 1, 0);
    c->duration = reg(d, "duration", CK_FLOAT, 0, 1e6, 0);
    c->track_samples = reg(d, "track_samples", CK_FLOAT, 0, 1e12, 0);
    c->track_samplerate = reg(d, "track_samplerate", CK_FLOAT, 0, 1e6, RX_SR);
    c->track_loaded = reg(d, "track_loaded", CK_TOGGLE, 0, 1, 0);
    c->beat_active = reg(d, "beat_active", CK_TOGGLE, 0, 1, 0);
    c->pfl = reg(d, "pfl", CK_TOGGLE, 0, 1, 0);
    c->volume = reg(d, "volume", CK_FLOAT, 0, 1, 1);
    c->pregain = reg(d, "pregain", CK_FLOAT, 0, 4, 1);
    c->orientation = reg(d, "orientation", CK_INT, 0, 2, (d->index & 1) ? 2 : 0);   /* decks 1 and 3 on the left of the crossfader, 2 and 4 on the right */
    c->key = reg(d, "key", CK_FLOAT, 0, 24, 0);
    c->key_sync = reg(d, "key_sync", CK_TRIGGER, 0, 1, 0);
    c->cues_dirty = reg(d, "cues_dirty", CK_TOGGLE, 0, 1, 0);
    c->touch_zone = reg(d, "touch_zone", CK_INT, 0, 2, 0);
    c->track_step = ctl_lookup(d->group, "track_step");      /* engine.c registers it; -1/+1 written here */
    c->bpm_text = reg(d, "bpm_text", CK_TEXT, 0, 0, 0);
    c->time_text = reg(d, "time_text", CK_TEXT, 0, 0, 0);
    c->title_text = reg(d, "title_text", CK_TEXT, 0, 0, 0);
    c->key_text = reg(d, "key_text", CK_TEXT, 0, 0, 0);
    c->rate_text = reg(d, "rate_text", CK_TEXT, 0, 0, 0);
    /* EQ and quick effect groups (the mixer reads them) */
    char g[48];
    snprintf(g, sizeof g, "[EqualizerRack1_%s_Effect1]", d->group);
    for (int i = 0; i < 3; i++) {
        snprintf(n, sizeof n, "parameter%d", i + 1); c->eq[i] = ctl_register(g, n, CK_FLOAT, 0, 4, 1);
        snprintf(n, sizeof n, "button_parameter%d", i + 1); c->eq_kill[i] = ctl_register(g, n, CK_TOGGLE, 0, 1, 0);
    }
    c->eq_enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 1);
    snprintf(g, sizeof g, "[QuickEffectRack1_%s]", d->group);
    c->qe_super1 = ctl_register(g, "super1", CK_FLOAT, 0, 1, 0.5);
    c->qe_enabled = ctl_register(g, "enabled", CK_TOGGLE, 0, 1, 1);
    ctl_set_text(c->title_text, "-"); ctl_set_text(c->bpm_text, "-"); ctl_set_text(c->time_text, "0:00");
    ctl_set_text(c->key_text, "-"); ctl_set_text(c->rate_text, "+0.0%");
    snprintf(d->t_title, sizeof d->t_title, "-"); snprintf(d->t_bpm, sizeof d->t_bpm, "-");
    snprintf(d->t_time, sizeof d->t_time, "0:00"); snprintf(d->t_key, sizeof d->t_key, "-"); snprintf(d->t_rate, sizeof d->t_rate, "+0.0%%");
}

int deck_index(rx_deck_t *d) { return d->index; }
const char *deck_track_path(rx_deck_t *d) { return d->path; }
void deck_host_tempo(rx_deck_t *d, double bpm, int playing) { d->host_bpm = bpm; d->host_playing = playing; }
double deck_current_rate(rx_deck_t *d) { return d->rate_now; }

/* ------------------------------------------------------------------------------------------------ track handover */
void deck_reap(rx_deck_t *d) {
    while (d->ret_r != __atomic_load_n(&d->ret_w, __ATOMIC_ACQUIRE)) {
        track_t *t = d->retire[d->ret_r % RETIRE_RING];
        pthread_mutex_lock(&d->view_lock);      /* a screen copy in flight finishes first */
        track_free(t);
        pthread_mutex_unlock(&d->view_lock);
        __atomic_store_n(&d->ret_r, d->ret_r + 1, __ATOMIC_RELEASE);
    }
}

int deck_set_track(rx_deck_t *d, const char *path, rx_audio_t *audio, rx_analysis_t *an) {
    if (!audio || audio->frames <= 0 || !audio->lr) return -1;
    deck_reap(d);
    track_t *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->au = audio; t->an = an;
    snprintf(t->path, sizeof t->path, "%s", path ? path : "");
    snprintf(d->path, sizeof d->path, "%s", t->path);
    track_t *old = __atomic_exchange_n(&d->pending, t, __ATOMIC_ACQ_REL);
    if (old) track_free(old);                   /* the audio thread never saw it */
    /* give the audio thread a moment to adopt it, so a following deck_cues_import lands on this track */
    for (int i = 0; i < 100 && __atomic_load_n(&d->pending, __ATOMIC_ACQUIRE) == t; i++) usleep(2000);
    return 0;
}

static void load_progress(void *u, int pct) { (void)u; (void)pct; }

void deck_load(rx_deck_t *d, const char *path) {
    rx_audio_t *au = calloc(1, sizeof *au);
    if (!au) return;
    int fr = decode_file(path, au, RX_MAX_TRACK_SEC * RX_SR, load_progress, d);
    if (fr <= 0) { rx_log("deck %d: decode failed (%d): %s", d->index + 1, fr, path); free(au); return; }
    rx_analysis_t *an = calloc(1, sizeof *an);
    if (an && analyze_audio(au->lr, au->frames, an, load_progress, d) != 0) { analyze_free(an); free(an); an = NULL; }
    if (deck_set_track(d, path, au, an)) { decode_free(au); free(au); if (an) { analyze_free(an); free(an); } }
}

void deck_eject(rx_deck_t *d) {
    ctl_set(d->c.eject, 1);                     /* the audio thread detaches at its next tick */
    for (int i = 0; i < 100 && cur_track(d); i++) usleep(2000);
    deck_reap(d);
    d->path[0] = 0;
}

void deck_destroy(rx_deck_t *d) {
    if (!d) return;
    if (d->index >= 0 && d->index < RX_DECKS && g_decks[d->index] == d) g_decks[d->index] = NULL;
    deck_reap(d);
    track_free(__atomic_exchange_n(&d->pending, NULL, __ATOMIC_ACQ_REL));
    track_free(__atomic_exchange_n(&d->cur, NULL, __ATOMIC_ACQ_REL));
    pthread_mutex_destroy(&d->view_lock);
    free(d);
}

/* audio thread: adopt a pending track / detach on eject */
static void retire_track(rx_deck_t *d, track_t *t) {
    if (!t) return;
    unsigned w = d->ret_w;
    if (w - __atomic_load_n(&d->ret_r, __ATOMIC_ACQUIRE) >= RETIRE_RING) return;   /* ring full: keep it (leak-free: still in cur) */
    d->retire[w % RETIRE_RING] = t;
    __atomic_store_n(&d->ret_w, w + 1, __ATOMIC_RELEASE);
}
static void reset_for_track(rx_deck_t *d, track_t *t) {
    d->pos = 0; d->render_pos = 0; d->seek_pending = 0; d->rate_old = 0; d->ola_active = 0;
    d->loop_in = d->loop_out = -1; d->loop_on = 0; d->slip_on = 0; ctl_set(d->c.slip_enabled, 0);
    d->cue = t ? 0 : -1; d->previewing = -1; d->cue_held = 0;
    for (int i = 0; i < RX_HOTCUES; i++) { d->hotcue[i] = -1; d->hc_held[i] = 0; }
    d->bpm = (t && t->an) ? t->an->bpm : 0; d->beat0 = (t && t->an) ? t->an->beat0 : 0;
    d->grid_override = 0; d->key = 0;
    d->psc.is_scratching = 0; d->psc.inertia_enabled = 0; d->psc.rate = 0;    /* PositionScratchController::reset */
    d->sc_active = 0; ctl_set(d->c.scratch2_enable, 0); ctl_set(d->c.scratch2, 0);
    d->temp_rate = 0; d->last_sync_adjust = 1.0;
    stop_deck(d);
    ctl_set(d->c.track_loaded, t ? 1 : 0);
    ctl_set(d->c.duration, t ? frames_of(t) / RX_SR : 0);
    ctl_set(d->c.track_samples, t ? frames_of(t) * 2 : 0);
    ctl_set(d->c.file_bpm, d->bpm); ctl_set(d->c.local_bpm, d->bpm);
    ctl_set(d->c.key, 0);
}
static void adopt_pending(rx_deck_t *d) {
    track_t *t = __atomic_exchange_n(&d->pending, NULL, __ATOMIC_ACQ_REL);
    if (!t) return;
    track_t *old = cur_track(d);
    __atomic_store_n(&d->cur, t, __ATOMIC_RELEASE);
    retire_track(d, old);
    reset_for_track(d, t);
}

/* ------------------------------------------------------------------------------------------------ cue text form */
void deck_cues_export(rx_deck_t *d, char *buf, int n) {
    int k = 0;
    if (n <= 0) return;
    buf[0] = 0;
    for (int i = 0; i < RX_HOTCUES; i++)
        if (d->hotcue[i] >= 0) k += snprintf(buf + k, (size_t)(n - k > 0 ? n - k : 0), "hot %d %.4f %d\n", i + 1, d->hotcue[i] / RX_SR, d->hotcue_colour[i]);
    if (d->cue >= 0) k += snprintf(buf + k, (size_t)(n - k > 0 ? n - k : 0), "cue %.4f\n", d->cue / RX_SR);
    if (d->loop_in >= 0 && d->loop_out > d->loop_in) k += snprintf(buf + k, (size_t)(n - k > 0 ? n - k : 0), "loop %.4f %.4f\n", d->loop_in / RX_SR, d->loop_out / RX_SR);
    if (d->grid_override) k += snprintf(buf + k, (size_t)(n - k > 0 ? n - k : 0), "grid %.4f %.6f\n", d->grid_bpm_override, d->grid_beat0_override);
    if (d->key > 0) k += snprintf(buf + k, (size_t)(n - k > 0 ? n - k : 0), "key %d\n", (int)d->key);
    if (k >= n) buf[n - 1] = 0;
}
void deck_cues_import(rx_deck_t *d, const char *text) {
    if (!text) return;
    for (int i = 0; i < 100 && __atomic_load_n(&d->import_flag, __ATOMIC_ACQUIRE); i++) usleep(2000);
    if (__atomic_load_n(&d->import_flag, __ATOMIC_ACQUIRE)) return;    /* audio thread not running: drop it */
    snprintf(d->import_buf, sizeof d->import_buf, "%s", text);
    __atomic_store_n(&d->import_flag, 1, __ATOMIC_RELEASE);
}
static void apply_import(rx_deck_t *d) {
    if (!__atomic_load_n(&d->import_flag, __ATOMIC_ACQUIRE)) return;
    const char *text = d->import_buf;
    double n = frames_of(cur_track(d));
    const char *s = text;
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        char line[128]; if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len); line[len] = 0;
        int i, col; double a, b;
        if (sscanf(line, "hot %d %lf %d", &i, &a, &col) == 3 && i >= 1 && i <= RX_HOTCUES) { d->hotcue[i - 1] = rx_clamp(a * RX_SR, 0, n > 0 ? n - 1 : 0); d->hotcue_colour[i - 1] = col & 7; }
        else if (sscanf(line, "cue %lf", &a) == 1) d->cue = rx_clamp(a * RX_SR, 0, n > 0 ? n - 1 : 0);
        else if (sscanf(line, "loop %lf %lf", &a, &b) == 2 && b > a) { d->loop_in = a * RX_SR; d->loop_out = b * RX_SR; d->loop_is_beatloop = 0; }
        else if (sscanf(line, "grid %lf %lf", &a, &b) == 2 && a > 0) { d->grid_override = 1; d->grid_bpm_override = a; d->grid_beat0_override = b; d->bpm = a; d->beat0 = b; ctl_set(d->c.file_bpm, a); ctl_set(d->c.local_bpm, a); }
        else if (sscanf(line, "key %d", &i) == 1 && i >= 0 && i <= 24) { d->key = i; ctl_set(d->c.key, i); }
        s += len + (nl ? 1 : 0);
    }
    cues_dirty(d);                         /* engine.c saves the (possibly normalised) set again; harmless */
    __atomic_store_n(&d->import_flag, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------------------------- screen data access */
int rx_wave_total_cols(int deck) {
    if (deck < 0 || deck >= RX_DECKS || !g_decks[deck]) return 0;
    track_t *t = cur_track(g_decks[deck]);
    return (t && t->an) ? t->an->ncols : 0;
}
int rx_wave_columns(int deck, rx_wave_col_t *out, int first_col, int n) {
    if (deck < 0 || deck >= RX_DECKS || !g_decks[deck] || n <= 0) return 0;
    rx_deck_t *d = g_decks[deck];
    int got = 0;
    pthread_mutex_lock(&d->view_lock);
    track_t *t = cur_track(d);
    if (t && t->an && t->an->cols) {
        int a = first_col < 0 ? 0 : first_col, b = first_col + n;
        if (b > t->an->ncols) b = t->an->ncols;
        if (b > a) { memcpy(out + (a - first_col), t->an->cols + a, (size_t)(b - a) * sizeof *out); got = b - a; }
        if (a > first_col) memset(out, 0, (size_t)(a - first_col) * sizeof *out);
        if (b < first_col + n && b >= a) memset(out + (b - first_col), 0, (size_t)(first_col + n - b) * sizeof *out);
    } else memset(out, 0, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->view_lock);
    return got;
}
int rx_overview(int deck, rx_wave_col_t *out) {
    if (deck < 0 || deck >= RX_DECKS || !g_decks[deck]) return 0;
    rx_deck_t *d = g_decks[deck];
    int ok = 0;
    pthread_mutex_lock(&d->view_lock);
    track_t *t = cur_track(d);
    if (t && t->an) { memcpy(out, t->an->overview, sizeof t->an->overview); ok = 1; }
    pthread_mutex_unlock(&d->view_lock);
    if (!ok) memset(out, 0, sizeof(rx_wave_col_t) * RX_OVERVIEW_W);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ wheel scratch */
void deck_scratch_enable(rx_deck_t *d, int enable, double rpm, double alpha, double beta) {
    if (enable) {
        d->sc_rpm = rpm > 0 ? rpm : 33.0 + 1.0 / 3.0;
        d->sc_alpha = alpha; d->sc_beta = beta;
        __atomic_store_n(&d->sc_req_enable, 1, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&d->sc_req_enable, 2, __ATOMIC_RELEASE);     /* 2 = disable with ramp (Mixxx scratchDisable(ramp=true)) */
    }
}
void deck_scratch_tick(rx_deck_t *d, double ticks) {
    __atomic_fetch_add(&d->sc_ticks_fp, (int32_t)lrint(ticks * 1024.0), __ATOMIC_ACQ_REL);
}
int deck_is_playing(rx_deck_t *d) { return ctlb(d->c.play); }
/* a finger turning the wheel on a stopped deck (touch.c): turns of the wheel, queued for the engine thread, which moves the playhead */
void deck_seek_by_turns(rx_deck_t *d, double turns) { __atomic_fetch_add(&d->ext_seek_fp, (int64_t)llround(turns * 1.8 * RX_SR * 16.0), __ATOMIC_ACQ_REL); }
void deck_ext_jog(rx_deck_t *d, int on) { __atomic_store_n(&d->ext_jog, on, __ATOMIC_RELEASE); }
void deck_jog(rx_deck_t *d, double amount) {
    ctl_post_delta(d->c.jog, amount);
    __atomic_fetch_add(&d->ext_platter_fp, (int64_t)llround(amount / 12.566 * 1e6), __ATOMIC_ACQ_REL);   /* CDJ mode: the wheel turns by what the finger adjusted (a turn = 12.6 jog units) */
}

/* the per-block part of ControllerEngine::scratchEnable/scratchProcess/scratchDisable, on the audio thread.
 * intervalsPerRev: Mixxx's resolution argument; here fixed to touch.c/midi.c's convention through deck_scratch_tick's
 * tick unit: a tick is 1/720 of a revolution (SCRATCH_TICKS_PER_REV in touch.c; a MIDI mapping's JS passes its own
 * intervalsPerRev to midi.c which rescales to this). */
#define TICKS_PER_REV 720.0
static void scratch_block(rx_deck_t *d, int frames) {
    double dt = (double)frames / RX_SR;
    int req = __atomic_exchange_n(&d->sc_req_enable, 0, __ATOMIC_ACQ_REL);
    if (req == 1) {
        double ips = d->sc_rpm * TICKS_PER_REV / 60.0;
        d->sc_dx = 1.0 / ips;
        double v0 = 0;      /* ramp=true in Mixxx's call from touch: start from the current speed */
        if (d->sc_active) v0 = ctlv(d->c.scratch2);
        else if (ctlb(d->c.play)) v0 = d->rate_now;
        if (d->sc_alpha != 0 && d->sc_beta != 0) abf_init(&d->abf, dt, v0, d->sc_alpha, d->sc_beta);
        else abf_init(&d->abf, dt, v0, 1.0 / 512, (1.0 / 512) / 1024);
        d->sc_active = 1; d->sc_ramp = 0; d->sc_ramp_blocks = 0;
        __atomic_store_n(&d->sc_ticks_fp, 0, __ATOMIC_RELEASE);
        ctl_set(d->c.scratch2_enable, 1);
    } else if (req == 2 && d->sc_active) {
        d->sc_ramp = 1; d->sc_ramp_blocks = 0;
        d->sc_ramp_to = ctlb(d->c.play) ? (1.0 + ctlv(d->c.rate_dir) * ctlv(d->c.rate) * ctlv(d->c.rateRange)) * (ctlb(d->c.reverse) ? -1 : 1) : 0.0;
    }
    if (!d->sc_active) return;
    int32_t ticks_fp = __atomic_exchange_n(&d->sc_ticks_fp, 0, __ATOMIC_ACQ_REL);
    double ticks = ticks_fp / 1024.0;
    /* jog_feel (ours): 0.5 = Mixxx's feel; lower = less sensitive, higher = more (x0.25 .. x4) */
    double feel = pow(4.0, (ctlv(d->c.jog_feel) - 0.5) * 2.0);
    if (d->sc_ramp && ticks == 0) abf_observation(&d->abf, d->sc_ramp_to * dt);     /* spinback / settle to play speed */
    else abf_observation(&d->abf, d->sc_dx * ticks * feel);
    double v = d->abf.v;
    ctl_set(d->c.scratch2, v);
    if (d->sc_ramp && (fabs(d->sc_ramp_to - v) <= 0.00001 || ++d->sc_ramp_blocks > 400 || !cur_track(d))) {
        d->sc_active = 0; d->sc_ramp = 0;
        ctl_set(d->c.scratch2_enable, 0);
        ctl_set(d->c.scratch2, 0);
    }
}

/* ------------------------------------------------------------------------------------------------- rate control */
/* after Mixxx RateControl::processTempRate, RampMode::Stepping (the default), coarse 4 %, fine 2 % */
static void process_temp_rate(rx_deck_t *d) {
    int dir = 0;
    if (ctlb(d->c.rate_temp_up)) dir = 1; else if (ctlb(d->c.rate_temp_down)) dir = 2;
    else if (ctlb(d->c.rate_temp_up_small)) dir = 3; else if (ctlb(d->c.rate_temp_down_small)) dir = 4;
    if (dir) {
        if (!d->temp_started) {
            d->temp_started = 1;
            double c = TEMP_COARSE / 100.0, s = TEMP_FINE / 100.0;
            d->temp_rate = dir == 1 ? c : dir == 2 ? -c : dir == 3 ? s : -s;
        }
    } else if (d->temp_started) { d->temp_started = 0; d->temp_rate = 0; }
}
/* RateControl::getJogFactor: the jog accumulator through a 25-tap moving average, times 0.1 */
static double jog_factor(rx_deck_t *d) {
    double v = ctlv(d->c.jog);
    if (v != 0) ctl_set(d->c.jog, 0);
    if (isnan(v)) v = 0;
    d->jog_head = (d->jog_head + 1) % JOG_FILTER_LEN;
    d->jog_hist[d->jog_head] = v;
    double s = 0; for (int i = 0; i < JOG_FILTER_LEN; i++) s += d->jog_hist[i];
    return s / JOG_FILTER_LEN * JOG_SENS;
}

/* sync: the deck this one follows (a sync master other than us, else the other playing deck with a grid) */
static rx_deck_t *sync_target(rx_deck_t *d) {
    for (int i = 0; i < RX_DECKS; i++) {
        rx_deck_t *o = g_decks[i];
        if (!o || o == d || !cur_track(o)) continue;
        if (ctlb(o->c.sync_master) && o->bpm > 0) return o;
    }
    for (int i = 0; i < RX_DECKS; i++) {
        rx_deck_t *o = g_decks[i];
        if (!o || o == d || !cur_track(o)) continue;
        if (ctlb(o->c.play) && o->bpm > 0) return o;
    }
    return NULL;
}
static double beat_distance_of(rx_deck_t *d, double pos) {
    double bl = beat_len_frames(d); if (bl <= 0) return 0;
    double x = fmod((pos - beat0_frames(d)) / bl, 1.0); if (x < 0) x += 1;
    return x;
}
/* BpmControl::shortestPercentageChange */
static double shortest_pct_change(double cur, double target) {
    if (cur == target) return 0;
    if (cur < target) { double f = target - cur, b = target - cur - 1.0; return fabs(f) < fabs(b) ? f : b; }
    double f = 1.0 - cur + target, b = target - cur; return fabs(f) < fabs(b) ? f : b;
}
/* BpmControl::calcSyncAdjustment (phase PD) and calcSyncedRate */
static double calc_synced_rate(rx_deck_t *d, rx_deck_t *t, double user_tweak) {
    double rate = 1.0;
    double target_bpm = t->bpm * (ctlb(t->c.play) ? fabs(t->rate_now) : fabs(ctlv(t->c.rate_ratio)));   /* the target's bpm */
    if (d->bpm > 0 && target_bpm > 0) rate = target_bpm / d->bpm;
    if (!ctlb(d->c.quantize) || d->bpm <= 0 || ctlb(d->c.reverse) || !ctlb(t->c.play)) { d->reset_sync_adjust = 1; return rate + user_tweak; }
    if (d->loop_on) { double ls = (d->loop_out - d->loop_in) / beat_len_frames(d); if (ls < 1.0 && ls > 0) { d->reset_sync_adjust = 1; return rate + user_tweak; } }
    if (d->reset_sync_adjust) { d->reset_sync_adjust = 0; d->last_sync_adjust = 1.0; }
    double error = shortest_pct_change(beat_distance_of(t, t->pos), beat_distance_of(d, d->pos) );
    double adjustment = 1.0;
    if (user_tweak != 0.0) {
        adjustment = 1.0;
        d->user_offset = fmod(error + d->user_offset, 1.0);
    } else {
        error = shortest_pct_change(beat_distance_of(t, t->pos) + d->user_offset, beat_distance_of(d, d->pos));
        const double kErrorThreshold = 0.01, kTrainWreck = 0.2, kCap = 0.05;
        if (fabs(error) > kTrainWreck) adjustment = 1.0 + kCap;
        else if (fabs(error) > kErrorThreshold) {
            const double kProp = 0.7, kDeltaCap = 0.02;
            double adjust = 1.0 + (-error * kProp);
            double delta = rx_clamp(adjust - d->last_sync_adjust, -kDeltaCap, kDeltaCap);
            adjustment = 1.0 + rx_clamp(d->last_sync_adjust - 1.0 + delta, -kCap, kCap);
        }
    }
    d->last_sync_adjust = adjustment;
    return (rate + user_tweak) * adjustment;
}

/* after Mixxx RateControl::calculateSpeed: the speed of this block. */
static double calculate_speed(rx_deck_t *d, int frames, int paused, int *report_scratching, int *report_reverse) {
    *report_scratching = 0; *report_reverse = 0;
    process_temp_rate(d);
    double speed = 1.0 + ctlv(d->c.rate_dir) * ctlv(d->c.rateRange) * ctlv(d->c.rate);
    double rate;
    double searching = ctlv(d->c.rateSearch);
    if (searching != 0) {
        rate = searching;
    } else {
        double wheel = ctlv(d->c.wheel);
        double jog = jog_factor(d);
        int use_scratch2 = ctlb(d->c.scratch2_enable);
        if (use_scratch2 && ctlb(d->c.scratch2_indicates_scratching)) *report_scratching = 1;
        double scratch = ctlv(d->c.scratch2); if (isnan(scratch)) scratch = 0;
        if (paused) {
            rate = use_scratch2 ? scratch + jog + wheel * WHEEL_MULT : jog * PAUSED_JOG_MULT + wheel;
        } else {
            if (use_scratch2) rate = scratch;
            else { rate = speed + d->temp_rate; if (rate < 0) rate = 0; rate += wheel; }
            rate += jog;
        }
        psc_process(&d->psc, ctlb(d->c.scratch_position_enable), ctlv(d->c.scratch_position), d->pos * 2, rate, frames * 2, 1.0,
                    d->wrap_count, d->wrap_trigger * 2, d->wrap_target * 2);
        d->wrap_count = 0;
        if (d->psc.is_scratching) {
            rate = d->psc.rate; *report_scratching = 1;
        } else {
            if (ctlb(d->c.sync_enabled) && !ctlb(d->c.sync_master) && !paused && !use_scratch2) {
                rx_deck_t *t = sync_target(d);
                if (t) rate = calc_synced_rate(d, t, *report_scratching ? 0.0 : d->temp_rate + wheel + jog);
            }
            if (ctlb(d->c.reverse) && !use_scratch2) { rate = -rate; *report_reverse = 1; }
        }
    }
    return rate;
}

/* ------------------------------------------------------------------------------------------------ loops & cues */
static void set_loop_enabled(rx_deck_t *d, int on) {
    if (on && !(d->loop_in >= 0 && d->loop_out > d->loop_in)) on = 0;
    d->loop_on = on;
    ctl_set(d->c.loop_enabled, on);
}
static void set_loop(rx_deck_t *d, double in, double out, int enable, int is_beatloop) {
    double n = frames_of(cur_track(d));
    if (in < 0) in = 0;
    if (out > n) out = n;
    if (out <= in) { d->loop_in = d->loop_out = -1; set_loop_enabled(d, 0); }
    else { d->loop_in = in; d->loop_out = out; d->loop_is_beatloop = is_beatloop; if (enable) set_loop_enabled(d, 1); }
    ctl_set(d->c.loop_start_position, d->loop_in >= 0 ? d->loop_in * 2 : -1);
    ctl_set(d->c.loop_end_position, d->loop_out >= 0 ? d->loop_out * 2 : -1);
    cues_dirty(d);
}
static void beatloop(rx_deck_t *d, double beats, int enable, int keep_start) {
    double bl = beat_len_frames(d);
    if (bl <= 0) {   /* no grid: Mixxx refuses; we loop a bar of 120 bpm so the button still does something */
        bl = 60.0 / 120.0 * RX_SR;
    }
    double start = (keep_start && d->loop_in >= 0) ? d->loop_in : (ctlb(d->c.quantize) ? prev_beat(d, d->pos) : d->pos);
    double len = beats * bl;
    double n = frames_of(cur_track(d));
    if (start + len > n) start = n - len; if (start < 0) start = 0;
    set_loop(d, start, start + len, enable, 1);
    /* LoopingControl: if the position is now outside the loop, seek into it */
    if (d->loop_on && (d->pos < d->loop_in || d->pos >= d->loop_out)) seek(d, d->loop_in);
}
static void set_beatloop_size(rx_deck_t *d, double s) {
    if (s < RX_LOOP_SIZES[0]) s = RX_LOOP_SIZES[0]; if (s > 64) s = 64;
    d->beatloop_size = s;
    ctl_set(d->c.beatloop_size, s);
    int best = 0; for (int i = 0; i < N_LOOP_SIZES; i++) if (fabs(RX_LOOP_SIZES[i] - s) < fabs(RX_LOOP_SIZES[best] - s)) best = i;
    ctl_set(d->c.beatloop_size_idx, best);
    /* an active beatloop follows the size (LoopingControl::slotBeatLoopSizeChanged) */
    if (d->loop_on && d->loop_is_beatloop) beatloop(d, s, 1, 1);
}
static void set_beatjump_size(rx_deck_t *d, double s) {
    if (s < RX_LOOP_SIZES[0]) s = RX_LOOP_SIZES[0]; if (s > 64) s = 64;
    d->beatjump_size = s;
    ctl_set(d->c.beatjump_size, s);
    int best = 0; for (int i = 0; i < N_LOOP_SIZES; i++) if (fabs(RX_LOOP_SIZES[i] - s) < fabs(RX_LOOP_SIZES[best] - s)) best = i;
    ctl_set(d->c.beatjump_size_idx, best);
}
static void loop_move(rx_deck_t *d, double beats) {
    double bl = beat_len_frames(d); if (bl <= 0 || d->loop_in < 0) return;
    double len = d->loop_out - d->loop_in, in = d->loop_in + beats * bl, n = frames_of(cur_track(d));
    if (in < 0) in = 0; if (in + len > n) in = n - len;
    set_loop(d, in, in + len, d->loop_on, d->loop_is_beatloop);
    if (d->loop_on) seek(d, d->pos + beats * bl);
}
static void beatjump(rx_deck_t *d, double beats) {
    double bl = beat_len_frames(d); if (bl <= 0) bl = 60.0 / 120.0 * RX_SR;
    /* LoopingControl::slotBeatJump: inside an active loop the loop moves, else the position jumps */
    if (d->loop_on && d->pos >= d->loop_in && d->pos < d->loop_out) { loop_move(d, beats); return; }
    double target = d->pos + beats * bl;
    if (ctlb(d->c.quantize)) target = nearest_beat(d, target);
    seek(d, target);
}

static void hotcue_release(rx_deck_t *d, int i) {
    if (d->previewing == i) {           /* release of a previewing hotcue: stop, back to it */
        d->previewing = -1;
        stop_deck(d);
        if (d->hotcue[i] >= 0) seek(d, d->hotcue[i]);
    }
}
static void hotcue_press(rx_deck_t *d, int i) {
    if (ctlb(d->c.hotcue_clear_mode)) {     /* ours: the next activate clears */
        d->hotcue[i] = -1; ctl_set(d->c.hotcue_clear_mode, 0); cues_dirty(d); return;
    }
    if (d->hotcue[i] >= 0) {
        if (ctlb(d->c.play) && d->previewing < 0) {        /* playing by Play: hotcueGoto */
            seek(d, ctlb(d->c.quantize) ? nearest_beat(d, d->hotcue[i]) : d->hotcue[i]);
        } else {                                           /* paused or previewing: hotcueActivatePreview */
            if (d->previewing != i) {
                d->previewing = i;
                seek(d, d->hotcue[i]);
                ctl_set(d->c.play, 1);
            }
        }
    } else {                                                /* hotcueSet */
        d->hotcue[i] = quantized(d, d->pos);
        if (d->hotcue[i] < 0) d->hotcue[i] = 0;
        cues_dirty(d);
    }
}
/* after Mixxx CueControl::cueCDJ (the "Mixxx" cue mode) */
static void cue_press(rx_deck_t *d) {
    int freely_playing = ctlb(d->c.play) && !d->scratching;
    double n = frames_of(cur_track(d));
    int at_end = d->pos >= n - 1;
    if (d->cue < 0) d->cue = 0;
    int at_cue = fabs(d->pos - d->cue) < 1.0;
    if (d->previewing == RX_HOTCUES) return;                /* already previewing */
    if (d->previewing >= 0) {                               /* previewing a hotcue: jump to cue, keep previewing */
        d->previewing = RX_HOTCUES; seek(d, d->cue);
    } else if (freely_playing || at_end) {
        stop_deck(d); seek(d, d->cue);
    } else if (at_cue) {
        d->previewing = RX_HOTCUES; ctl_set(d->c.play, 1);
    } else {                                                /* paused, not at cue: set the cue here */
        d->cue = quantized(d, d->pos); cues_dirty(d);
        if (ctlb(d->c.quantize)) seek(d, d->cue);
    }
}
static void cue_release(rx_deck_t *d) {
    if (d->previewing == RX_HOTCUES) { d->previewing = -1; stop_deck(d); seek(d, d->cue); }
}

/* a hold-capable trigger (cue_default, hotcue_N_activate): press on 0->1, release on 1->0; a press that never gets
 * released (the MPC's parameter buttons only send 1) is released by itself after HOLD_AUTORELEASE_BLOCKS */
static int hold_edge(ctl_id id, int *prev, int *held, int *blocks, int *release) {
    int v = ctl_get(id) >= 0.5;
    *release = 0;
    int press = 0;
    if (v && !*held) { *held = 1; *blocks = 0; press = 1; }
    else if (!v && *held) { *held = 0; *release = 1; }
    else if (v && *held && ++*blocks > HOLD_AUTORELEASE_BLOCKS) { *held = 0; *release = 1; ctl_set(id, 0); }
    *prev = v;
    return press;
}

/* ------------------------------------------------------------------------------------------------ tick controls */
static void update_texts(rx_deck_t *d, track_t *t) {
    char s[96];
    double bpm = d->bpm * fabs(ctlv(d->c.rate_ratio));
    if (d->bpm > 0) snprintf(s, sizeof s, "%.1f", bpm); else snprintf(s, sizeof s, "-");
    set_text_if(d->c.bpm_text, d->t_bpm, sizeof d->t_bpm, s);
    double sec = d->pos / RX_SR, dur = frames_of(t) / RX_SR;
    snprintf(s, sizeof s, "%d:%02d/%d:%02d", (int)sec / 60, (int)sec % 60, (int)dur / 60, (int)dur % 60);
    set_text_if(d->c.time_text, d->t_time, sizeof d->t_time, s);
    const char *title = (t && t->au && t->au->title[0]) ? t->au->title : (t ? strrchr(t->path, '/') ? strrchr(t->path, '/') + 1 : t->path : "-");
    set_text_if(d->c.title_text, d->t_title, sizeof d->t_title, title);
    static const char *const KEYS[] = {"-", "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B",
                                       "Cm", "Dbm", "Dm", "Ebm", "Em", "Fm", "Gbm", "Gm", "Abm", "Am", "Bbm", "Bm"};
    int k = (int)d->key; if (k < 0 || k > 24) k = 0;
    set_text_if(d->c.key_text, d->t_key, sizeof d->t_key, KEYS[k]);
    snprintf(s, sizeof s, "%+.1f%%", (ctlv(d->c.rate_ratio) - 1.0) * 100.0);
    set_text_if(d->c.rate_text, d->t_rate, sizeof d->t_rate, s);
}

void deck_tick_controls(rx_deck_t *d) {
    deck_ctls_t *c = &d->c;
    d->blocks++;
    adopt_pending(d);
    apply_import(d);
    track_t *t = cur_track(d);

    /* ---- rate range: keep rateRange and rateRange_idx consistent (whichever moved last wins) */
    {
        static const double eps = 1e-6;
        int idx = (int)ctlv(c->rateRange_idx); if (idx < 0) idx = 0; if (idx > 3) idx = 3;
        if (take(c->rateRange_next)) { idx = (idx + 1) % 4; ctl_set(c->rateRange_idx, idx); ctl_set(c->rateRange, RX_RATE_RANGE_VALUES[idx]); }
        double rr = ctlv(c->rateRange);
        static double last_rr[RX_DECKS]; static int last_idx[RX_DECKS];
        if (d->index >= 0 && d->index < RX_DECKS) {
            if (fabs(rr - last_rr[d->index]) > eps && idx == last_idx[d->index]) {     /* rateRange moved: find the index */
                int best = 0; for (int i = 0; i < 4; i++) if (fabs(RX_RATE_RANGE_VALUES[i] - rr) < fabs(RX_RATE_RANGE_VALUES[best] - rr)) best = i;
                ctl_set(c->rateRange_idx, best); idx = best;
            } else if (idx != last_idx[d->index]) { ctl_set(c->rateRange, RX_RATE_RANGE_VALUES[idx]); rr = RX_RATE_RANGE_VALUES[idx]; }
            last_rr[d->index] = rr; last_idx[d->index] = idx;
        }
        /* RateControl::slotControlRatePerm*: step the slider; rate_set_default: centre */
        double rate = ctlv(c->rate), dir = ctlv(c->rate_dir);
        if (take(c->rate_perm_up)) rate += dir * PERM_COARSE / (100.0 * rr);
        if (take(c->rate_perm_down)) rate -= dir * PERM_COARSE / (100.0 * rr);
        if (take(c->rate_perm_up_small)) rate += dir * PERM_FINE / (100.0 * rr);
        if (take(c->rate_perm_down_small)) rate -= dir * PERM_FINE / (100.0 * rr);
        if (take(c->rate_set_default)) rate = 0;
        rate = rx_clamp(rate, -1, 1);
        if (rate != ctlv(c->rate)) ctl_set(c->rate, rate);
        ctl_set(c->rate_ratio, 1.0 + dir * rr * rate);
    }

    /* ---- eject / track step (engine.c loads) */
    if (take(c->eject)) {
        track_t *old = __atomic_exchange_n(&d->cur, NULL, __ATOMIC_ACQ_REL);
        if (old) { retire_track(d, old); reset_for_track(d, NULL); }
        t = NULL;
    }
    if (take(c->track_prev) && c->track_step != CTL_NONE) ctl_set(c->track_step, -1);
    if (take(c->track_next) && c->track_step != CTL_NONE) ctl_set(c->track_step, 1);

    if (!t) {   /* nothing loaded: swallow the triggers so they do not fire on the next track */
        for (int i = 0; i < RX_HOTCUES; i++) { take(c->hc_activate[i]); take(c->hc_set[i]); take(c->hc_clear[i]); take(c->hc_goto[i]); take(c->hc_gotoandplay[i]); take(c->hc_gotoandstop[i]); }
        take(c->cue_default); take(c->cue_set); take(c->cue_gotoandplay); take(c->cue_gotoandstop); take(c->loop_in); take(c->loop_out);
        take(c->reloop_toggle); take(c->loop_halve); take(c->loop_double); take(c->beatjump_forward); take(c->beatjump_backward);
        take(c->beatsync); take(c->beatsync_phase); take(c->beatsync_tempo); take(c->key_sync); take(c->back_tap); take(c->fwd_tap);
        take(c->start); take(c->end); take(c->start_play); take(c->start_stop); take(c->play_stutter); take(c->loop_exit); take(c->reloop_andstop);
        for (int i = 0; i < N_LOOP_SIZES; i++) { take(c->bl_activate[i]); take(c->bl_toggle[i]); take(c->bj_fwd[i]); take(c->bj_back[i]); }
        if (ctlb(c->play)) ctl_set(c->play, 0);
        ctl_set(c->playposition, 0);
        if (d->blocks % TEXT_EVERY == 0) {
            set_text_if(c->title_text, d->t_title, sizeof d->t_title, "-"); set_text_if(c->bpm_text, d->t_bpm, sizeof d->t_bpm, "-");
            set_text_if(c->time_text, d->t_time, sizeof d->t_time, "0:00"); set_text_if(c->key_text, d->t_key, sizeof d->t_key, "-");
            char s[16]; snprintf(s, sizeof s, "%+.1f%%", (ctlv(c->rate_ratio) - 1.0) * 100.0); set_text_if(c->rate_text, d->t_rate, sizeof d->t_rate, s);
        }
        return;
    }
    double n = frames_of(t);

    /* ---- the transport */
    if (take(c->start)) seek(d, 0);
    {   /* the companion page: tap a spot. Playing: jump there and carry on. Stopped: cue from there (the cue point moves too, like a CDJ) */
        double ws = ctlv(c->web_seek);
        if (ws >= 0) {
            ctl_set(c->web_seek, -1);
            double fr = ws * RX_SR; if (fr > n - 1) fr = n - 1;
            if (!ctlb(c->play)) { d->cue = quantized(d, fr); cues_dirty(d); fr = d->cue; }
            seek(d, ctlb(c->quantize) ? nearest_beat(d, fr) : fr);
        }
    }
    if (take(c->end)) seek(d, n - 1);
    if (take(c->start_play)) { seek(d, 0); ctl_set(c->play, 1); }
    if (take(c->start_stop)) { seek(d, 0); stop_deck(d); }
    if (take(c->play_stutter)) { seek(d, d->cue >= 0 ? d->cue : 0); ctl_set(c->play, 1); }
    if (take(c->cue_set)) { d->cue = quantized(d, d->pos); cues_dirty(d); }
    if (take(c->cue_gotoandstop)) { stop_deck(d); seek(d, d->cue >= 0 ? d->cue : 0); }
    if (take(c->cue_gotoandplay)) { seek(d, d->cue >= 0 ? d->cue : 0); ctl_set(c->play, 1); }
    {
        int rel; int press = hold_edge(c->cue_default, &d->cue_prev_val, &d->cue_held, &d->cue_hold_blocks, &rel);
        if (press) cue_press(d);
        if (rel) cue_release(d);
        /* Mixxx "cue_preview" is a toggle-style hold */
        int cp = ctlb(c->cue_preview);
        if (cp && !d->cue_preview_prev) { d->previewing = RX_HOTCUES; seek(d, d->cue >= 0 ? d->cue : 0); ctl_set(c->play, 1); }
        else if (!cp && d->cue_preview_prev) cue_release(d);
        d->cue_preview_prev = cp;
    }
    {   /* Mixxx CueControl::updateIndicatorsAndModifyPlay: while previewing, "play" was set by the cue (or a hot cue) button, so a PLAY press
         * (the toggle going 1 -> 0) is the LATCH: stop previewing and keep playing. Hold CUE, press PLAY, let go of CUE: it plays on */
        int pl = ctlb(c->play);
        if (d->previewing >= 0 && !pl && d->play_prev && d->pos < n - 2) { d->previewing = -1; ctl_set(c->play, 1); pl = 1; }
        d->play_prev = pl;
    }
    ctl_set(c->cue_point, d->cue >= 0 ? d->cue * 2 : -1);
    ctl_set(c->cue_indicator, (!ctlb(c->play)) ? 1 : 0);

    /* ---- hot cues */
    for (int i = 0; i < RX_HOTCUES; i++) {
        int rel; int press = hold_edge(c->hc_activate[i], &d->hc_prev_val[i], &d->hc_held[i], &d->hc_hold_blocks[i], &rel);
        if (press) hotcue_press(d, i);
        if (rel) hotcue_release(d, i);
        if (take(c->hc_set[i])) { d->hotcue[i] = quantized(d, d->pos); cues_dirty(d); }
        if (take(c->hc_clear[i])) { d->hotcue[i] = -1; cues_dirty(d); }
        if (take(c->hc_goto[i]) && d->hotcue[i] >= 0) seek(d, d->hotcue[i]);
        if (take(c->hc_gotoandplay[i]) && d->hotcue[i] >= 0) { seek(d, d->hotcue[i]); ctl_set(c->play, 1); }
        if (take(c->hc_gotoandstop[i]) && d->hotcue[i] >= 0) { seek(d, d->hotcue[i]); stop_deck(d); }
        int col = (int)ctlv(c->hc_color[i]) & 7;
        if (col != d->hotcue_colour[i]) { d->hotcue_colour[i] = col; cues_dirty(d); }
        ctl_set(c->hc_position[i], d->hotcue[i] >= 0 ? d->hotcue[i] * 2 : -1);
        ctl_set(c->hc_enabled[i], d->hotcue[i] >= 0 ? 1 : 0);
    }

    /* ---- loops */
    {
        double bls = ctlv(c->beatloop_size);
        int idx = (int)ctlv(c->beatloop_size_idx); if (idx < 0) idx = 0; if (idx >= N_LOOP_SIZES) idx = N_LOOP_SIZES - 1;
        static double last_bls[RX_DECKS]; static int last_bidx[RX_DECKS];
        if (d->index >= 0 && d->index < RX_DECKS) {
            if (idx != last_bidx[d->index]) set_beatloop_size(d, RX_LOOP_SIZES[idx]);
            else if (fabs(bls - last_bls[d->index]) > 1e-9) set_beatloop_size(d, bls);
            last_bls[d->index] = ctlv(c->beatloop_size); last_bidx[d->index] = (int)ctlv(c->beatloop_size_idx);
        }
        double bjs = ctlv(c->beatjump_size);
        int jidx = (int)ctlv(c->beatjump_size_idx); if (jidx < 0) jidx = 0; if (jidx >= N_LOOP_SIZES) jidx = N_LOOP_SIZES - 1;
        static double last_bjs[RX_DECKS]; static int last_jidx[RX_DECKS];
        if (d->index >= 0 && d->index < RX_DECKS) {
            if (jidx != last_jidx[d->index]) set_beatjump_size(d, RX_LOOP_SIZES[jidx]);
            else if (fabs(bjs - last_bjs[d->index]) > 1e-9) set_beatjump_size(d, bjs);
            last_bjs[d->index] = ctlv(c->beatjump_size); last_jidx[d->index] = (int)ctlv(c->beatjump_size_idx);
        }
    }
    if (take(c->loop_halve)) set_beatloop_size(d, d->beatloop_size / 2);
    if (take(c->loop_double)) set_beatloop_size(d, d->beatloop_size * 2);
    if (take(c->loop_in)) {           /* LoopingControl::slotLoopIn (press): the in point at the (quantized) position */
        double in = quantized(d, d->pos);
        double out = (d->loop_out > in) ? d->loop_out : -1;
        set_loop(d, in, out, d->loop_on && out > 0, 0);
    }
    if (take(c->loop_out)) {          /* slotLoopOut: the out point; with an in point before it, looping starts */
        double out = quantized(d, d->pos);
        if (d->loop_in >= 0 && out > d->loop_in) set_loop(d, d->loop_in, out, 1, 0);
        else if (d->loop_in < 0) set_loop(d, 0, out, 0, 0);
    }
    if (take(c->loop_exit)) set_loop_enabled(d, 0);
    if (take(c->reloop_toggle)) {
        if (d->loop_on) { if (d->roll_held) { ctl_set(c->slip_enabled, 0); d->roll_held = 0; } set_loop_enabled(d, 0); }
        else if (d->loop_in >= 0 && d->loop_out > d->loop_in) { set_loop_enabled(d, 1); if (d->pos > d->loop_out) seek(d, d->loop_in); }
    }
    if (take(c->reloop_andstop)) { if (d->loop_in >= 0) { seek(d, d->loop_in); set_loop_enabled(d, 1); } stop_deck(d); }
    for (int i = 0; i < N_LOOP_SIZES; i++) {
        if (take(c->bl_activate[i])) { set_beatloop_size(d, RX_LOOP_SIZES[i]); beatloop(d, RX_LOOP_SIZES[i], 1, 0); }
        if (take(c->bl_toggle[i])) {
            if (d->loop_on && d->loop_is_beatloop && fabs(d->beatloop_size - RX_LOOP_SIZES[i]) < 1e-9) set_loop_enabled(d, 0);
            else { set_beatloop_size(d, RX_LOOP_SIZES[i]); beatloop(d, RX_LOOP_SIZES[i], 1, 0); }
        }
        int roll = ctlb(c->blr_activate[i]);     /* beatlooproll: slip + loop while held */
        static int roll_prev[RX_DECKS][N_LOOP_SIZES];
        if (d->index >= 0 && d->index < RX_DECKS) {
            if (roll && !roll_prev[d->index][i]) { if (!d->roll_held) { d->roll_held = 1; ctl_set(c->slip_enabled, 1); } beatloop(d, RX_LOOP_SIZES[i], 1, 0); }
            else if (!roll && roll_prev[d->index][i] && d->roll_held) {
                int any = 0; for (int k = 0; k < N_LOOP_SIZES; k++) any |= ctlb(c->blr_activate[k]);
                if (!any) { d->roll_held = 0; set_loop_enabled(d, 0); ctl_set(c->slip_enabled, 0); }
            }
            roll_prev[d->index][i] = roll;
        }
        if (take(c->bj_fwd[i])) beatjump(d, RX_LOOP_SIZES[i]);
        if (take(c->bj_back[i])) beatjump(d, -RX_LOOP_SIZES[i]);
    }
    if (take(c->beatjump_forward)) beatjump(d, d->beatjump_size);
    if (take(c->beatjump_backward)) beatjump(d, -d->beatjump_size);
    { double lm = ctlv(c->loop_move); if (lm != 0) { ctl_set(c->loop_move, 0); loop_move(d, lm); } }

    /* ---- slip (EngineBuffer::processSlip's enable/disable edge; the position runs in deck_render) */
    {
        int slip = ctlb(c->slip_enabled);
        if (slip != d->slip_on) {
            d->slip_on = slip;
            if (slip) { d->slip_pos = d->pos; d->slip_rate = d->rate_old; d->loop_on_before_slip = d->loop_on; }
            else {
                if (!d->loop_on_before_slip && d->loop_on) set_loop_enabled(d, 0);   /* loops made during slip end with it */
                seek(d, d->slip_pos);
            }
        }
    }

    /* ---- reverse roll (reverse + slip while held) */
    {
        static int rr_prev[RX_DECKS];
        int rr = ctlb(c->reverseroll);
        if (d->index >= 0 && d->index < RX_DECKS) {
            if (rr && !rr_prev[d->index]) { ctl_set(c->slip_enabled, 1); ctl_set(c->reverse, 1); }
            else if (!rr && rr_prev[d->index]) { ctl_set(c->reverse, 0); ctl_set(c->slip_enabled, 0); }
            rr_prev[d->index] = rr;
        }
    }

    /* ---- search: back/fwd held = +-4x (RateControl::slotControlFastForward), taps = one beat */
    {
        int b = ctlb(c->back), f = ctlb(c->fwd);
        ctl_set(c->rateSearch, f ? RATE_SEARCH : b ? -RATE_SEARCH : 0.0);
        if (take(c->back_tap)) beatjump(d, -1);
        if (take(c->fwd_tap)) beatjump(d, 1);
    }

    /* ---- sync */
    if (take(c->beatsync) || take(c->beatsync_tempo) || take(c->beatsync_phase)) {
        /* (all three: tempo + phase; Mixxx's beatsync does both) */
        rx_deck_t *tg = sync_target(d);
        if (tg && d->bpm > 0 && tg->bpm > 0) {
            double target_bpm = tg->bpm * fabs(tg->rate_now > 0 ? tg->rate_now : ctlv(tg->c.rate_ratio));
            double ratio = target_bpm / d->bpm;
            double rr = ctlv(c->rateRange);
            /* Mixxx halves/doubles when the ratio is far off */
            if (ratio > 1.5) ratio /= 2; else if (ratio < 0.66) ratio *= 2;
            ctl_set(c->rate, rx_clamp((ratio - 1.0) / rr * ctlv(c->rate_dir), -1, 1));
            ctl_set(c->rate_ratio, 1.0 + ctlv(c->rate_dir) * rr * ctlv(c->rate));
            /* phase: move to the same beat fraction as the target (BpmControl::syncPhase) */
            if (ctlb(c->play) && ctlb(tg->c.play)) {
                double bl = beat_len_frames(d);
                double want = beat_distance_of(tg, tg->pos), have = beat_distance_of(d, d->pos);
                double delta = shortest_pct_change(have, want) * bl;
                double np = d->pos + delta;
                if (d->loop_on && (np < d->loop_in || np >= d->loop_out)) { /* keep it inside the loop */ np = d->loop_in + fmod(np - d->loop_in + (d->loop_out - d->loop_in), d->loop_out - d->loop_in); }
                seek(d, np);
                d->user_offset = 0;
            }
        }
    }
    ctl_set(c->sync_mode, ctlb(c->sync_master) ? 2 : ctlb(c->sync_enabled) ? 1 : 0);

    /* ---- key sync (ours): pitch = semitone difference to the other deck's key */
    if (take(c->key_sync)) {
        rx_deck_t *tg = sync_target(d);
        if (!tg) for (int i = 0; i < RX_DECKS; i++) if (g_decks[i] && g_decks[i] != d && g_decks[i]->key > 0) { tg = g_decks[i]; break; }
        if (tg && d->key > 0 && tg->key > 0) {
            int mine = ((int)d->key - 1) % 12, theirs = ((int)tg->key - 1) % 12;
            int diff = ((theirs - mine + 6) % 12 + 12) % 12 - 6;
            ctl_set(c->pitch, diff);
        }
    }
    { double k = ctlv(c->key); if (k != d->key) d->key = k; }

    /* ---- readbacks */
    ctl_set(c->playposition, n > 0 ? d->pos / n : 0);
    ctl_set(c->bpm, d->bpm * fabs(ctlv(c->rate_ratio)));
    {   /* the jog centre's number: the live BPM rounded, three digit pictures (no leading zero); none loaded = blank */
        int b = (int)lround(d->bpm * fabs(ctlv(c->rate_ratio))); if (b > 999) b = 999; if (b < 0) b = 0;
        double h = d->bpm > 0 && b >= 100 ? (b / 100) / 10.0 : 1.0, t = d->bpm > 0 && b >= 10 ? ((b / 10) % 10) / 10.0 : 1.0, u = d->bpm > 0 ? (b % 10) / 10.0 : 1.0;
        if (h != ctlv(c->bpm_h)) ctl_set(c->bpm_h, h);
        if (t != ctlv(c->bpm_t)) ctl_set(c->bpm_t, t);
        if (u != ctlv(c->bpm_u)) ctl_set(c->bpm_u, u);
    }
    ctl_set(c->beat_distance, beat_distance_of(d, d->pos));
    {
        double bl = beat_len_frames(d);
        int active = bl > 0 && beat_distance_of(d, d->pos) * bl < RX_BLOCK * 8;    /* ~23 ms after each beat */
        ctl_set(c->beat_active, active && ctlb(c->play) ? 1 : 0);
    }
    if (d->blocks % TEXT_EVERY == 0) update_texts(d, t);
}

/* ------------------------------------------------------------------------------------------------------- render */
/* one frame of the track with linear interpolation (Mixxx EngineBufferScaleLinear's kernel) */
static inline void read_frame(const int16_t *lr, int64_t n, double p, float *l, float *r) {
    if (p < 0) p = 0;
    int64_t i0 = (int64_t)p;
    if (i0 >= n - 1) { i0 = n - 1; *l = lr[2 * i0] * (1.0f / 32768); *r = lr[2 * i0 + 1] * (1.0f / 32768); return; }
    float f = (float)(p - (double)i0);
    const int16_t *a = lr + 2 * i0;
    *l = (a[0] + f * (a[2] - a[0])) * (1.0f / 32768);
    *r = (a[1] + f * (a[3] - a[1])) * (1.0f / 32768);
}

/* advance the play position by `step` frames, wrapping a loop (LoopingControl::nextTrigger/process) or stopping at
 * the track's ends; returns 0 when the deck must stop */
static inline int advance(rx_deck_t *d, double step, double n) {
    double prev = d->pos, np = prev + step;
    if (d->loop_on) {
        double len = d->loop_out - d->loop_in;
        if (step > 0 && prev < d->loop_out && np >= d->loop_out) { np -= len; d->wrap_count++; d->wrap_trigger = d->loop_out; d->wrap_target = d->loop_in; }
        else if (step < 0 && prev >= d->loop_in && np < d->loop_in) { np += len; d->wrap_count++; d->wrap_trigger = d->loop_in; d->wrap_target = d->loop_out; }
    }
    if (np >= n - 1) { d->pos = n - 1; return 0; }
    if (np < 0) { d->pos = 0; return 0; }
    d->pos = np;
    return 1;
}

/* the jog wheel's touch dial and its ring picture: run every block, stopped or playing */
static void jog_block(rx_deck_t *d, int frames) {
    {   /* queued by touch.c: a finger on the wheel of a deck that is stopped moves the playhead (VINYL and CDJ alike); while playing it is dropped */
        int64_t q = __atomic_exchange_n(&d->ext_seek_fp, 0, __ATOMIC_ACQ_REL);
        if (q != 0 && ctlv(d->c.play) < 0.5) {
            seek(d, d->pos + (double)q / 16.0);
            d->platter += (double)q / 16.0 / (1.8 * RX_SR); d->platter -= floor(d->platter);
        }
    }
    /* the touch dial round the jog ring is endless: a finger's move on it is the wheel turning
         * wheel turning (clockwise = left up, right down, top right, bottom left): VINYL scratches the track, CDJ nudges the pitch (scaled by JOG
         * ADJUST); each strip is put back to the middle so it never hits an end. The ring's picture follows the platter. */
    static const double sgn[4] = { -1, 1, -1, 1 };
        double feel = 0.25 + ctlv(d->c.jog_feel) * 1.5, dsum = 0;
        if (!d->jp_init) { for (int k = 0; k < 4; k++) d->jp[k] = 0.5; d->jp_init = 1; }
        int ext = __atomic_load_n(&d->ext_jog, __ATOMIC_ACQUIRE);
        for (int k = 0; k < 4; k++) {
            double v = ctlv(d->c.jog_k[k]);
            if (fabs(v - d->jp[k]) > 1e-6) { if (!ext) dsum += sgn[k] * (v - d->jp[k]); d->jp[k] = v; }
            if (fabs(v - 0.5) > 0.3) { ctl_set(d->c.jog_k[k], 0.5); d->jp[k] = 0.5; }
        }
        if (dsum != 0) {
            d->jog_idle = 0;
            if (ctlv(d->c.play) < 0.5 && !d->jog_scr) {          /* stopped: the wheel moves the playhead itself (VINYL and CDJ alike), a turn = 1.8 s of the track (33 1/3 rpm) */
                seek(d, d->pos + dsum * feel * 1.8 * RX_SR);
                d->platter += dsum * feel; d->platter -= floor(d->platter);
            } else if (ctlv(d->c.vinyl_mode) > 0.5) {
                if (!d->jog_scr) { deck_scratch_enable(d, 1, 33.0 + 1.0 / 3.0, 1.0 / 8, 1.0 / 256); d->jog_scr = 1; }
                deck_scratch_tick(d, dsum * 720.0 * feel);
            } else deck_jog(d, dsum * 12.6 * feel);
        } else d->jog_idle += (double)frames / RX_SR;
        if (d->jog_idle > 0.15) {
            if (d->jog_scr && !ext) { deck_scratch_enable(d, 0, 0, 0, 0); d->jog_scr = 0; }
            for (int k = 0; k < 4; k++) if (fabs(ctlv(d->c.jog_k[k]) - 0.5) > 1e-6) { ctl_set(d->c.jog_k[k], 0.5); d->jp[k] = 0.5; }
        }
        double ph = d->platter * 24.0 * RX_JOG_SPIN; ph -= floor(ph);
        int jf = (int)(ph * RX_JOG_FRAMES) % RX_JOG_FRAMES; double jv = (RX_JOG_FRAMES - 1 - jf) / (double)(RX_JOG_FRAMES - 1);      /* the strip's frames run the other way round: forward turns the ring clockwise */
        if (jv != ctlv(d->c.jog_disp)) ctl_set(d->c.jog_disp, jv);
}

void deck_render(rx_deck_t *d, float *out, int frames) {
    track_t *t = cur_track(d);
    deck_ctls_t *c = &d->c;
    if (!t || !t->au || frames <= 0) { memset(out, 0, sizeof(float) * 2 * (size_t)frames); d->rate_old = d->rate_now = 0; d->vu_l = d->vu_r = 0; d->seek_pending = 0; return; }
    const int16_t *lr = t->au->lr;
    const double n = (double)t->au->frames;
    /* the block renders from where the last one ended; a seek requested since lands after it (see seek()) */
    double seek_target = d->seek_target; int seek_pending = d->seek_pending;
    if (seek_pending) d->pos = d->render_pos;
    int playing = ctlb(c->play);
    scratch_block(d, frames);
    int scratching, reversing;
    double rate = calculate_speed(d, frames, !playing, &scratching, &reversing);
    if (isnan(rate) || isinf(rate)) rate = 0;
    if (fabs(rate) > 100) rate = rate > 0 ? 100 : -100;         /* PositionScratchController's kMaxVelocity */
    d->scratching = scratching; d->reverse_now = rate < 0;
    if (!playing && fabs(rate) < MIN_SEEK_SPEED && !scratching) rate = 0;      /* stopped: nothing moves */
    d->rate_now = rate;
    double rate_old = d->rate_old;
    if (rate_old * rate < 0 && !scratching) rate_old = 0;                        /* the scaler cannot ramp through a direction change */

    /* slip: the virtual position keeps running at the rate it had when slip started (EngineBuffer::processSlip) */
    if (d->slip_on) {
        double sd = frames * d->slip_rate;
        if (d->loop_on_before_slip && d->loop_in >= 0 && d->loop_out > d->loop_in) {
            double len = d->loop_out - d->loop_in, np = d->slip_pos + sd;
            while (np >= d->loop_out) np -= len; while (np < d->loop_in) np += len;
            d->slip_pos = np;
        } else d->slip_pos = rx_clamp(d->slip_pos + sd, 0, n - 1);
    }

    if (rate == 0 && rate_old == 0) {
        memset(out, 0, sizeof(float) * 2 * (size_t)frames);
        d->ola_active = 0;
        d->rate_old = 0;
        jog_block(d, frames);
        apply_seek(d); d->render_pos = d->pos;
        if (++d->silent_blocks > 4) { d->vu_l = d->vu_r = 0; }
        return;
    }
    d->silent_blocks = 0;

    /* keylock: tempo without pitch (our OLA), unless scratching or at 1x with no pitch offset (Mixxx's rule) */
    double pitch_st = ctlv(c->pitch) + ctlv(c->pitch_adjust);
    int keylock = ctlb(c->keylock) && !scratching && (fabs(fabs(rate) - 1.0) > 1e-4 || fabs(pitch_st) > 1e-4) && fabs(rate) > MIN_SEEK_SPEED;
    const double dr = (rate - rate_old) / frames;
    double r = rate_old;
    int stopped = 0;

    if (!keylock) {
        /* after Mixxx EngineBufferScaleLinear: rate ramps linearly over the block, linear interpolation */
        d->ola_active = 0;
        for (int i = 0; i < frames; i++) {
            r += dr;
            read_frame(lr, t->au->frames, d->pos, &out[2 * i], &out[2 * i + 1]);
            if (!advance(d, r, n)) { stopped = 1; for (int k = i + 1; k < frames; k++) out[2 * k] = out[2 * k + 1] = 0; break; }
        }
    } else {
        /* OLA time-stretch (ours; "WSOLA without the similarity search", quality second to cost): 40 ms Hann grains
         * every 20 ms, each read from the track at the pitch ratio (2^(semitones/12), signed by direction) while the
         * grain start follows the play position, which advances at the tempo. */
        double pitch_ratio = pow(2.0, pitch_st / 12.0);
        double step = (rate < 0 ? -1 : 1) * pitch_ratio;
        if (!d->ola_active) { d->ola_active = 1; d->ola_phase = 0; d->grain[0].age = OLA_GRAIN; d->grain[1].age = OLA_GRAIN; }
        for (int i = 0; i < frames; i++) {
            r += dr;
            if (d->ola_phase == 0) {                     /* spawn a grain at the play position */
                int g = d->grain[0].age >= d->grain[1].age ? 0 : 1;
                d->grain[g].rpos = d->pos; d->grain[g].age = 0;
            }
            d->ola_phase = (d->ola_phase + 1) % OLA_HOP;
            float L = 0, R = 0;
            for (int g = 0; g < 2; g++) {
                if (d->grain[g].age >= OLA_GRAIN) continue;
                float gl, gr; read_frame(lr, t->au->frames, d->grain[g].rpos, &gl, &gr);
                float w = d->win[d->grain[g].age];
                L += gl * w; R += gr * w;
                d->grain[g].age++;
                double p = d->grain[g].rpos + step;
                if (d->loop_on) { if (p >= d->loop_out) p -= d->loop_out - d->loop_in; else if (p < d->loop_in) p += d->loop_out - d->loop_in; }
                if (p < 0) p = 0; if (p > n - 1) p = n - 1;
                d->grain[g].rpos = p;
            }
            out[2 * i] = L; out[2 * i + 1] = R;
            if (!advance(d, r, n)) { stopped = 1; for (int k = i + 1; k < frames; k++) out[2 * k] = out[2 * k + 1] = 0; break; }
        }
    }
    if (stopped && playing && !d->loop_on) stop_deck(d);       /* Mixxx: the deck stops at the end */
    if (seek_pending) { d->seek_target = seek_target; apply_seek(d); }
    d->render_pos = d->pos;

    /* gain: replay gain (analysis) x vinyl speed gain (after Mixxx EnginePregain: -inf dB at 0x, 0 dB at 1x, never
     * above 1 here since the pregain knob lives in the mixer), ramped over the block; through zero on a direction
     * change while scratching */
    float rg = (t->an && t->an->gain > 0) ? rx_clampf(t->an->gain, 0, 10) : 1.0f;
    float speed_gain = (float)(log10(fabs(rate) * 4.0 + 1.0) / log10(5.0)); if (speed_gain > 1) speed_gain = 1;
    float gain = rg * speed_gain;
    if (d->rate_old * rate < 0 && scratching) {
        rx_apply_ramp(out, frames / 2, d->gain_prev, 0);
        rx_apply_ramp(out + 2 * (frames / 2), frames - frames / 2, 0, gain);
    } else rx_apply_ramp(out, frames, d->gain_prev, gain);
    d->gain_prev = gain;
    d->rate_old = rate;

    /* platter (0.1.19): it does not turn with the playing track, as on a CDJ; it turns only when it is scratched (33 1/3 rpm at 1x, following the
     * actual rate) or adjusted by touch/phone in CDJ mode (ext_platter_fp, from deck_jog), and by the stopped-track seeks in jog_block */
    if (d->scratching) d->platter += rate * (33.0 + 1.0 / 3.0) / 60.0 * frames / RX_SR;
    { int64_t q = __atomic_exchange_n(&d->ext_platter_fp, 0, __ATOMIC_ACQ_REL); if (q) d->platter += (double)q / 1e6; }
    d->platter -= floor(d->platter);
    jog_block(d, frames);

    /* pre-fader VU (after Mixxx EngineVuMeter: mean |x| over ~1/30 s on a log scale, fast attack, slow decay) */
    float sl = 0, sr = 0;
    for (int i = 0; i < frames; i++) { sl += fabsf(out[2 * i]); sr += fabsf(out[2 * i + 1]); }
    d->vu_sum_l += sl; d->vu_sum_r += sr; d->vu_n += frames;
    if (d->vu_n > RX_SR / 30) {
        float nl = log10f(32767.0f * d->vu_sum_l / (d->vu_n * 1000.0f) + 1), nr = log10f(32767.0f * d->vu_sum_r / (d->vu_n * 1000.0f) + 1);
        d->vu_l = nl > d->vu_l ? nl : d->vu_l - 0.1f * (d->vu_l - nl);
        d->vu_r = nr > d->vu_r ? nr : d->vu_r - 0.1f * (d->vu_r - nr);
        d->vu_l = rx_clampf(d->vu_l, 0, 1); d->vu_r = rx_clampf(d->vu_r, 0, 1);
        d->vu_sum_l = d->vu_sum_r = 0; d->vu_n = 0;
    }
}

/* --------------------------------------------------------------------------------------------------------- view */
void deck_view(rx_deck_t *d, rx_deck_view_t *v) {
    memset(v, 0, sizeof *v);
    track_t *t = cur_track(d);
    v->loaded = t != NULL;
    v->analysing = 0; v->analyse_pct = v->loaded ? 100 : 0;
    v->duration = frames_of(t) / RX_SR;
    v->bpm = d->bpm; v->beat0 = d->beat0;
    v->pos = d->pos / RX_SR;
    v->rate = d->rate_now;
    v->playing = ctlb(d->c.play);
    v->loop_on = d->loop_on;
    v->loop_in = d->loop_in >= 0 ? d->loop_in / RX_SR : -1;
    v->loop_out = d->loop_out >= 0 ? d->loop_out / RX_SR : -1;
    v->cue = d->cue >= 0 ? d->cue / RX_SR : -1;
    for (int i = 0; i < RX_HOTCUES; i++) { v->hotcue[i] = d->hotcue[i] >= 0 ? d->hotcue[i] / RX_SR : -1; v->hotcue_colour[i] = d->hotcue_colour[i]; }
    v->vu_l = d->vu_l; v->vu_r = d->vu_r;
    if (t && t->au) { snprintf(v->title, sizeof v->title, "%s", t->au->title[0] ? t->au->title : (strrchr(t->path, '/') ? strrchr(t->path, '/') + 1 : t->path)); snprintf(v->artist, sizeof v->artist, "%s", t->au->artist); }
    v->key = d->key;
    v->platter_angle = (float)d->platter;
    v->touch_zone = (int)ctlv(d->c.touch_zone);
}
