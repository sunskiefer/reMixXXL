/* reMixXXL -- control.c: the control table (Mixxx's [group],name ControlObjects, see remix.h).
 *
 * What it is
 *   A fixed table of CTL_CAP controls, each with a group ("[Channel1]"), a name ("play"), a kind, a range and a
 *   value.  Mixxx's ControlObject/ControlProxy pattern (src/control/controlobject.h) is the model: every part of
 *   the plugin talks to the others by reading and writing named controls, and listeners get called when a value
 *   changed.  The JS API (engine.getValue / engine.setValue / engine.connectControl) and MIDI learn map straight
 *   onto this table.
 *
 * Threads (the reason for the design)
 *   - render()/midi()/set_param() (the "engine threads", serialised by the wrapper's lock) use ctl_set() and
 *     ctl_drain() directly.  ctl_drain() runs on the AUDIO THREAD: it must never block and never allocate.
 *   - every other thread (touch, the QuickJS timers, the screen) uses ctl_post()/ctl_post_delta(), which queue the
 *     write on a ring guarded by a plain mutex, and ctl_get(), which is a lock-free atomic read.
 *   - ctl_drain() takes the ring mutex with pthread_mutex_trylock(): if a poster holds it right now the drain is
 *     simply skipped for this block (the posts stay queued, 128 frames = 2.9 ms later they are applied).  So the
 *     audio thread never waits on anybody.
 *
 * Values
 *   A control's value is a double stored in a uint64_t that is read and written with __atomic builtins.  ARMv7-A
 *   has 64-bit exclusive load/store (ldrexd/strexd), so clang inlines these without libatomic, and a reader can
 *   never see a torn double.  No seqlock, no per-control mutex.
 *
 * Listeners
 *   Up to CTL_MAX_LISTENERS_PER_CTL per control, CTL_MAX_LISTENERS in all.  They fire from ctl_drain() (the audio
 *   thread) for every control whose value differs from the value it was last notified with, after the posts have
 *   been applied.  That covers ctl_set() from the engine as well as posts, exactly like Mixxx's valueChanged
 *   signals.  Listener registration takes the same mutex the ring uses, so a drain that races with a JS
 *   connectControl() is skipped, never torn.
 *
 * Text controls (CK_TEXT)
 *   Hold a CTL_TEXT_LEN string behind a small mutex (ctl_text copies it out; the screen reads titles that way). */

#include "remix.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CTL_CAP 2048                         /* controls */
#define CTL_HASH_CAP 4096                    /* open-addressing slots (2x the controls: short probe chains) */
#define CTL_GROUP_LEN 64                     /* "[EqualizerRack1_[Channel1]_Effect1]" is 36 */
#define CTL_NAME_LEN 64
#define CTL_TEXT_LEN 128
#define CTL_RING 4096                        /* queued posts (one block's worth of wheel ticks is a few hundred) */
#define CTL_MAX_LISTENERS 512
#define CTL_MAX_LISTENERS_PER_CTL 8

typedef struct {
    char group[CTL_GROUP_LEN];
    char name[CTL_NAME_LEN];
    int kind;
    double lo, hi, def;
    uint64_t value_bits;                     /* the double, atomically read/written (see ctl_get/ctl_set) */
    double last_notified;                    /* audio thread only: the value the listeners last saw */
    int16_t listeners[CTL_MAX_LISTENERS_PER_CTL];   /* handles into g_listeners, -1 = free */
    int text_index;                          /* CK_TEXT: slot in g_text, else -1 */
} ctl_t;

typedef struct { ctl_id id; double v; int delta; } ctl_post_t;

typedef struct { ctl_id id; ctl_listener_fn fn; void *user; int used; } ctl_listener_t;

#define CTL_TEXT_SLOTS 256                   /* text controls are few (titles, artists, browse rows, readouts) */

static struct {
    ctl_t ctl[CTL_CAP];
    int count;
    int16_t hash[CTL_HASH_CAP];              /* control index or -1 */
    pthread_mutex_t reg_lock;                /* registration (ctl_register) and the hash map writes */

