/* reMixXXL -- the Force's touchscreen, read directly from evdev (read-only, never grabbed), turned into wheel /
 * fader / button gestures on the two wheel pages, plus the diag log of every touch event.
 *
 * Device discovery (touch_start): every /dev/input/event* is opened O_RDONLY|O_NONBLOCK, its name (EVIOCGNAME),
 * EV_ABS bits (EVIOCGBIT) and abs ranges (EVIOCGABS) are logged. A usable device has either multitouch type B
 * (ABS_MT_POSITION_X/Y + ABS_MT_SLOT + ABS_MT_TRACKING_ID) or single-touch (ABS_X/ABS_Y + BTN_TOUCH). EVIOCGRAB is
 * tried once to learn whether MPC (or anything) holds an exclusive grab: EBUSY -> "grabbed by MPC: no direct touch",
 * the device is closed and not used; success -> the grab is RELEASED at once (EVIOCGRAB 0). We only read the
 * stream MPC also reads; we never steal it. The first usable device wins.
 *
 * Reader thread: poll() + read() of input_event batches; slots (up to 10 fingers) are tracked through SYN_REPORT.
 * Coordinates are scaled from the abs range to the 1280 x 720 display, then made relative to the plugin page's
 * origin on the display (overlay_box_origin), so they are in skin coordinates (RX_PART_BOXES, RX_WHEEL*_CX/CY/R).
 * The first 200 events are logged in full; after that a summary line every 2 s (events/s, fingers down, min/avg/
 * max interval between position reports) - the latency diag the user asked for.
 *
 * Gestures, only while the shown page (overlay_page_shown) is RX_PAGE_WHEELS or RX_PAGE_WHEEL:
 *   wheel circle: a finger inside r_top with [ChannelN],vinyl_mode=1 scratches (deck_scratch_enable on down,
 *     deck_scratch_tick per move with ticks = unwrapped angle delta * 720/2pi * feel, deck_scratch_enable(0) on
 *     lift); with vinyl_mode=0 (CDJ) it nudges (deck_jog(angle delta * k)). [ChannelN],touch_zone
 *     (1 top, 2 ring, 0 none) is posted as a hint for the screen.
 *   part boxes (RX_PART_BOXES, PART_CONTROLS from tools/remix_pages.py, mirrored in PART_ACTIONS below): trigger /
 *     toggle / hold / trigger_hold / vfader_inv (tempo: top of the box = rate -1, bottom = +1, + at the bottom like
 *     the CDJ) / knob (jog feel: vertical drag, dy/200) / two-button parts (left half, right half) / deck select
 *     100..103 -> [reMixXXL],wheel_deck_l or _r.
 * Everything is posted through ctl_post / ctl_post_delta; deck_scratch_* / deck_jog are the only engine calls (they
 * are documented thread-safe for this). The gesture state is per slot so two fingers can work two wheels.
 *
 * touch_set_wheels / touch_set_controls let engine.c replace the built-in geometry for a page (stored, used as is).
 *
 * UNVERIFIED on the device: the event node name and ranges, whether MPC grabs it (EVIOCGRAB), whether the
 * touchscreen reports type B multitouch. The log says what was found. */
#define _GNU_SOURCE
#include "remix.h"
#include "boxes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define TOUCH_SLOTS 10
#define DISPLAY_W 1280
#define DISPLAY_H 720
#define LOG_FULL_EVENTS 200
#define SUMMARY_S 2.0
#define MAX_WHEELS 2
#define MAX_PAGE_CTLS 64
#define SCRATCH_TICKS_PER_REV 720.0      /* the plugin's wheel resolution (deck_scratch_tick ticks per revolution) */
#define SCRATCH_RPM 33.3333333
#define SCRATCH_ALPHA (1.0 / 8)          /* Mixxx's usual alpha/beta */
#define SCRATCH_BETA (SCRATCH_ALPHA / 32)

