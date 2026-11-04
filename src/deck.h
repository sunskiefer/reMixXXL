/* reMixXXL -- deck.c additions beyond remix.h (engine.c includes this). */
#pragma once
#include "remix.h"

/* Takes ownership of a decoded track and its analysis (both made by decode_file()/analyze_audio() or read from a
 * cache) and swaps them in exactly as deck_load() does after decoding. Off the audio thread only. The deck frees
 * both (decode_free/analyze_free) when the track is replaced or ejected. Returns 0 on success; on failure the
 * caller still owns audio/analysis. */
int deck_set_track(rx_deck_t *d, const char *path, rx_audio_t *audio, rx_analysis_t *an);

/* Text form of the deck's cue data (hot cues with colour, main cue, loop in/out, beat grid override):
 *   hot <n> <seconds> <colour>\n   cue <seconds>\n   loop <in> <out>\n   grid <bpm> <beat0>\n
 * deck_cues_export only reads the deck's own fields (safe from the audio thread). deck_cues_import stages the
 * text; it is applied by the audio thread at the next deck_tick_controls(), after any pending track swap, so call
 * it after deck_set_track()/deck_load() for the same track. */
void deck_cues_export(rx_deck_t *d, char *buf, int n);
void deck_cues_import(rx_deck_t *d, const char *text);

/* Frees a track the audio thread ejected (the "eject" trigger only detaches it: the audio thread never frees).
 * Any of deck_load/deck_set_track/deck_eject/deck_destroy also reaps; engine.c may call this from its worker
 * whenever convenient. Off the audio thread only. */
void deck_reap(rx_deck_t *d);

/* The rate the deck actually used in its last block (scratch, search, sync included; signed). */
double deck_current_rate(rx_deck_t *d);
