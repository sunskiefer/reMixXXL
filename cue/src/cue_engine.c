/* reMixXXL CUE: an audio effect that plays reMixXXL's cue bus (the decks with PFL on): decks 1/3 on the left, 2/4 on the right.
 * Put it on an audio track's effect slot and route that track to the Force outputs you want for the headphones (for example 3/4).
 * The cue audio is read from reMixXXL's own library (rx_cue_read, found with dlopen(RTLD_NOLOAD)); the track's own input is replaced.
 * Without reMixXXL on a track it plays silence. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "engine.h"
#include "params.h"

typedef int (*cue_read_fn)(float *out_lr, int frames);

typedef struct {
    _Atomic(cue_read_fn) fn;
    float level;
    _Atomic int stop;
    pthread_t th;
    int have_th;
} cue_t;

/* find reMixXXL's loaded library through /proc/self/maps (never loads it: RTLD_NOLOAD); retried once a second until found */
static cue_read_fn find_reader(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return NULL;
    char line[1024], path[1024] = "";
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        size_t n = strlen(p); while (n && (p[n - 1] == '\n' || p[n - 1] == ' ')) p[--n] = 0;
        const char *b = strrchr(p, '/'); b = b ? b + 1 : p;
        if (!strcmp(b, "remixxxl.so")) { snprintf(path, sizeof path, "%s", p); break; }
    }
    fclose(f);
    if (!path[0]) return NULL;
    void *h = dlopen(path, RTLD_NOLOAD | RTLD_LAZY);
    return h ? (cue_read_fn)dlsym(h, "rx_cue_read") : NULL;
}

static void *finder(void *a) {
    cue_t *c = a;
    while (!atomic_load(&c->stop)) {
        cue_read_fn fn = find_reader();
        if (fn) { atomic_store(&c->fn, fn); return NULL; }
        for (int i = 0; i < 10 && !atomic_load(&c->stop); i++) usleep(100000);
    }
    return NULL;
}

static void *create(const char *dir) {
    (void)dir;
    cue_t *c = calloc(1, sizeof *c);
    c->level = 0.8f;
    if (pthread_create(&c->th, NULL, finder, c) == 0) c->have_th = 1;
    return c;
}
static void destroy(void *p) {
    cue_t *c = p;
    atomic_store(&c->stop, 1);
    if (c->have_th) pthread_join(c->th, NULL);
    free(c);
}
static void midi(void *p, const uint8_t *m, int n) { (void)p; (void)m; (void)n; }
static void set_param(void *p, const char *key, const char *val) {
    cue_t *c = p;
    if (!strcmp(key, "level")) { float v = (float)atof(val); c->level = v < 0 ? 0 : v > 1 ? 1 : v; }
}
static int get_param(void *p, const char *key, char *buf, int len) {
    cue_t *c = p;
    if (!strcmp(key, "level")) return snprintf(buf, (size_t)len, "%g", c->level) + 1;
    return 0;
}
static void render(void *p, int16_t *out, int frames) { (void)p; memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); }
static void process(void *p, const int16_t *in, int16_t *out, int frames) {
    cue_t *c = p; (void)in;
    float tmp[128 * 2];
    cue_read_fn fn = atomic_load(&c->fn);
    int n = frames > 128 ? 128 : frames;
    if (!fn || !fn(tmp, n)) { memset(out, 0, sizeof(int16_t) * 2 * (size_t)frames); return; }
    float g = c->level * c->level * 32767.0f * 2.0f;          /* 0..1 knob, squared: 0.8 is about unity */
    for (int k = 0; k < n * 2; k++) { float s = tmp[k] * g; out[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); }
    for (int k = n * 2; k < frames * 2; k++) out[k] = 0;
}
static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, process};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
