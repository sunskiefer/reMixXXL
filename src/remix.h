/* reMixXXL -- Mixxx-style DJ decks for Akai Force / MPC OS (mpc_engine VST2).
 *
 * This header is the contract between the parts of the plugin; every part includes it and nothing else of the others.
 *
 *   engine.c    mpc_engine(): the wrapper's create/midi/set_param/get_param/render; owns the control table, the four
 *               decks, the mixer and the effects (deck.c, mixer.c, fx.c, decode.c, analyze.c), the params adapter and
 *               the state chunk.
 *   control.c   the control table: Mixxx's [group],name controls (play, rate, hotcue_1_activate ...). The engine
 *               writes and reads them directly inside render()/midi()/set_param() (the wrapper serialises those with a
 *               lock); other threads (touch, JS timers, the screen) use ctl_post() which queues the write for the next
 *               render(), and ctl_get() which is an atomic read. Listeners (JS connectControl, MIDI LED feedback) are
 *               called from render() after the queue is drained.
 *   overlay.c   the live screen on the Force's spare DRM plane (VADER's overlay program, generalised: a whole-page
 *               ARGB plane found by per-page marker pixels in the skin); screen.c draws the pages into it.
 *   touch.c     the Force's touchscreen read directly (evdev, read-only), turned into wheel/fader/button gestures
 *               on the wheel pages; also the diag log of every touch event (rate, down/up, slots).
 *   midi.c      raw MIDI from the track input: MIDI learn (saved to a file), and Mixxx controller mappings
 *               (<name>.midi.xml + <name>-scripts.js run by QuickJS with Mixxx's engine.* / midi.* script API).
 *
 * Audio contract (engine.h): 44100 Hz, interleaved int16 stereo, render() with 1..128 frames.
 * Everything that can block (file loads, analysis, JS) runs off the audio thread. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define RX_VERSION_STR "0.1.17"
#define RX_SR 44100
#define RX_BLOCK 128
#define RX_DECKS 4
#define RX_HOTCUES 8
#define RX_FX_UNITS 2
#define RX_FX_SLOTS 3
#define RX_MAX_TRACK_SEC (15 * 60)            /* a track longer than this is cut (in-memory decks, int16 stereo) */
#define RX_PATH 1024

/* ------------------------------------------------------------------------------------------------ the control table */
/* Mixxx control names. Groups: "[Channel1]".."[Channel4]", "[Master]", "[Playlist]", "[Library]",
 * "[EffectRack1_EffectUnit1]", "[EffectRack1_EffectUnit1_Effect1]", "[EqualizerRack1_[Channel1]_Effect1]",
 * "[QuickEffectRack1_[Channel1]]", "[reMixXXL]" (our own: page, deck_select, wheel_deck_l/r, folder, browse_*). */
typedef int ctl_id;                              /* an index into the table, -1 = no such control */
#define CTL_NONE (-1)

typedef void (*ctl_listener_fn)(ctl_id id, double value, void *user);

ctl_id ctl_lookup(const char *group, const char *name);
ctl_id ctl_lookup_full(const char *group_dot_name);          /* "[Channel1].play" or "[Channel1],play" */
const char *ctl_group(ctl_id id);
const char *ctl_name(ctl_id id);
int ctl_count(void);
double ctl_get(ctl_id id);                                    /* any thread: atomic read */
void ctl_set(ctl_id id, double v);                            /* engine threads only (inside the wrapper's lock) */
void ctl_post(ctl_id id, double v);                           /* any other thread: queued, applied at next render() */
void ctl_post_delta(ctl_id id, double dv);                    /* queued add (jog ticks accumulate, not overwrite) */
int ctl_add_listener(ctl_id id, ctl_listener_fn fn, void *user);  /* returns a handle; called from render() */
void ctl_remove_listener(int handle);
void ctl_drain(void);                                         /* engine: apply queued posts, fire listeners */
/* engine-side: controls a part owns are created at engine start by ctl_register; min/max/default inform MIDI learn and
 * the params adapter. kind: CK_FLOAT (continuous), CK_TOGGLE (0/1 stays), CK_TRIGGER (1 on press, the engine resets it
 * to 0 after acting), CK_TEXT (read-only text through ctl_text) */