/* PART_CONTROLS from tools/remix_pages.py, as actions: one or two controls per part */
enum { A_TRIGGER, A_TOGGLE, A_TRIGGER_HOLD, A_HOLD, A_VFADER_INV, A_KNOB };
typedef struct { int id; const char *c1; int a1; const char *c2; int a2; } part_action_t;
static const part_action_t PART_ACTIONS[] = {
    {3, "cue_default", A_TRIGGER_HOLD, NULL, 0},
    {4, "play", A_TOGGLE, NULL, 0},
    {5, "rate", A_VFADER_INV, NULL, 0},
    {6, "rate_set_default", A_TRIGGER, NULL, 0},
    {7, "keylock", A_TOGGLE, NULL, 0},
    {8, "rateRange_next", A_TRIGGER, NULL, 0},
    {9, "beatloop_4_activate", A_TRIGGER, "beatloop_8_activate", A_TRIGGER},
    {10, "loop_in", A_TRIGGER, "loop_out", A_TRIGGER},
    {11, "reloop_toggle", A_TRIGGER, NULL, 0},
    {12, "beatjump_backward", A_TRIGGER, "beatjump_forward", A_TRIGGER},
    {13, "slip_enabled", A_TOGGLE, "reverse", A_TOGGLE},
    {14, "track_prev", A_TRIGGER, "track_next", A_TRIGGER},
    {15, "back", A_HOLD, "fwd", A_HOLD},
    {16, "jog_feel", A_KNOB, NULL, 0},
    {17, "vinyl_mode", A_TOGGLE, NULL, 0},
    {18, "beatsync", A_TRIGGER, "sync_master", A_TOGGLE},
    {19, "key_sync", A_TRIGGER, NULL, 0},
};
#define PART_ACTIONS_N (sizeof PART_ACTIONS / sizeof PART_ACTIONS[0])

typedef struct {
    int down;                            /* finger present */
    int tracking_id;
    int x, y;                            /* page coordinates (skin pixels) */
    int raw_x, raw_y;
    int moved;                           /* position changed since the last SYN_REPORT */
    /* the gesture this finger owns */
    int kind;                            /* 0 none, 1 wheel, 2 part */
    int deck;                            /* 0..3 for a wheel gesture */
    int wheel;                           /* wheel index on the page */
    int zone;                            /* 1 top, 2 ring */
    int scratching;
    double last_angle;
    int part_idx;                        /* index into RX_PART_BOXES, or -1 */
    int part_id, side;
    ctl_id hold_ctl;                     /* posted 0 on lift (hold / trigger_hold) */
    int knob_y0; double knob_v0;         /* knob drag */
    ctl_id knob_ctl;
    int tempo_ctl;                       /* vfader: ctl to follow */
    int box_y, box_h;
    int list_y0;                         /* BROWSE list drag: the y of the last row step */
} slot_t;

typedef struct { int cx, cy, r, r_top; } wheel_t;

struct rx_touch {
    rx_overlay_t *overlay;
    rx_engine_t *engine;
    int fd;
    int available;
    char dev_name[128], dev_path[64];
    int mt;                              /* multitouch type B */
    int ax_min, ax_max, ay_min, ay_max;
    pthread_t thread;
    atomic_int running;

    slot_t slot[TOUCH_SLOTS];
    int cur_slot;
    int st_down;                         /* single-touch BTN_TOUCH state */

    atomic_int wheel_deck[2];            /* decks of the left/right (WHEELS) wheel; WHEEL uses [0] */
    wheel_t wheels[RX_PAGES][MAX_WHEELS]; int nwheels[RX_PAGES];
    rx_touch_ctl_t ctls[RX_PAGES][MAX_PAGE_CTLS]; int nctls[RX_PAGES];
    pthread_mutex_t geom_lock;