    /* the post ring (producers: any thread, under ring_lock; consumer: ctl_drain under trylock) */
    ctl_post_t ring[CTL_RING];
    unsigned ring_head, ring_tail;           /* head = next to write, tail = next to read; both guarded */
    unsigned ring_dropped;                   /* posts dropped because the ring was full (logged once) */
    pthread_mutex_t ring_lock;

    ctl_listener_t listeners[CTL_MAX_LISTENERS];   /* guarded by ring_lock too (see the header comment) */

    char text[CTL_TEXT_SLOTS][CTL_TEXT_LEN];
    int text_count;
    pthread_mutex_t text_lock;
    int inited;
} g;

/* ------------------------------------------------------------------------------------------------ helpers */
static inline uint64_t dbl_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double bits_dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* FNV-1a over group + '\0' + name: group and name are hashed as one key, so "[Channel1]"+"play" never collides
 * with "[Channel1]p"+"lay" (the separator byte is part of the key). */
static uint32_t ctl_hash(const char *group, const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = group; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= 0; h *= 16777619u;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h;
}

static int ctl_valid(ctl_id id) { return id >= 0 && id < g.count; }

/* The hash map is only written inside reg_lock; readers (lookup) run lock-free because an entry, once written, is
 * never moved or removed, and g.count is only ever incremented after the entry is complete. */
static ctl_id ctl_find(const char *group, const char *name) {
    uint32_t h = ctl_hash(group, name);
    for (int probe = 0; probe < CTL_HASH_CAP; probe++) {
        unsigned slot = (h + (unsigned)probe) & (CTL_HASH_CAP - 1);
        int16_t idx = __atomic_load_n(&g.hash[slot], __ATOMIC_ACQUIRE);
        if (idx < 0) return CTL_NONE;
        if (strcmp(g.ctl[idx].group, group) == 0 && strcmp(g.ctl[idx].name, name) == 0) return idx;
    }
    return CTL_NONE;
}

/* ------------------------------------------------------------------------------------------------ init */
void ctl_init(void) {
    memset(&g, 0, sizeof g);
    for (int i = 0; i < CTL_HASH_CAP; i++) g.hash[i] = -1;
    for (int i = 0; i < CTL_CAP; i++) {
        g.ctl[i].text_index = -1;
        for (int l = 0; l < CTL_MAX_LISTENERS_PER_CTL; l++) g.ctl[i].listeners[l] = -1;
    }
    pthread_mutex_init(&g.reg_lock, NULL);
    pthread_mutex_init(&g.ring_lock, NULL);
    pthread_mutex_init(&g.text_lock, NULL);
    g.inited = 1;
}

void ctl_shutdown(void) {
    if (!g.inited) return;
    pthread_mutex_destroy(&g.reg_lock);
    pthread_mutex_destroy(&g.ring_lock);
    pthread_mutex_destroy(&g.text_lock);
    g.inited = 0;
    g.count = 0;
}

/* ------------------------------------------------------------------------------------------------ registration */
ctl_id ctl_register(const char *group, const char *name, int kind, double lo, double hi, double def) {
    if (!g.inited) ctl_init();
    if (!group || !name) return CTL_NONE;
    pthread_mutex_lock(&g.reg_lock);
    ctl_id id = ctl_find(group, name);
    if (id != CTL_NONE) {                    /* registered twice (deck.c and midi.c may both want it): same id */
        pthread_mutex_unlock(&g.reg_lock);
        return id;
    }
    if (g.count >= CTL_CAP) {
        pthread_mutex_unlock(&g.reg_lock);
        rx_log("control: table full (%d), cannot register %s,%s", CTL_CAP, group, name);
        return CTL_NONE;
    }
    id = g.count;
    ctl_t *c = &g.ctl[id];
    snprintf(c->group, sizeof c->group, "%s", group);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->kind = kind;
    c->lo = lo; c->hi = hi; c->def = def;
    c->last_notified = def;
    __atomic_store_n(&c->value_bits, dbl_bits(def), __ATOMIC_RELEASE);
    if (kind == CK_TEXT) {
        pthread_mutex_lock(&g.text_lock);
        if (g.text_count < CTL_TEXT_SLOTS) { c->text_index = g.text_count++; g.text[c->text_index][0] = 0; }
        pthread_mutex_unlock(&g.text_lock);
    }
    /* publish: the hash slot last, after the entry is complete, so a lock-free lookup never sees a half entry */
    uint32_t h = ctl_hash(group, name);
    for (int probe = 0; probe < CTL_HASH_CAP; probe++) {
        unsigned slot = (h + (unsigned)probe) & (CTL_HASH_CAP - 1);
        if (g.hash[slot] < 0) { __atomic_store_n(&g.hash[slot], (int16_t)id, __ATOMIC_RELEASE); break; }
    }
    __atomic_store_n(&g.count, id + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g.reg_lock);
    return id;
}

