/* reMixXXL -- compat.c: the Mixxx control names that controller maps still use but the plugin did not have.
 *
 * Many Mixxx maps (the Denon MC3000 one is from Mixxx 1.10) talk to controls by their older or shorter names. Each one that
 * was missing was a control that did nothing; this file gives them to the plugin as aliases of the real ones, so those maps
 * work the way they do in Mixxx.
 *   [ChannelN] filterHigh / filterMid / filterLow (+Kill)   the channel's three-band EQ (parameter3/2/1, button_parameter3/2/1)
 *   [Master] headVolume                                     the headphone gain (headGain)
 *   [ChannelN] flanger                                      the effect unit 1 switch of the channel
 *   [Flanger] lfoDepth / lfoDelay / lfoPeriod               accepted, no effect (the old flanger's knobs)
 *   [ChannelN] beatloop_X, beatloop_X_enabled               activate a beat loop of X beats / that loop is on
 *   [ChannelN] reloop_exit                                  the loop on/off
 *   [ChannelN] beats_translate_curpos                       accepted, no effect yet
 *   [Playlist] SelectNextTrack, SelectPrevTrack, SelectTrackKnob, SelectNextPlaylist, SelectPrevPlaylist, LoadSelectedIntoFirstStopped
 *                                                           the BROWSE list: a row down / up, a page forward / back, load into the first stopped deck
 * Runs on the engine thread (render, before engine_actions): ctl_get / ctl_set only. */
#include "remix.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { ctl_id a, b; double la, lb; int init; } alias_t;
static alias_t al[48]; static int nal;
static ctl_id c_next_track, c_prev_track, c_knob, c_next_pl, c_prev_pl, c_load_stopped;
static ctl_id c_browse_row, c_page_next, c_page_prev, c_load[RX_DECKS], c_play[RX_DECKS], c_loop_on[RX_DECKS], c_bl_size[RX_DECKS], c_reloop[RX_DECKS], c_reloop_exit[RX_DECKS];
#define BL_N 10
static const double BL[BL_N] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};
static ctl_id c_bl_act[RX_DECKS][BL_N], c_bl_set[RX_DECKS][BL_N], c_bl_en[RX_DECKS][BL_N];

static void add_alias(ctl_id a, ctl_id b) {
    if (a == CTL_NONE || b == CTL_NONE || nal >= (int)(sizeof al / sizeof al[0])) return;
    al[nal++] = (alias_t){a, b, 0, 0, 0};
}

void compat_init(void) {
    static int done;
    if (done) return;
    done = 1;
    char g[64], n[64], m[64];
    for (int d = 0; d < RX_DECKS; d++) {
        snprintf(g, sizeof g, "[Channel%d]", d + 1);
        snprintf(m, sizeof m, "[EqualizerRack1_[Channel%d]_Effect1]", d + 1);
        static const char *const band[3] = {"filterLow", "filterMid", "filterHigh"};
        for (int b = 0; b < 3; b++) {
            snprintf(n, sizeof n, "parameter%d", b + 1);
            add_alias(ctl_register(g, band[b], CK_FLOAT, 0, 4, 1), ctl_lookup(m, n));
            char k[32]; snprintf(k, sizeof k, "%sKill", band[b]);
            snprintf(n, sizeof n, "button_parameter%d", b + 1);
            add_alias(ctl_register(g, k, CK_TOGGLE, 0, 1, 0), ctl_lookup(m, n));
        }
        snprintf(m, sizeof m, "group_[Channel%d]_enable", d + 1);
        add_alias(ctl_register(g, "flanger", CK_TOGGLE, 0, 1, 0), ctl_lookup("[EffectRack1_EffectUnit1]", m));
        ctl_register(g, "beats_translate_curpos", CK_TRIGGER, 0, 1, 0);
        c_reloop_exit[d] = ctl_register(g, "reloop_exit", CK_TRIGGER, 0, 1, 0);
        c_reloop[d] = ctl_lookup(g, "reloop_toggle");
        c_load[d] = ctl_register(g, "LoadSelectedTrack", CK_TRIGGER, 0, 1, 0);
        c_play[d] = ctl_lookup(g, "play");
        c_loop_on[d] = ctl_lookup(g, "loop_enabled");
        c_bl_size[d] = ctl_lookup(g, "beatloop_size");
        for (int i = 0; i < BL_N; i++) {
            snprintf(n, sizeof n, "beatloop_%g_activate", BL[i]); c_bl_act[d][i] = ctl_lookup(g, n);
            snprintf(n, sizeof n, "beatloop_%g", BL[i]); c_bl_set[d][i] = ctl_register(g, n, CK_TRIGGER, 0, 1, 0);
            snprintf(n, sizeof n, "beatloop_%g_enabled", BL[i]); c_bl_en[d][i] = ctl_register(g, n, CK_TOGGLE, 0, 1, 0);
        }
    }
    add_alias(ctl_register("[Master]", "headVolume", CK_FLOAT, 0, 4, 1), ctl_lookup("[Master]", "headGain"));
    ctl_register("[Flanger]", "lfoDepth", CK_FLOAT, 0, 1, 0.5);
    ctl_register("[Flanger]", "lfoDelay", CK_FLOAT, 50, 10000, 50);
    ctl_register("[Flanger]", "lfoPeriod", CK_FLOAT, 50000, 2000000, 1000000);
    c_next_track = ctl_register("[Playlist]", "SelectNextTrack", CK_TRIGGER, 0, 1, 0);
    c_prev_track = ctl_register("[Playlist]", "SelectPrevTrack", CK_TRIGGER, 0, 1, 0);
    c_knob = ctl_register("[Playlist]", "SelectTrackKnob", CK_FLOAT, -127, 127, 0);
    c_next_pl = ctl_register("[Playlist]", "SelectNextPlaylist", CK_TRIGGER, 0, 1, 0);
    c_prev_pl = ctl_register("[Playlist]", "SelectPrevPlaylist", CK_TRIGGER, 0, 1, 0);
    c_load_stopped = ctl_register("[Playlist]", "LoadSelectedIntoFirstStopped", CK_TRIGGER, 0, 1, 0);
    c_browse_row = ctl_lookup("[reMixXXL]", "browse_row");
    c_page_next = ctl_lookup("[reMixXXL]", "browse_page_next");
    c_page_prev = ctl_lookup("[reMixXXL]", "browse_page_prev");
    rx_log("compat: %d Mixxx control aliases", nal);
}

