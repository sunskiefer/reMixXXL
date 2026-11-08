/* The cue bus: what the decks with PFL on sound like (pre-fader), for the reMixXXL CUE effect on an audio track.
 * MPC plays only the first stereo pair (outputs 1/2) of a plugin, so the cue cannot leave through this plugin's own outputs.
 * Instead the CUE effect (its own .so, on an audio track routed to Force outputs 3/4) reads this ring through rx_cue_read (found by
 * dlopen(RTLD_NOLOAD) of this library). Left = decks 1 and 3 folded to mono, right = decks 2 and 4.
 * One producer (reMixXXL's render) and one consumer (the CUE effect's process) run on MPC's pool of audio threads, in no fixed order and
 * with no lock: a write position the producer publishes, a read position only the consumer touches, and CUE_LAT frames of cushion
 * so either may run first inside a period. */
#include <stdatomic.h>
#include <string.h>
#include "remix.h"

#define CUE_RING 4096             /* frames (a power of two) */
#define CUE_LAT 384               /* frames the consumer stays behind the producer: about 8.7 ms at 44.1 kHz */

static float g_ring[CUE_RING * 2];
static _Atomic unsigned g_w;      /* frames written so far (wraps; only differences are used) */
static unsigned g_r;              /* consumer: frames read so far */
static int g_primed;

void rx_cue_push(const float *lr, int frames) {
    unsigned w = atomic_load_explicit(&g_w, memory_order_relaxed);
    for (int k = 0; k < frames; k++) {
        unsigned i = (w + (unsigned)k) & (CUE_RING - 1);
        g_ring[2 * i] = lr[2 * k]; g_ring[2 * i + 1] = lr[2 * k + 1];
    }
    atomic_store_explicit(&g_w, w + (unsigned)frames, memory_order_release);
}

/* the consumer: fills out_lr (interleaved float stereo) with `frames` frames; silence until reMixXXL has produced some. Returns 1 when real
 * cue audio was delivered, 0 for silence. */
__attribute__((visibility("default"))) int rx_cue_read(float *out_lr, int frames) {
    unsigned w = atomic_load_explicit(&g_w, memory_order_acquire);
    int behind = (int)(w - g_r);
    if (!g_primed || behind < 0 || behind > CUE_RING - 512) {      /* first call, a restart, or fallen far behind: re-aim at the cushion */
        if (w == 0) { memset(out_lr, 0, sizeof(float) * 2 * (size_t)frames); return 0; }
        g_r = w - CUE_LAT; g_primed = 1; behind = CUE_LAT;
    } else if (behind > 3 * CUE_LAT) { g_r = w - CUE_LAT; behind = CUE_LAT; }   /* drifted ahead of the cushion: skip, never grow the delay */
    if (behind < frames) { memset(out_lr, 0, sizeof(float) * 2 * (size_t)frames); return 0; }   /* underrun: silence, the cushion refills */
    for (int k = 0; k < frames; k++) {
        unsigned i = (g_r + (unsigned)k) & (CUE_RING - 1);
        out_lr[2 * k] = g_ring[2 * i]; out_lr[2 * k + 1] = g_ring[2 * i + 1];
    }
    g_r += (unsigned)frames;
    return 1;
}
