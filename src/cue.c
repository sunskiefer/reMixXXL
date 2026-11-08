/* The cue bus: what the decks with PFL on sound like (pre-fader), for the reMixXXL CUE effect on an audio track.
 * MPC plays only the first stereo pair (outputs 1/2) of a plugin, so the cue cannot leave through this plugin's own outputs.
 * Instead reMixXXL writes it into a ring in a memory-mapped file (cue_shm.h) that the CUE effect, in the same MPC process, reads.
 * Left = decks 1 and 3 folded to mono, right = decks 2 and 4. One producer, one consumer, no lock: a write position the producer
 * publishes, a read position only the consumer keeps, and CUE_LAT frames of cushion so either may run first inside a period. */
#include "remix.h"
#include "cue_shm.h"

static cue_shm_t *g_shm;
static int g_tries;

void rx_cue_push(const float *lr, int frames) {
    if (!g_shm) {
        if (g_tries++ % 256) return;                       /* retry now and then, never every block */
        g_shm = cue_shm_open();
        if (!g_shm) return;
        atomic_store(&g_shm->w, 0);                        /* a fresh start: the consumer re-aims */
        rx_log("cue: ring mapped");
    }
    unsigned w = atomic_load_explicit(&g_shm->w, memory_order_relaxed);
    for (int k = 0; k < frames; k++) {
        unsigned i = (w + (unsigned)k) & (CUE_RING - 1);
        g_shm->ring[2 * i] = lr[2 * k]; g_shm->ring[2 * i + 1] = lr[2 * k + 1];
    }
    atomic_store_explicit(&g_shm->w, w + (unsigned)frames, memory_order_release);
}