enum { CK_FLOAT, CK_TOGGLE, CK_TRIGGER, CK_INT, CK_TEXT };
ctl_id ctl_register(const char *group, const char *name, int kind, double lo, double hi, double def);
int ctl_kind(ctl_id id);
double ctl_min(ctl_id id);
double ctl_max(ctl_id id);
void ctl_set_text(ctl_id id, const char *text);               /* CK_TEXT: engine side */
int ctl_text(ctl_id id, char *buf, int n);                    /* any thread: the text (copied under a short lock) */
void ctl_init(void);
void ctl_shutdown(void);

/* ------------------------------------------------------------------------------------------------ the screen's data */
/* What the screen draws, published by the engine every block (seqlock copy, like VADER's overlay_publish). */
#define RX_JOG_FRAMES 16              /* the jog ring's film strip: one dimple pitch (15 degrees) in this many frames (tools/remix_skin.py JOG_FRAMES) */
#define RX_JOG_SPIN 0.25              /* how fast the ring turns against the platter (the display updates ~25 times a second: full speed would strobe) */
#define RX_OVERVIEW_W 1024            /* overview strip resolution per deck (columns) */
#define RX_WAVE_PX_PER_SEC 400        /* the scrolling waveform's summary resolution: one column per 1/400 s */
#define RX_WAVE_MAX_COLS (RX_MAX_TRACK_SEC * RX_WAVE_PX_PER_SEC)

typedef struct {                      /* one column of waveform summary: Mixxx's RGB waveform data */
    uint8_t low, mid, high;           /* peak per band, 0..255 */
    uint8_t all;                      /* overall peak */
} rx_wave_col_t;

typedef struct {
    int loaded;                       /* a track is loaded and analysed */
    int analysing;                    /* analysis in progress (progress 0..100 in analyse_pct) */
    int analyse_pct;
    double duration;                  /* seconds */
    double bpm;                       /* 0 = unknown */
    double beat0;                     /* first beat, seconds */
    double pos;                       /* play position, seconds */
    double rate;                      /* playback rate factor (1 = normal), negative when reversed */
    int playing;
    int loop_on;
    double loop_in, loop_out;         /* seconds, -1 none */
    double cue;                       /* main cue point, seconds, -1 none */
    double hotcue[RX_HOTCUES];        /* seconds, -1 none */
    int hotcue_colour[RX_HOTCUES];    /* 0..7 */
    float vu_l, vu_r;                 /* post-fader level 0..1 */
    char title[96], artist[96];
    double key;                       /* 0 unknown, else Mixxx key index 1..24 */
    float platter_angle;              /* the platter's angle, turns (0..1), for the spinning centre label */
    int touch_zone;                   /* 0 none, 1 top (vinyl), 2 ring (bend) */
} rx_deck_view_t;

typedef struct {
    rx_deck_view_t deck[RX_DECKS];
    int page;                         /* RX_PAGE_* */
    int deck_select;                  /* DECK page's deck 0..3 */
    int wheel_deck[2];                /* WHEELS page: left and right wheel's deck 0..3; WHEEL page uses [0] */
    double zoom;                      /* DECK page scrolling waveform: seconds across the box */
    float master_vu_l, master_vu_r;
    double cpu_pct;                   /* audio thread cost, % of the block budget (shown on DECK) */
    double xfader;                    /* -1..1 */
    float ch_vol[RX_DECKS];
} rx_view_t;

enum { RX_PAGE_DECK = 0, RX_PAGE_BROWSE, RX_PAGE_MIXER, RX_PAGE_FX, RX_PAGE_WHEELS, RX_PAGE_WHEEL, RX_PAGES };

/* The waveform summaries live in the deck; the screen reads them through these (copied columns, thread-safe). */
int rx_wave_columns(int deck, rx_wave_col_t *out, int first_col, int n);   /* returns columns copied */
int rx_wave_total_cols(int deck);
int rx_overview(int deck, rx_wave_col_t *out);                             /* RX_OVERVIEW_W columns, returns 1 if any */

/* The screen's boxes on the plugin page (skin coordinates: 1280 x 628, the page MPC draws under its own header).
 * The skin builder (tools/remix_skin.py) puts marker pixels at each box's corners; overlay.c reads them from the display
 * to find where the page is and which page shows (RX_MARK_* ids). The same numbers are used by screen.c to draw. */
