/* reMixXXL: the analysis cache next to a track ("<track>.rxa", see cache_read / cache_write in engine.c) is a raw dump of
 * fixed-size pieces. They must be the same size on 32-bit and 64-bit ARM, so a cache written by one build is read by the other
 * (and re-analysis is never needed because of the CPU). Run as armv7 and as aarch64 by test/arm_sim.sh. */
#include <stdio.h>
#include <stdint.h>
#include "remix.h"

int main(void) {
    rx_analysis_t a;
    size_t col = sizeof(rx_wave_col_t), ov = sizeof a.overview, bpm = sizeof a.bpm, b0 = sizeof a.beat0, gain = sizeof a.gain;
    int ok = col == 4 && ov == (size_t)RX_OVERVIEW_W * 4 && bpm == 8 && b0 == 8 && gain == 4;
    printf("cache pieces: wave column %zu, overview %zu, bpm %zu, beat0 %zu, gain %zu (a pointer here is %zu bytes) -> %s\n",
           col, ov, bpm, b0, gain, sizeof(void *), ok ? "same on both CPUs" : "DIFFERENT");
    return ok ? 0 : 1;
}