    /* diag */
    long events, logged;
    double t_window; long win_events, win_reports;
    double win_min_dt, win_max_dt, win_sum_dt; long win_ndt;
    double last_report_t;
    ctl_id zone_ctl[RX_DECKS], vinyl_ctl[RX_DECKS], feel_ctl[RX_DECKS], wheel_l_ctl, wheel_r_ctl, brow_ctl;
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------------------------------ geometry */
static void default_geometry(rx_touch_t *t) {
    t->wheels[RX_PAGE_WHEELS][0] = (wheel_t){ RX_WHEELS_0_CX, RX_WHEELS_0_CY, RX_WHEELS_0_R, RX_WHEELS_0_RTOP };
    t->wheels[RX_PAGE_WHEELS][1] = (wheel_t){ RX_WHEELS_1_CX, RX_WHEELS_1_CY, RX_WHEELS_1_R, RX_WHEELS_1_RTOP };
    t->nwheels[RX_PAGE_WHEELS] = 2;
    t->wheels[RX_PAGE_WHEEL][0] = (wheel_t){ RX_WHEEL_0_CX, RX_WHEEL_0_CY, RX_WHEEL_0_R, RX_WHEEL_0_RTOP };
    t->nwheels[RX_PAGE_WHEEL] = 1;
}

void touch_set_wheels(rx_touch_t *t, int page, const rx_wheel_geom_t *w, int n) {
    if (!t || page < 0 || page >= RX_PAGES) return;
    pthread_mutex_lock(&t->geom_lock);
    t->nwheels[page] = 0;
    for (int i = 0; i < n && i < MAX_WHEELS; i++) {
        t->wheels[page][i] = (wheel_t){ w[i].cx, w[i].cy, w[i].r_outer, w[i].r_top };
        t->nwheels[page]++;
    }
    pthread_mutex_unlock(&t->geom_lock);
    rx_log("touch: page %d wheels replaced (%d)", page, t->nwheels[page]);
}

void touch_set_controls(rx_touch_t *t, int page, const rx_touch_ctl_t *c, int n) {
    if (!t || page < 0 || page >= RX_PAGES) return;
    pthread_mutex_lock(&t->geom_lock);
    t->nctls[page] = 0;
    for (int i = 0; i < n && i < MAX_PAGE_CTLS; i++) t->ctls[page][t->nctls[page]++] = c[i];
    pthread_mutex_unlock(&t->geom_lock);
    rx_log("touch: page %d extra controls set (%d)", page, t->nctls[page]);
}

void touch_set_wheel_decks(rx_touch_t *t, int left, int right) {
    if (!t) return;
    atomic_store(&t->wheel_deck[0], left);
    atomic_store(&t->wheel_deck[1], right);
}

static int wheel_deck_of(rx_touch_t *t, int page, int wheel) {
    /* the engine's wheel_deck_l/r controls are the truth when they exist; touch_set_wheel_decks is the fallback */
    ctl_id c = (page == RX_PAGE_WHEELS && wheel == 1) ? t->wheel_r_ctl : t->wheel_l_ctl;
    int d = c != CTL_NONE ? (int)ctl_get(c) : atomic_load(&t->wheel_deck[wheel]);
    return d < 0 ? 0 : d >= RX_DECKS ? RX_DECKS - 1 : d;
}

static ctl_id deck_ctl(int deck, const char *name) {
    char g[24];
    snprintf(g, sizeof g, "[Channel%d]", deck + 1);
    return ctl_lookup(g, name);
}

static double deck_feel(rx_touch_t *t, int deck) {
    ctl_id c = t->feel_ctl[deck];
    double f = c != CTL_NONE ? ctl_get(c) : 0.5;
    return 0.25 + f * 1.5;               /* 0 -> 0.25x, 0.5 -> 1x, 1 -> 1.75x */
}

static void post_zone(rx_touch_t *t, int deck, int zone) {
    if (t->zone_ctl[deck] != CTL_NONE) ctl_post(t->zone_ctl[deck], zone);
}

/* ------------------------------------------------------------------------------------------------ gestures */
static void finger_down(rx_touch_t *t, slot_t *s, int page) {
    s->kind = 0; s->hold_ctl = CTL_NONE; s->knob_ctl = CTL_NONE; s->tempo_ctl = CTL_NONE; s->part_idx = -1;
    if (page == RX_PAGE_BROWSE) {        /* the track list: a finger on it that moves up or down scrolls (MPC's own tap still selects the row) */
        if (t->brow_ctl != CTL_NONE && s->y >= RX_STRIPE_H && s->y < RX_SKIN_H - 100) { s->kind = 3; s->list_y0 = s->y; }
        return;
    }
    pthread_mutex_lock(&t->geom_lock);
    /* 1. the wheels */
    for (int w = 0; w < t->nwheels[page]; w++) {
        wheel_t *wh = &t->wheels[page][w];
        double dx = s->x - wh->cx, dy = s->y - wh->cy, dist = sqrt(dx * dx + dy * dy);
        if (dist > wh->r) continue;
        if (dist < wh->r * 0.30) continue;     /* the middle: the QR switch (WHEEL) / the deck's centre disc (WHEELS) */
        int deck = wheel_deck_of(t, page, w);
        int vinyl = t->vinyl_ctl[deck] != CTL_NONE ? ctl_get(t->vinyl_ctl[deck]) > 0.5 : 1;
        s->kind = 1; s->deck = deck; s->wheel = w;
        s->zone = dist < wh->r_top ? 1 : 2;
        s->last_angle = atan2(dy, dx);
        deck_ext_jog(rx_deck(t->engine, deck), 1);
        s->scratching = vinyl && deck_is_playing(rx_deck(t->engine, deck));   /* 0.1.20: VINYL scratches the track (clockwise = forward); CDJ adjusts (nudges the pitch) */       /* stopped: the wheel moves the playhead itself (finger_move) */               /* VINYL: the whole wheel (disc and ring) spins the track; CDJ: it nudges the pitch (deck_jog, scaled by JOG ADJUST) */
        if (s->scratching) deck_scratch_enable(rx_deck(t->engine, deck), 1, SCRATCH_RPM, SCRATCH_ALPHA, SCRATCH_BETA);
        post_zone(t, deck, s->zone);
        pthread_mutex_unlock(&t->geom_lock);
        if (t->logged < LOG_FULL_EVENTS * 4) rx_log("touch: wheel %d down deck %d zone %s (%d,%d) r=%.0f", w, deck + 1, s->zone == 1 ? "top" : "ring", s->x, s->y, dist);
        return;
    }
    /* 0.1.17 test: the wheel only; the buttons round it stay the skin's own (this layer's boxes would fire them twice) */
    pthread_mutex_unlock(&t->geom_lock);
    return;
    /* 2. engine-supplied extra controls for the page */
    for (int i = 0; i < t->nctls[page]; i++) {
        rx_touch_ctl_t *c = &t->ctls[page][i];
        if (s->x < c->x || s->x >= c->x + c->w || s->y < c->y || s->y >= c->y + c->h || c->ctl == CTL_NONE) continue;
        s->kind = 2; s->part_id = -1;
        if (c->kind == 0) ctl_post(c->ctl, 1);
        else if (c->kind == 1) ctl_post(c->ctl, ctl_get(c->ctl) > 0.5 ? 0 : 1);
        else if (c->kind == 2) { s->tempo_ctl = c->ctl; s->box_y = c->y; s->box_h = c->h; ctl_post(c->ctl, c->lo + (c->hi - c->lo) * (1.0 - (double)(s->y - c->y) / (c->h > 1 ? c->h - 1 : 1))); }
        else if (c->kind == 3) ctl_post(c->ctl, c->lo + (c->hi - c->lo) * (double)(s->x - c->x) / (c->w > 1 ? c->w - 1 : 1));
        pthread_mutex_unlock(&t->geom_lock);
        return;
    }
    pthread_mutex_unlock(&t->geom_lock);
    /* 3. the built-in part boxes of the page */
    for (int i = 0; i < (int)RX_PART_BOXES_N; i++) {
        const rx_part_box_t *b = &RX_PART_BOXES[i];
        if (b->page != page || s->x < b->x || s->x >= b->x + b->w || s->y < b->y || s->y >= b->y + b->h) continue;
        s->kind = 2; s->part_idx = i; s->part_id = b->id; s->side = b->side;
        if (b->id >= 100 && b->id <= 103) {                 /* DECK 1..4 select for this side */
            ctl_id c = (page == RX_PAGE_WHEELS && b->side == 1) ? t->wheel_r_ctl : t->wheel_l_ctl;
            if (c != CTL_NONE) ctl_post(c, b->id - 100);
            atomic_store(&t->wheel_deck[page == RX_PAGE_WHEELS ? b->side : 0], b->id - 100);
            rx_log("touch: deck %d selected for wheel side %d", b->id - 99, b->side);
            return;
        }
        if (b->id == 1) return;                             /* the platter box outside the circle: nothing */
        const part_action_t *pa = NULL;
        for (int k = 0; k < (int)PART_ACTIONS_N; k++) if (PART_ACTIONS[k].id == b->id) { pa = &PART_ACTIONS[k]; break; }
        if (!pa) return;
        int deck = wheel_deck_of(t, page, page == RX_PAGE_WHEELS ? b->side : 0);
        const char *cname = pa->c1; int action = pa->a1;
        if (pa->c2 && s->x >= b->x + b->w / 2) { cname = pa->c2; action = pa->a2; }
        ctl_id c = deck_ctl(deck, cname);
        if (c == CTL_NONE) { rx_log("touch: part %d: no control [Channel%d],%s", b->id, deck + 1, cname); return; }
        switch (action) {
        case A_TRIGGER: ctl_post(c, 1); break;
        case A_TOGGLE: ctl_post(c, ctl_get(c) > 0.5 ? 0 : 1); break;
        case A_TRIGGER_HOLD: case A_HOLD: ctl_post(c, 1); s->hold_ctl = c; break;
        case A_VFADER_INV:
            s->tempo_ctl = c; s->box_y = b->y; s->box_h = b->h;
            ctl_post(c, 1.0 - ((double)(s->y - b->y) / (b->h > 1 ? b->h - 1 : 1)) * 2.0);   /* top +1 (slower with rate_dir -1), bottom -1 (faster) */
            break;
        case A_KNOB: s->knob_ctl = c; s->knob_y0 = s->y; s->knob_v0 = ctl_get(c); break;
        }
        if (t->logged < LOG_FULL_EVENTS * 4) rx_log("touch: part %d (%s) deck %d -> [Channel%d],%s", b->id, action == A_TOGGLE ? "toggle" : action == A_TRIGGER ? "trigger" : "hold/drag", deck + 1, deck + 1, cname);
        return;
    }
}

static void finger_move(rx_touch_t *t, slot_t *s, int page) {
    (void)page;
    if (s->kind == 3) {                  /* BROWSE list drag: one entry per 78 px (a row), the engine turns the pages */
        int dy = s->y - s->list_y0;
        while (dy >= 78) { ctl_post(t->brow_ctl, 0); s->list_y0 += 78; dy -= 78; }      /* finger down the screen: the list moves up = earlier */
        while (dy <= -78) { ctl_post(t->brow_ctl, 7); s->list_y0 -= 78; dy += 78; }
        return;
    }
    if (s->kind == 1) {
        wheel_t *wh = &t->wheels[page][s->wheel];
        double dx = s->x - wh->cx, dy = s->y - wh->cy;
        double a = atan2(dy, dx), d = a - s->last_angle;
        while (d > M_PI) d -= 2 * M_PI;            /* unwrap to -pi..pi */
        while (d < -M_PI) d += 2 * M_PI;
        s->last_angle = a;
        double feel = deck_feel(t, s->deck);
        rx_deck_t *dk = rx_deck(t->engine, s->deck);
        if (!deck_is_playing(dk)) deck_seek_by_turns(dk, d / (2 * M_PI) * feel);
        else if (s->scratching) deck_scratch_tick(dk, d * (SCRATCH_TICKS_PER_REV / (2 * M_PI)) * feel);
        else deck_jog(dk, d * feel * 2.0);         /* a full turn ~ 12.6 jog units at feel 1: about Mixxx's jog scale */
        return;
    }
    if (s->kind != 2) return;
    if (s->tempo_ctl != CTL_NONE) {
        double f = (double)(s->y - s->box_y) / (s->box_h > 1 ? s->box_h - 1 : 1);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ctl_post(s->tempo_ctl, 1.0 - f * 2.0);
    }
    if (s->knob_ctl != CTL_NONE) {
        double v = s->knob_v0 - (double)(s->y - s->knob_y0) / 200.0;   /* up = more */
        double lo = ctl_min(s->knob_ctl), hi = ctl_max(s->knob_ctl);
        ctl_post(s->knob_ctl, v < lo ? lo : v > hi ? hi : v);
    }
}

static void finger_up(rx_touch_t *t, slot_t *s) {
    if (s->kind == 1) {
        deck_ext_jog(rx_deck(t->engine, s->deck), 0);
        if (s->scratching) deck_scratch_enable(rx_deck(t->engine, s->deck), 0, 0, 0, 0);
        post_zone(t, s->deck, 0);
    } else if (s->kind == 2 && s->hold_ctl != CTL_NONE) ctl_post(s->hold_ctl, 0);
    s->kind = 0; s->hold_ctl = CTL_NONE; s->knob_ctl = CTL_NONE; s->tempo_ctl = CTL_NONE; s->scratching = 0;
}

/* ------------------------------------------------------------------------------------------------ the reader */
/* MPC's landscape screen (lw x lh, u right, v down) turned into the display (x, y): backwards, in continuous coordinates (XYZ-STACK's finding:
 * the Force's panel is portrait and MPC's screen is turned into it; 3.9.1: rot 3) */
static inline void fb_to_ui_f(int rot, int lw, int lh, float x, float y, float *u, float *v) {
    switch (rot) {
    case 0: *u = x; *v = y; break;
    case 1: *u = y; *v = (float)lh - x; break;
    case 2: *u = (float)lw - x; *v = (float)lh - y; break;
    default: *u = (float)lw - y; *v = x; break;
    }
}

static void scale_to_page(rx_touch_t *t, slot_t *s, int ox, int oy) {
    int rot, lw, lh, pox, poy;
    unsigned fbw, fbh;
    if (t->overlay && overlay_geom(t->overlay, &rot, &lw, &lh, &pox, &poy, &fbw, &fbh) && t->ax_max > t->ax_min && t->ay_max > t->ay_min) {
        /* the touch chip's range -> the display's pixels (the kernel already gives positions in the display's orientation) -> MPC's screen -> the page */
        float fx = (float)(s->raw_x - t->ax_min) / (float)(t->ax_max - t->ax_min) * (float)fbw;
        float fy = (float)(s->raw_y - t->ay_min) / (float)(t->ay_max - t->ay_min) * (float)fbh;
        float u, v;
        fb_to_ui_f(rot, lw, lh, fx, fy, &u, &v);
        s->x = (int)floorf(u - (float)pox);
        s->y = (int)floorf(v - (float)poy);
        return;
    }
    int dx = t->ax_max > t->ax_min ? (int)((long)(s->raw_x - t->ax_min) * (DISPLAY_W - 1) / (t->ax_max - t->ax_min)) : s->raw_x;
    int dy = t->ay_max > t->ay_min ? (int)((long)(s->raw_y - t->ay_min) * (DISPLAY_H - 1) / (t->ay_max - t->ay_min)) : s->raw_y;
    s->x = dx - ox;
    s->y = dy - oy;
}

static void sync_report(rx_touch_t *t) {
    double now = now_s();
    int page = t->overlay ? overlay_page_shown(t->overlay) : -1;
    int ox = 0, oy = 0;
    if (t->overlay && !overlay_box_origin(t->overlay, &ox, &oy)) { ox = 0; oy = 110; }   /* no page found: MPC's 110 px header (docs/NOTES.md) */
    int active = page == RX_PAGE_WHEEL || page == RX_PAGE_WHEELS;   /* 0.1.17 test: this layer works the WHEEL page's wheel only (it has never been live on a Force before) */
    int fingers = 0;
    for (int i = 0; i < TOUCH_SLOTS; i++) {
        slot_t *s = &t->slot[i];
        if (s->down && s->tracking_id < 0) {            /* lifted in this report */
            s->down = 0;
            if (t->logged < LOG_FULL_EVENTS) rx_log("touch: slot %d UP", i);
            finger_up(t, s);
            continue;
        }
        if (!s->down) continue;
        fingers++;
        scale_to_page(t, s, ox, oy);
        if (s->tracking_id >= 0 && s->kind == 0 && s->moved == 2) {   /* 2: just went down */
            if (t->logged < LOG_FULL_EVENTS) rx_log("touch: slot %d DOWN raw (%d,%d) page (%d,%d) shown page %d origin (%d,%d)", i, s->raw_x, s->raw_y, s->x, s->y, page, ox, oy);
            if (active) finger_down(t, s, page);
        } else if (s->moved) {
            if (t->logged < LOG_FULL_EVENTS) rx_log("touch: slot %d move raw (%d,%d) page (%d,%d)", i, s->raw_x, s->raw_y, s->x, s->y);
            if (s->kind) finger_move(t, s, page);
        }
        if (s->moved) {
            if (t->last_report_t > 0) {
                double dt = (now - t->last_report_t) * 1000.0;
                if (t->win_ndt == 0 || dt < t->win_min_dt) t->win_min_dt = dt;
                if (dt > t->win_max_dt) t->win_max_dt = dt;
                t->win_sum_dt += dt; t->win_ndt++;
            }
            t->last_report_t = now;
        }
        s->moved = 0;
    }
    t->win_reports++;
    if (now - t->t_window >= SUMMARY_S) {
        if (t->win_events)
            rx_log("touch: %.0f ev/s, %ld reports, %d finger(s), move interval min %.1f avg %.1f max %.1f ms, page %d",
                   t->win_events / (now - t->t_window), t->win_reports, fingers, t->win_ndt ? t->win_min_dt : 0,
                   t->win_ndt ? t->win_sum_dt / t->win_ndt : 0, t->win_max_dt, page);
        t->t_window = now; t->win_events = 0; t->win_reports = 0; t->win_ndt = 0; t->win_min_dt = t->win_max_dt = t->win_sum_dt = 0;
    }
}

static void handle_event(rx_touch_t *t, const struct input_event *ev) {
    t->events++; t->win_events++;
    if (t->logged < LOG_FULL_EVENTS) {
        t->logged++;
        rx_log("touch: ev #%ld type %d code %d value %d", t->events, ev->type, ev->code, ev->value);
        if (t->logged == LOG_FULL_EVENTS) rx_log("touch: (first %d events logged in full; summaries every %.0f s from here)", LOG_FULL_EVENTS, SUMMARY_S);
    }
    if (ev->type == EV_SYN) { if (ev->code == SYN_REPORT) sync_report(t); return; }
    if (ev->type == EV_ABS) {
        if (t->mt) {
            if (ev->code == ABS_MT_SLOT) { t->cur_slot = ev->value >= 0 && ev->value < TOUCH_SLOTS ? ev->value : TOUCH_SLOTS - 1; return; }
            slot_t *s = &t->slot[t->cur_slot];
            if (ev->code == ABS_MT_TRACKING_ID) {
                if (ev->value >= 0 && !s->down) { s->down = 1; s->tracking_id = ev->value; s->moved = 2; s->kind = 0; }
                else if (ev->value < 0) s->tracking_id = -1;      /* up: finished in sync_report */
                return;
            }
            if (ev->code == ABS_MT_POSITION_X) { s->raw_x = ev->value; if (s->moved != 2) s->moved = 1; }
            else if (ev->code == ABS_MT_POSITION_Y) { s->raw_y = ev->value; if (s->moved != 2) s->moved = 1; }
        } else {
            slot_t *s = &t->slot[0];
            if (ev->code == ABS_X) { s->raw_x = ev->value; if (s->moved != 2) s->moved = 1; }
            else if (ev->code == ABS_Y) { s->raw_y = ev->value; if (s->moved != 2) s->moved = 1; }
        }
        return;
    }
    if (ev->type == EV_KEY && ev->code == BTN_TOUCH && !t->mt) {
        slot_t *s = &t->slot[0];
        if (ev->value && !s->down) { s->down = 1; s->tracking_id = 1; s->moved = 2; s->kind = 0; }
        else if (!ev->value && s->down) s->tracking_id = -1;
    }
}

static void *reader_main(void *arg) {
    rx_touch_t *t = (rx_touch_t *)arg;
    struct input_event evs[64];
    t->t_window = now_s();
    while (atomic_load(&t->running)) {
        struct pollfd p = { t->fd, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; rx_log("touch: poll: %s", strerror(errno)); break; }
        if (r == 0) { sync_report(t); continue; }     /* idle: still emit the periodic summary */
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { rx_log("touch: device went away"); break; }
        ssize_t n = read(t->fd, evs, sizeof evs);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; rx_log("touch: read: %s", strerror(errno)); break; }
        for (int i = 0; i < (int)(n / sizeof evs[0]); i++) handle_event(t, &evs[i]);
    }
    for (int i = 0; i < TOUCH_SLOTS; i++) if (t->slot[i].down) finger_up(t, &t->slot[i]);   /* release anything held */
    t->available = 0;
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ discovery */
#define NBITS(x) (((x) + 7) / 8)
static int bit_set(const unsigned char *bits, int b) { return (bits[b / 8] >> (b % 8)) & 1; }

static int probe_device(rx_touch_t *t, const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { rx_log("touch: %s: %s", path, strerror(errno)); return 0; }
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    unsigned char ev_bits[NBITS(EV_MAX + 1)] = {0}, abs_bits[NBITS(ABS_MAX + 1)] = {0}, key_bits[NBITS(KEY_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev_bits), ev_bits);
    int has_abs = bit_set(ev_bits, EV_ABS), has_key = bit_set(ev_bits, EV_KEY);
    if (has_abs) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits);
    if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits);
    int mt = has_abs && bit_set(abs_bits, ABS_MT_POSITION_X) && bit_set(abs_bits, ABS_MT_POSITION_Y);
    int mt_b = mt && bit_set(abs_bits, ABS_MT_SLOT) && bit_set(abs_bits, ABS_MT_TRACKING_ID);
    int st = has_abs && bit_set(abs_bits, ABS_X) && bit_set(abs_bits, ABS_Y) && has_key && bit_set(key_bits, BTN_TOUCH);
    char ranges[256] = "";
    struct input_absinfo ai;
    int codes[4] = { ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
    const char *cn[4] = { "X", "Y", "MT_X", "MT_Y" };
    int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (int i = 0; i < 4; i++) {
        if (!has_abs || !bit_set(abs_bits, codes[i])) continue;
        if (ioctl(fd, EVIOCGABS(codes[i]), &ai) == 0) {
            size_t l = strlen(ranges);
            snprintf(ranges + l, sizeof ranges - l, " %s=%d..%d", cn[i], ai.minimum, ai.maximum);
            if (i == (mt ? 2 : 0)) { xmin = ai.minimum; xmax = ai.maximum; }
            if (i == (mt ? 3 : 1)) { ymin = ai.minimum; ymax = ai.maximum; }
        }
    }
    rx_log("touch: %s \"%s\": EV_ABS=%d EV_KEY=%d MT=%d (type B=%d) single-touch=%d%s", path, name, has_abs, has_key, mt, mt_b, st, ranges[0] ? ranges : " (no abs axes)");
    if (!(mt || st)) { close(fd); return 0; }
    /* the grab test: EBUSY means someone (MPC) holds it exclusively and we would never see events */
    if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        if (errno == EBUSY) rx_log("touch: %s is grabbed by MPC: no direct touch", path);
        else rx_log("touch: %s EVIOCGRAB test failed: %s (trying to read anyway)", path, strerror(errno));
        if (errno == EBUSY) { close(fd); return 0; }
    } else {
        ioctl(fd, EVIOCGRAB, (void *)0);        /* release at once: we read alongside MPC, never instead of it */
        rx_log("touch: %s grab test ok, released (MPC keeps its touch)", path);
    }
    t->fd = fd;
    t->mt = mt;
    t->ax_min = xmin; t->ax_max = xmax; t->ay_min = ymin; t->ay_max = ymax;
    snprintf(t->dev_name, sizeof t->dev_name, "%s", name);
    snprintf(t->dev_path, sizeof t->dev_path, "%s", path);
    if (mt && !mt_b) rx_log("touch: %s: multitouch without slots (type A): tracked as one finger per report", path);
    return 1;
}

