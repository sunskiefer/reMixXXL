/* The cue bus ring, shared as a memory-mapped file between reMixXXL (producer) and the reMixXXL CUE effect (consumer): both are inside
 * MPC's one process, so a mapped file in /dev/shm (or /tmp) is a plain shared buffer. No library lookup, no symbols. */
#pragma once
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CUE_RING 4096             /* frames (a power of two) */
#define CUE_LAT 384               /* frames the consumer stays behind the producer: about 8.7 ms at 44.1 kHz */

typedef struct { _Atomic unsigned w; unsigned magic; float ring[CUE_RING * 2]; } cue_shm_t;

static inline cue_shm_t *cue_shm_open(void) {
    static const char *const paths[] = {"/dev/shm/remixxxl_cue.ring", "/tmp/remixxxl_cue.ring"};
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_RDWR | O_CREAT, 0666);
        if (fd < 0) continue;
        if (ftruncate(fd, (off_t)sizeof(cue_shm_t)) != 0) { close(fd); continue; }
        void *m = mmap(NULL, sizeof(cue_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m != MAP_FAILED) return (cue_shm_t *)m;
    }
    return NULL;
}

/* the consumer: fills out_lr (interleaved float stereo) with `frames` frames; *r is the consumer's own read position. Returns 1 when real
 * cue audio was delivered, 0 for silence. */
static inline int cue_shm_read(cue_shm_t *s, unsigned *r, int *primed, float *out_lr, int frames) {
    unsigned w = atomic_load_explicit(&s->w, memory_order_acquire);
    int behind = (int)(w - *r);
    if (!*primed || behind < 0 || behind > CUE_RING - 512) {
        if (w == 0) { memset(out_lr, 0, sizeof(float) * 2 * (size_t)frames); return 0; }
        *r = w - CUE_LAT; *primed = 1; behind = CUE_LAT;
    } else if (behind > 3 * CUE_LAT) { *r = w - CUE_LAT; behind = CUE_LAT; }
    if (behind < frames) { memset(out_lr, 0, sizeof(float) * 2 * (size_t)frames); return 0; }
    for (int k = 0; k < frames; k++) {
        unsigned i = (*r + (unsigned)k) & (CUE_RING - 1);
        out_lr[2 * k] = s->ring[2 * i]; out_lr[2 * k + 1] = s->ring[2 * i + 1];
    }
    *r += (unsigned)frames;
    return 1;
}