static int fired(ctl_id c) { if (c == CTL_NONE || ctl_get(c) < 0.5) return 0; ctl_set(c, 0); return 1; }

void compat_tick(void) {
    for (int i = 0; i < nal; i++) {
        alias_t *x = &al[i];
        double va = ctl_get(x->a), vb = ctl_get(x->b);
        if (!x->init) { x->init = 1; x->la = va; x->lb = vb; if (va != vb) { ctl_set(x->a, vb); x->la = vb; } continue; }
        if (va != x->la) { ctl_set(x->b, va); x->la = va; x->lb = va; }
        else if (vb != x->lb) { ctl_set(x->a, vb); x->la = vb; x->lb = vb; }
    }
    for (int d = 0; d < RX_DECKS; d++) {
        if (fired(c_reloop_exit[d]) && c_reloop[d] != CTL_NONE) ctl_set(c_reloop[d], 1);
        double sz = c_bl_size[d] != CTL_NONE ? ctl_get(c_bl_size[d]) : 4;
        int on = c_loop_on[d] != CTL_NONE && ctl_get(c_loop_on[d]) >= 0.5;
        for (int i = 0; i < BL_N; i++) {
            if (fired(c_bl_set[d][i]) && c_bl_act[d][i] != CTL_NONE) ctl_set(c_bl_act[d][i], 1);
            if (c_bl_en[d][i] != CTL_NONE) { double e = on && fabs(sz - BL[i]) < 1e-6; if (ctl_get(c_bl_en[d][i]) != e) ctl_set(c_bl_en[d][i], e); }
        }
    }
    if (c_browse_row != CTL_NONE) {
        if (fired(c_next_track)) ctl_set(c_browse_row, 7);          /* the list cursor: 0 / 7 = one entry up / down (engine.c) */
        else if (fired(c_prev_track)) ctl_set(c_browse_row, 0);
        else if (c_knob != CTL_NONE && ctl_get(c_knob) != 0) { double k = ctl_get(c_knob); ctl_set(c_knob, 0); ctl_set(c_browse_row, k > 0 ? 7 : 0); }
    }
    if (fired(c_next_pl) && c_page_next != CTL_NONE) ctl_set(c_page_next, 1);
    if (fired(c_prev_pl) && c_page_prev != CTL_NONE) ctl_set(c_page_prev, 1);
    if (fired(c_load_stopped)) {
        for (int d = 0; d < RX_DECKS; d++)
            if (c_play[d] != CTL_NONE && ctl_get(c_play[d]) < 0.5) { if (c_load[d] != CTL_NONE) ctl_set(c_load[d], 1); break; }
    }
}