rx_touch_t *touch_start(const char *plugin_dir, rx_overlay_t *o, rx_engine_t *e) {
    (void)plugin_dir;
    rx_touch_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->overlay = o;
    t->engine = e;
    t->fd = -1;
    pthread_mutex_init(&t->geom_lock, NULL);
    default_geometry(t);
    for (int i = 0; i < TOUCH_SLOTS; i++) { t->slot[i].tracking_id = -1; t->slot[i].hold_ctl = CTL_NONE; t->slot[i].knob_ctl = CTL_NONE; t->slot[i].tempo_ctl = CTL_NONE; t->slot[i].part_idx = -1; }
    atomic_store(&t->wheel_deck[0], 0);
    atomic_store(&t->wheel_deck[1], 1);
    /* the controls we read and post */
    t->wheel_l_ctl = ctl_lookup("[reMixXXL]", "wheel_deck_l");
    t->wheel_r_ctl = ctl_lookup("[reMixXXL]", "wheel_deck_r");
    t->brow_ctl = ctl_lookup("[reMixXXL]", "browse_row");   /* BROWSE: a drag over the list scrolls it a row per row height */
    for (int d = 0; d < RX_DECKS; d++) {
        char g[24];
        snprintf(g, sizeof g, "[Channel%d]", d + 1);
        t->vinyl_ctl[d] = ctl_lookup(g, "vinyl_mode");
        t->feel_ctl[d] = ctl_lookup(g, "jog_feel");
        t->zone_ctl[d] = ctl_lookup(g, "touch_zone");
        if (t->zone_ctl[d] == CTL_NONE) t->zone_ctl[d] = ctl_register(g, "touch_zone", CK_INT, 0, 2, 0);   /* the view hint */
    }
    /* scan /dev/input/event0..31 */
    int found = 0, seen = 0;
    for (int i = 0; i < 32 && !found; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (access(path, F_OK) != 0) continue;
        seen++;
        found = probe_device(t, path);
    }
    if (!found) {
        rx_log("touch: no usable touchscreen among %d input device(s): touch_available()=0 (the wheel pages need MPC's own "
               "touch forwarding, which does not reach plugins)", seen);
        return t;
    }
    t->available = 1;
    atomic_store(&t->running, 1);
    if (pthread_create(&t->thread, NULL, reader_main, t) != 0) {
        rx_log("touch: cannot start the reader thread: %s", strerror(errno));
        atomic_store(&t->running, 0);
        t->available = 0;
        close(t->fd); t->fd = -1;
        return t;
    }
    rx_log("touch: reading %s \"%s\" (%s), ranges X %d..%d Y %d..%d -> display %dx%d", t->dev_path, t->dev_name, t->mt ? "multitouch" : "single-touch",
           t->ax_min, t->ax_max, t->ay_min, t->ay_max, DISPLAY_W, DISPLAY_H);
    return t;
}

void touch_stop(rx_touch_t *t) {
    if (!t) return;
    if (atomic_exchange(&t->running, 0)) pthread_join(t->thread, NULL);
    if (t->fd >= 0) close(t->fd);
    pthread_mutex_destroy(&t->geom_lock);
    rx_log("touch: stopped after %ld events", t->events);
    free(t);
}

int touch_available(rx_touch_t *t) { return t ? t->available : 0; }
void touch_status(rx_touch_t *t, char *buf, int n) {
    if (!t) { snprintf(buf, (size_t)n, "TOUCH: not started"); return; }
    if (!t->available) { snprintf(buf, (size_t)n, "TOUCH: no device (%s)", t->dev_name[0] ? t->dev_name : "none usable"); return; }
    int down = 0; for (int i = 0; i < TOUCH_SLOTS; i++) down += t->slot[i].down;
    snprintf(buf, (size_t)n, "TOUCH %s ev %ld dn %d pg %d", t->dev_path + 11, t->events, down, t->overlay ? overlay_page_shown(t->overlay) : -1);
}
