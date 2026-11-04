/* reMixXXL test stub for control.c, deck scratch/jog, the overlay and the engine instance: enough for test_midi.c to
 * drive midi.c / midi_js.c / midi_xml.c natively. Controls are registered on demand by ctl_lookup (so any Mixxx name
 * the mapping or the scripts use exists), posts are applied immediately (ctl_drain fires the listeners), and the
 * deck calls are recorded in counters the test reads. The real control.c is written by another agent. */
#define _GNU_SOURCE
#include "remix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAXC 4096
typedef struct { char group[48], name[64]; int kind; double lo, hi, v; } c_t;
static c_t C[MAXC];
static int NC;
typedef struct { int used; ctl_id id; ctl_listener_fn fn; void *user; } l_t;
static l_t L[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int auto_register = 1;

/* the stub's extras for the test */
int stub_scratch_enable[RX_DECKS], stub_scratch_calls[RX_DECKS];
double stub_scratch_ticks[RX_DECKS], stub_jog[RX_DECKS];
void stub_ctl_auto(int on) { auto_register = on; }

static void fire(ctl_id id, double v) {
    for (int i = 0; i < 1024; i++) if (L[i].used && L[i].id == id) L[i].fn(id, v, L[i].user);
}

ctl_id ctl_register(const char *group, const char *name, int kind, double lo, double hi, double def) {
    for (int i = 0; i < NC; i++) if (!strcmp(C[i].group, group) && !strcmp(C[i].name, name)) return i;
    if (NC >= MAXC) return CTL_NONE;
    snprintf(C[NC].group, 48, "%s", group);
    snprintf(C[NC].name, 64, "%s", name);
    C[NC].kind = kind; C[NC].lo = lo; C[NC].hi = hi; C[NC].v = def;
    return NC++;
}

/* Mixxx-like kinds/ranges for the names the MC3000 mapping uses; anything else is a float 0..1 */
static ctl_id auto_reg(const char *group, const char *name) {
    if (!auto_register) return CTL_NONE;
    if (!strcmp(name, "play") || !strcmp(name, "keylock") || !strcmp(name, "pfl") || !strcmp(name, "quantize") || !strncmp(name, "filter", 6) ||
        !strcmp(name, "slip_enabled") || !strcmp(name, "reverse") || !strcmp(name, "vinyl_mode") || !strcmp(name, "sync_master") || !strcmp(name, "loop_enabled") ||
        (strstr(name, "_enabled") != NULL))
        return ctl_register(group, name, CK_TOGGLE, 0, 1, 0);
    if (!strcmp(name, "cue_default") || !strcmp(name, "beatsync") || strstr(name, "_activate") || !strcmp(name, "loop_in") || !strcmp(name, "loop_out") ||
        !strcmp(name, "reloop_exit") || !strcmp(name, "reloop_toggle") || !strcmp(name, "back") || !strcmp(name, "fwd") || !strcmp(name, "rate_temp_up") ||
        !strcmp(name, "rate_temp_down") || !strncmp(name, "Select", 6) || !strncmp(name, "Load", 4) || !strcmp(name, "loop_halve") || !strcmp(name, "loop_double") ||
        strstr(name, "_clear") || !strcmp(name, "beats_translate_curpos") || !strcmp(name, "rate_set_default") || !strcmp(name, "key_sync") || !strcmp(name, "rateRange_next"))
        return ctl_register(group, name, CK_TRIGGER, 0, 1, 0);
    if (!strcmp(name, "rate") || !strcmp(name, "jog") || !strcmp(name, "crossfader") || !strcmp(name, "balance") || !strcmp(name, "headMix"))
        return ctl_register(group, name, CK_FLOAT, -1, 1, 0);
    if (!strcmp(name, "pregain") || !strcmp(name, "volume") || !strcmp(name, "headVolume"))
        return ctl_register(group, name, CK_FLOAT, 0, 1, 1);
    if (!strcmp(name, "loop_start_position") || !strcmp(name, "loop_end_position") || !strcmp(name, "cue_point") || strstr(name, "_position"))
        return ctl_register(group, name, CK_FLOAT, -1, 1e12, -1);
    if (!strcmp(name, "scratch2_enable")) return ctl_register(group, name, CK_TOGGLE, 0, 1, 0);
    return ctl_register(group, name, CK_FLOAT, 0, 1, 0);
}

ctl_id ctl_lookup(const char *group, const char *name) {
    if (!group || !name || !name[0]) return CTL_NONE;
    for (int i = 0; i < NC; i++) if (!strcmp(C[i].group, group) && !strcmp(C[i].name, name)) return i;
    return auto_reg(group, name);
}

ctl_id ctl_lookup_full(const char *gn) {
    char g[48], n[64];
    const char *sep = strpbrk(gn, ".,");
    if (!sep) return CTL_NONE;
    snprintf(g, sizeof g, "%.*s", (int)(sep - gn), gn);
    snprintf(n, sizeof n, "%s", sep + 1);
    return ctl_lookup(g, n);
}

const char *ctl_group(ctl_id id) { return id >= 0 && id < NC ? C[id].group : ""; }
const char *ctl_name(ctl_id id) { return id >= 0 && id < NC ? C[id].name : ""; }
int ctl_count(void) { return NC; }
double ctl_get(ctl_id id) { return id >= 0 && id < NC ? C[id].v : 0; }
void ctl_set(ctl_id id, double v) { if (id >= 0 && id < NC) { C[id].v = v; fire(id, v); } }
void ctl_post(ctl_id id, double v) { pthread_mutex_lock(&lock); ctl_set(id, v); pthread_mutex_unlock(&lock); }
void ctl_post_delta(ctl_id id, double dv) { pthread_mutex_lock(&lock); if (id >= 0 && id < NC) ctl_set(id, C[id].v + dv); pthread_mutex_unlock(&lock); }
int ctl_add_listener(ctl_id id, ctl_listener_fn fn, void *user) {
    for (int i = 0; i < 1024; i++) if (!L[i].used) { L[i].used = 1; L[i].id = id; L[i].fn = fn; L[i].user = user; return i; }
    return -1;
}
void ctl_remove_listener(int h) { if (h >= 0 && h < 1024) L[h].used = 0; }
void ctl_drain(void) {}
int ctl_kind(ctl_id id) { return id >= 0 && id < NC ? C[id].kind : CK_FLOAT; }
double ctl_min(ctl_id id) { return id >= 0 && id < NC ? C[id].lo : 0; }
double ctl_max(ctl_id id) { return id >= 0 && id < NC ? C[id].hi : 1; }
void ctl_set_text(ctl_id id, const char *t) { (void)id; (void)t; }
int ctl_text(ctl_id id, char *b, int n) { (void)id; if (n) b[0] = 0; return 0; }
void ctl_init(void) {}
void ctl_shutdown(void) {}

/* decks */
struct rx_deck { int index; };
static struct rx_deck decks[RX_DECKS] = {{0}, {1}, {2}, {3}};
struct rx_engine { int dummy; };
static struct rx_engine eng;
rx_deck_t *rx_deck(rx_engine_t *e, int i) { (void)e; return i >= 0 && i < RX_DECKS ? &decks[i] : NULL; }
rx_engine_t *rx_engine_instance(void) { return &eng; }
int deck_index(rx_deck_t *d) { return d->index; }
void deck_scratch_enable(rx_deck_t *d, int enable, double rpm, double alpha, double beta) {
    (void)rpm; (void)alpha; (void)beta;
    stub_scratch_enable[d->index] = enable; stub_scratch_calls[d->index]++;
    char g[24]; snprintf(g, sizeof g, "[Channel%d]", d->index + 1);
    ctl_set(ctl_lookup(g, "scratch2_enable"), enable);
}
void deck_scratch_tick(rx_deck_t *d, double ticks) { stub_scratch_ticks[d->index] += ticks; }
void deck_jog(rx_deck_t *d, double amount) { stub_jog[d->index] += amount; }

/* overlay (touch.c links against these; the test does not start touch) */
struct rx_overlay { int dummy; };
int overlay_page_shown(rx_overlay_t *o) { (void)o; return -1; }
int overlay_box_origin(rx_overlay_t *o, int *x, int *y) { (void)o; *x = 0; *y = 0; return 0; }