ctl_id ctl_lookup(const char *group, const char *name) {
    if (!g.inited || !group || !name) return CTL_NONE;
    return ctl_find(group, name);
}

/* "[Channel1].play", "[Channel1],play", also "[EqualizerRack1_[Channel1]_Effect1],parameter1": the group is the
 * bracketed prefix with nested brackets balanced; the separator after it is ',' or '.' (Mixxx's own JS uses the
 * comma form in connectControl's "[Channel1],play" shorthand; the dot form is what our MIDI-learn file writes). */
ctl_id ctl_lookup_full(const char *group_dot_name) {
    if (!group_dot_name) return CTL_NONE;
    const char *s = group_dot_name;
    while (*s == ' ') s++;
    if (*s != '[') return CTL_NONE;
    int depth = 0;
    const char *p = s;
    for (; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
    }
    if (depth != 0) return CTL_NONE;
    size_t glen = (size_t)(p - s);
    if (glen >= CTL_GROUP_LEN) return CTL_NONE;
    while (*p == ' ') p++;
    if (*p != ',' && *p != '.') return CTL_NONE;
    p++;
    while (*p == ' ') p++;
    char group[CTL_GROUP_LEN], name[CTL_NAME_LEN];
    memcpy(group, s, glen); group[glen] = 0;
    snprintf(name, sizeof name, "%s", p);
    /* trailing blanks off the name */
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') name[--n] = 0;
    return ctl_lookup(group, name);
}

const char *ctl_group(ctl_id id) { return ctl_valid(id) ? g.ctl[id].group : ""; }
const char *ctl_name(ctl_id id) { return ctl_valid(id) ? g.ctl[id].name : ""; }
int ctl_count(void) { return __atomic_load_n(&g.count, __ATOMIC_ACQUIRE); }
int ctl_kind(ctl_id id) { return ctl_valid(id) ? g.ctl[id].kind : CK_FLOAT; }
double ctl_min(ctl_id id) { return ctl_valid(id) ? g.ctl[id].lo : 0; }
double ctl_max(ctl_id id) { return ctl_valid(id) ? g.ctl[id].hi : 1; }

/* ------------------------------------------------------------------------------------------------ values */
double ctl_get(ctl_id id) {
    if (!ctl_valid(id)) return 0.0;
    return bits_dbl(__atomic_load_n(&g.ctl[id].value_bits, __ATOMIC_ACQUIRE));
}

void ctl_set(ctl_id id, double v) {
    if (!ctl_valid(id)) return;
    __atomic_store_n(&g.ctl[id].value_bits, dbl_bits(v), __ATOMIC_RELEASE);
}

static void ctl_enqueue(ctl_id id, double v, int delta) {
    if (!ctl_valid(id)) return;
    pthread_mutex_lock(&g.ring_lock);
    unsigned used = g.ring_head - g.ring_tail;
    if (used >= CTL_RING) {
        if (g.ring_dropped++ == 0) rx_log("control: post ring full, dropping (%s,%s)", g.ctl[id].group, g.ctl[id].name);
    } else {
        ctl_post_t *p = &g.ring[g.ring_head & (CTL_RING - 1)];
        p->id = id; p->v = v; p->delta = delta;
        g.ring_head++;
    }
    pthread_mutex_unlock(&g.ring_lock);
}

void ctl_post(ctl_id id, double v) { ctl_enqueue(id, v, 0); }
void ctl_post_delta(ctl_id id, double dv) { ctl_enqueue(id, dv, 1); }

