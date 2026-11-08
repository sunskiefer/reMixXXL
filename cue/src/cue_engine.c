/* reMixXXL CUE: an audio effect that plays reMixXXL's cue bus (the decks with PFL on): decks 1/3 on the left, 2/4 on the right.
 * Put it on an audio track's effect slot and route that track to the Force outputs you want for the headphones (for example 3/4).
 * The cue audio is read from a ring in a memory-mapped file that reMixXXL writes (cue_shm.h); the track's own input is replaced.
 * Without reMixXXL on a track it plays silence. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "engine.h"
#include "params.h"
#include "cue_shm.h"

typedef struct {
    cue_shm_t *shm;
    unsigned r;
    int primed, tries;
    float level;
    volatile int state, quiet, stalled;
    unsigned prev_w;
} cue_t;

static void *create(const char *dir) {
    (void)dir;
    cue_t *c = calloc(1, sizeof *c);
    c->level = 0.8f;
    c->shm = cue_shm_open();
    return c;
}
static void destroy(void *p) { free(p); }
static void midi(void *p, const uint8_t *m, int n) { (void)p; (void)m; (void)n; }
static void set_param(void *p, const char *key, const char *val) {
    cue_t *c = p;
    if (!strcmp(key, "level")) { float v = (float)atof(val); c->level = v < 0 ? 0 : v > 1 ? 1 : v; }
}
static int get_param(void *p, const char *key, char *buf, int len) {
    cue_t *c = p;
    if (!strcmp(key, "level")) return snprintf(buf, (size_t)len, "%g", c->level) + 1;
    if (!strcmp(key, "status")) {
        static const char *const T[] = {"WAITING: EFFECT NOT RUN YET", "NO RING FILE (/dev/shm, /tmp)", "reMixXXL NOT RUNNING",
            "reMixXXL STOPPED", "NO PFL ON (SILENT)", "PLAYING THE CUE", "RING UNDERRUN"};
        int s = c->state; if (s < 0 || s > 6) s = 0;
        return snprintf(buf, (size_t)len, "%s", T[s]) + 1;
    }
    return 0;
}
static void render(void *p, int16_t *out, int frames) { (void)p; memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); }
static void process(void *p, const int16_t *in, int16_t *out, int frames) {
    cue_t *c = p; (void)in;
    float tmp[128 * 2];
    if (!c->shm && (c->tries++ & 63) == 0) c->shm = cue_shm_open();
    int n = frames > 128 ? 128 : frames;
    if (!c->shm) { c->state = 1; memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); return; }
    unsigned w = atomic_load_explicit(&c->shm->w, memory_order_relaxed);
    if (w == 0) c->state = 2;
    else if (w == c->prev_w) { if (++c->stalled > 400) c->state = 3; }
    else c->stalled = 0;
    c->prev_w = w;
    if (!cue_shm_read(c->shm, &c->r, &c->primed, tmp, n)) {
        if (w != 0 && c->stalled <= 400) c->state = 6;
        memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); return;
    }
    float g = c->level * c->level * 32767.0f * 2.0f;          /* 0..1 knob, squared: 0.8 is about unity */
    float pk = 0;
    for (int k = 0; k < n * 2; k++) { float a = tmp[k] < 0 ? -tmp[k] : tmp[k]; if (a > pk) pk = a; float s = tmp[k] * g; out[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); }
    for (int k = n * 2; k < frames * 2; k++) out[k] = 0;
    if (pk > 1e-5f) { c->quiet = 0; if (c->stalled <= 400) c->state = 5; }
    else if (++c->quiet > 40 && c->stalled <= 400) c->state = 4;
}
static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, process};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