#define RX_SKIN_W 1280
#define RX_SKIN_H 628
typedef struct { int x, y, w, h; } rx_box_t;
/* per page, the boxes the overlay draws into (all in skin coordinates); the skin builder writes them to
 * build/boxes.json and the C side has them as constants in boxes.h (generated by tools/gen_params.py) */

/* ------------------------------------------------------------------------------------------------ the engine's parts */
typedef struct rx_engine rx_engine_t;         /* engine.c */
typedef struct rx_deck rx_deck_t;             /* deck.c */
typedef struct rx_mixer rx_mixer_t;           /* mixer.c */
typedef struct rx_fx rx_fx_t;                 /* fx.c */

/* deck.c */
rx_deck_t *deck_create(int index);
void deck_destroy(rx_deck_t *d);
void deck_register_controls(rx_deck_t *d);                    /* [ChannelN] controls, and the EQ/quick-effect groups */
void deck_load(rx_deck_t *d, const char *path);               /* off the audio thread: decode, analyse (analyze.c), swap in */
void deck_eject(rx_deck_t *d);
void deck_render(rx_deck_t *d, float *out_lr, int frames);    /* pre-fader stereo float at RX_SR; applies rate, scratch, loops */
void deck_tick_controls(rx_deck_t *d);                        /* once per block: act on triggers, update readbacks */
void deck_view(rx_deck_t *d, rx_deck_view_t *v);
const char *deck_track_path(rx_deck_t *d);
void deck_host_tempo(rx_deck_t *d, double bpm, int playing);  /* MPC's own tempo (for sync to host, optional) */
/* scratch/jog from a wheel (touch.c, midi.c): Mixxx semantics */
void deck_scratch_enable(rx_deck_t *d, int enable, double rpm, double alpha, double beta);
void deck_scratch_tick(rx_deck_t *d, double ticks);           /* ticks of the wheel (Mixxx: engine.scratchTick) */
int deck_is_playing(rx_deck_t *d);
void deck_seek_by_turns(rx_deck_t *d, double turns);          /* a finger turning the wheel of a stopped deck: the playhead follows */
void deck_ext_jog(rx_deck_t *d, int on);                      /* a finger on the wheel (touch.c) works the wheel: the skin knob stands back */
void deck_jog(rx_deck_t *d, double amount);                   /* nudge (Mixxx [ChannelN],jog) */
int deck_index(rx_deck_t *d);

/* mixer.c: 4 channels -> master; EQ, filter, gain, fader, crossfader, master gain, VU */
rx_mixer_t *mixer_create(void);
void mixer_destroy(rx_mixer_t *m);
void mixer_register_controls(rx_mixer_t *m);
void mixer_render(rx_mixer_t *m, float *deck_lr[RX_DECKS], float *out_lr, int frames, rx_fx_t *fx);
void mixer_view(rx_mixer_t *m, rx_view_t *v);

/* fx.c: 2 units x 3 slots, Mixxx-style (echo, filter, reverb, flanger, phaser, bitcrusher, moog filter); a unit is
 * applied to the channels enabled on it, after the EQ, before the fader (Mixxx's insert position) */
rx_fx_t *fx_create(void);
void fx_destroy(rx_fx_t *f);
void fx_register_controls(rx_fx_t *f);
void fx_process_channel(rx_fx_t *f, int channel, float *lr, int frames);   /* in place */
void fx_process_master(rx_fx_t *f, float *lr, int frames);
int fx_effect_count(void);
const char *fx_effect_name(int i);
const char *fx_param_name(int effect, int p);               /* NULL past the end */
int fx_param_count(int effect);

/* decode.c: a whole file to int16 stereo at RX_SR; returns frames, or <0 (errno-style) */
typedef struct { int16_t *lr; int64_t frames; char title[96], artist[96]; } rx_audio_t;
int decode_file(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user);
void decode_free(rx_audio_t *a);
int decode_is_audio_file(const char *name);                   /* by extension: wav, flac, mp3, aif/aiff, ogg? (ogg no) */