/* ------------------------------------------------------------------------------------------------ listeners */
int ctl_add_listener(ctl_id id, ctl_listener_fn fn, void *user) {
    if (!ctl_valid(id) || !fn) return -1;
    pthread_mutex_lock(&g.ring_lock);
    int handle = -1;
    for (int i = 0; i < CTL_MAX_LISTENERS; i++) if (!g.listeners[i].used) { handle = i; break; }
    if (handle >= 0) {
        int slot = -1;
        for (int l = 0; l < CTL_MAX_LISTENERS_PER_CTL; l++) if (g.ctl[id].listeners[l] < 0) { slot = l; break; }
        if (slot < 0) handle = -1;           /* this control already has 8 listeners */
        else {
            g.listeners[handle].id = id; g.listeners[handle].fn = fn; g.listeners[handle].user = user;
            g.listeners[handle].used = 1;
            g.ctl[id].listeners[slot] = (int16_t)handle;
        }
    }
    pthread_mutex_unlock(&g.ring_lock);
    if (handle < 0) rx_log("control: no listener slot for %s,%s", g.ctl[id].group, g.ctl[id].name);
    return handle;
}

void ctl_remove_listener(int handle) {
    if (handle < 0 || handle >= CTL_MAX_LISTENERS) return;
    pthread_mutex_lock(&g.ring_lock);
    if (g.listeners[handle].used) {
        ctl_id id = g.listeners[handle].id;
        for (int l = 0; l < CTL_MAX_LISTENERS_PER_CTL; l++)
            if (g.ctl[id].listeners[l] == handle) g.ctl[id].listeners[l] = -1;
        g.listeners[handle].used = 0;
        g.listeners[handle].fn = NULL;
    }
    pthread_mutex_unlock(&g.ring_lock);
}

/* The audio thread, once per render(): apply the queued posts, then notify. Never blocks (trylock), never
 * allocates. Listeners are called with the lock held so a concurrent ctl_remove_listener cannot pull the entry
 * from under us; listeners must be quick (they are: JS callbacks are queued by midi.c, LED feedback is a ring). */
void ctl_drain(void) {
    if (!g.inited) return;
    if (pthread_mutex_trylock(&g.ring_lock) != 0) return;   /* a poster is busy: next block */
    while (g.ring_tail != g.ring_head) {
        ctl_post_t *p = &g.ring[g.ring_tail & (CTL_RING - 1)];
        ctl_t *c = &g.ctl[p->id];
        double v = p->v;
        if (p->delta) v += bits_dbl(__atomic_load_n(&c->value_bits, __ATOMIC_ACQUIRE));
        __atomic_store_n(&c->value_bits, dbl_bits(v), __ATOMIC_RELEASE);
        g.ring_tail++;
    }
    int n = ctl_count();
    for (int i = 0; i < n; i++) {
        ctl_t *c = &g.ctl[i];
        double v = bits_dbl(__atomic_load_n(&c->value_bits, __ATOMIC_ACQUIRE));
        if (v == c->last_notified) continue;
        c->last_notified = v;
        for (int l = 0; l < CTL_MAX_LISTENERS_PER_CTL; l++) {
            int h = c->listeners[l];
            if (h < 0) continue;
            ctl_listener_t *L = &g.listeners[h];
            if (L->used && L->fn) L->fn(i, v, L->user);
        }
    }
    pthread_mutex_unlock(&g.ring_lock);
}

/* ------------------------------------------------------------------------------------------------ text */
void ctl_set_text(ctl_id id, const char *text) {
    if (!ctl_valid(id) || g.ctl[id].text_index < 0) return;
    pthread_mutex_lock(&g.text_lock);
    snprintf(g.text[g.ctl[id].text_index], CTL_TEXT_LEN, "%s", text ? text : "");
    pthread_mutex_unlock(&g.text_lock);
    /* bump the numeric value so listeners (the screen's dirty flag) see a change */
    ctl_set(id, ctl_get(id) + 1.0);
}

int ctl_text(ctl_id id, char *buf, int n) {
    if (!buf || n <= 0) return 0;
    buf[0] = 0;
    if (!ctl_valid(id) || g.ctl[id].text_index < 0) return 0;
    pthread_mutex_lock(&g.text_lock);
    snprintf(buf, (size_t)n, "%s", g.text[g.ctl[id].text_index]);
    pthread_mutex_unlock(&g.text_lock);
    return (int)strlen(buf);
}