/* analyze.c: waveform summary, overview, BPM + first beat (qm-dsp), replay gain; runs on the loading thread */
typedef struct {
    rx_wave_col_t *cols; int ncols;                           /* RX_WAVE_PX_PER_SEC per second */
    rx_wave_col_t overview[RX_OVERVIEW_W];
    double bpm, beat0;                                        /* beat0: first beat in seconds */
    float gain;                                               /* replay gain factor (1 = none) */
} rx_analysis_t;
int analyze_audio(const int16_t *lr, int64_t frames, rx_analysis_t *out, void (*progress)(void *, int), void *user);
void analyze_free(rx_analysis_t *a);

/* ------------------------------------------------------------------------------------------------ browse */
/* The track list (engine.c): a folder, sorted file names, pages of RX_BROWSE_ROWS, a selected row. Mixxx names for
 * the mapping: [Playlist],SelectTrackKnob/SelectNextTrack/SelectPrevTrack, [ChannelN],LoadSelectedTrack. */
#define RX_BROWSE_ROWS 6
int browse_set_folder(rx_engine_t *e, const char *path);      /* rescans (off the audio thread) */
int browse_count(rx_engine_t *e);
const char *browse_name(rx_engine_t *e, int i);               /* display name (file name without extension) */
const char *browse_path(rx_engine_t *e, int i);
double browse_bpm(rx_engine_t *e, int i);                     /* from the sidecar cache, 0 unknown */

/* ------------------------------------------------------------------------------------------------ the screen */
typedef struct rx_overlay rx_overlay_t;
rx_overlay_t *overlay_start(const char *plugin_dir, int *ok);  /* ok=0: no plane (the log says why); still returns */
void overlay_stop(rx_overlay_t *o);
void overlay_publish(rx_overlay_t *o, const rx_view_t *v);     /* engine, every block */
void overlay_read_view(rx_overlay_t *o, rx_view_t *v);
int overlay_page_shown(rx_overlay_t *o);                       /* RX_PAGE_* currently found on the display, -1 none */
int overlay_geom(rx_overlay_t *o, int *rot, int *lw, int *lh, int *ox, int *oy, unsigned *fbw, unsigned *fbh);   /* the display's turn and size, and the page's place on MPC's screen (for touch); 0 when no page was found */
int overlay_box_origin(rx_overlay_t *o, int *x, int *y);       /* the page's top-left on the display (for touch) */
int overlay_live(rx_overlay_t *o);
int overlay_status(rx_overlay_t *o, int i, char *buf, int n);   /* the last log lines, newest first (on-screen diagnosis) */
void overlay_set_platter_frames(rx_overlay_t *o, const uint8_t *argb, int w, int h, int frames); /* centre label strip */

/* screen.c: draws one frame of a page into an ARGB buffer (w x h = the page, pitch bytes); overlay.c calls it */
void screen_draw(int page, const rx_view_t *v, uint8_t *argb, int w, int h, int pitch, double now_s);
void screen_set_ring(const uint8_t *ring_rgba, const uint8_t *light_rgba, int d);   /* the WHEEL page's turning ring and its fixed light (straight RGBA, d x d each) */
void screen_set_panel_font(const char *ttf_path);              /* optional: a font file next to the plugin for text */

/* ------------------------------------------------------------------------------------------------ touch */
typedef struct rx_touch rx_touch_t;
void touch_status(rx_touch_t *t, char *buf, int n);
rx_touch_t *touch_start(const char *plugin_dir, rx_overlay_t *o, rx_engine_t *e);   /* opens /dev/input/event*, logs */
void touch_stop(rx_touch_t *t);
int touch_available(rx_touch_t *t);
void touch_set_wheel_decks(rx_touch_t *t, int left, int right);
/* the wheel geometry on each page, display pixels relative to the page origin (from boxes.h): centre, radii */
typedef struct { int cx, cy, r_outer, r_top; int deck_slot; } rx_wheel_geom_t;   /* r_top: inside = vinyl/top zone */
void touch_set_wheels(rx_touch_t *t, int page, const rx_wheel_geom_t *w, int n);
typedef struct { int x, y, w, h; ctl_id ctl; int kind; double lo, hi; } rx_touch_ctl_t;  /* kind: 0 button(trigger), 1 toggle, 2 vfader, 3 hfader */
void touch_set_controls(rx_touch_t *t, int page, const rx_touch_ctl_t *c, int n);

/* ------------------------------------------------------------------------------------------------ the companion web page */
typedef struct rx_web rx_web_t;
rx_web_t *web_start(rx_engine_t *e);                                      /* http://<ip>:8090.. in a browser: live waveforms (web.c) */
void web_stop(rx_web_t *w);
void web_publish(rx_web_t *w, const rx_view_t *v);              /* engine, every block (try-lock copy, never waits) */
const char *web_url(rx_web_t *w);
#define RX_QR_VER 4                    /* the page address's QR code: version 4 = 33 modules, level M (src/qr.c) */
#define RX_QR_N 33
#define RX_QR_BYTES 5                 /* 8 modules a byte */
int web_qr(rx_web_t *w, unsigned char *out);                    /* RX_QR_N*RX_QR_BYTES bytes: 1 ok, -1 no network, 0 busy */

/* ------------------------------------------------------------------------------------------------ MIDI */
typedef struct rx_midi rx_midi_t;
rx_midi_t *midi_start(const char *plugin_dir, rx_engine_t *e);
void midi_stop(rx_midi_t *m);
void midi_in(rx_midi_t *m, const uint8_t *msg, int len);        /* engine->midi(): inside the wrapper's lock */
void midi_tick(rx_midi_t *m);                                   /* once per block: JS timers, LED feedback queue */
int midi_learn_arm(rx_midi_t *m, ctl_id target);                /* next incoming message maps to target */
int midi_learn_clear(rx_midi_t *m, ctl_id target);
int midi_load_mapping(rx_midi_t *m, const char *xml_path);      /* a Mixxx <name>.midi.xml (+ its -scripts.js) */
const char *midi_mapping_name(rx_midi_t *m);
int midi_mapping_count(rx_midi_t *m);                           /* mappings found in <plugin>/Mappings */
const char *midi_mapping_file(rx_midi_t *m, int i);
/* CONTROLLERS page / web map builder (midi.c; worker and web threads only) */
void midi_rescan(rx_midi_t *m);
int midi_mapping_total(rx_midi_t *m);                           /* -1 while the list is rebuilt */
int midi_mapping_path_at(rx_midi_t *m, int i, char *out, int n);
int midi_mapping_name_at(rx_midi_t *m, int i, char *out, int n);
int midi_last_msg(rx_midi_t *m, int *st, int *d1, int *d2);     /* returns a counter that grows with each note/CC/bend */
int midi_report(rx_midi_t *m, char *file, int fn, char *name, int nn, int *nin, int *nscript, int *nunknown, char *unknown_list, int un, int try_only);   /* 1 loaded, 0 none, -1 busy (try_only) */
int midi_unload_mapping(rx_midi_t *m);
int midi_pick_mapping(rx_midi_t *m, const char *path);          /* load and remember */
int midi_pick_none(rx_midi_t *m);
void midi_autoload(rx_midi_t *m);
int midi_save_map(rx_midi_t *m, const char *name, const char *data, size_t len, char *saved, int sn);
/* LED/controller feedback: MPC OS ignores VST MIDI out, so midi.c opens an ALSA sequencer port when libasound is there
 * (dlopen at runtime: never a link dependency); without it, feedback is logged only */

/* ------------------------------------------------------------------------------------------------ diag log */
void rx_log(const char *fmt, ...);                               /* <plugin dir>/remixxxl_diag.log, timestamped */
void rx_log_open(const char *plugin_dir);
const char *rx_plugin_dir(void);

/* deck access for the other parts (touch.c, midi.c) */
rx_deck_t *rx_deck(rx_engine_t *e, int i);
void compat_init(void);                                          /* compat.c: Mixxx control names older maps use (aliases) */
void compat_tick(void);
void engine_map_rescan(rx_engine_t *e);                          /* web map builder: a map file was saved (worker looks again) */
void engine_map_pick(rx_engine_t *e, const char *path);          /* load a map and remember it (worker) */
void engine_map_none(rx_engine_t *e);
rx_midi_t *engine_midi(rx_engine_t *e);
rx_engine_t *rx_engine_instance(void);                           /* the one instance that owns the screen/touch/MIDI */
