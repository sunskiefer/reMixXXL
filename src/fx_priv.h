/* reMixXXL -- fx.c additions beyond remix.h. */
#pragma once
#include "remix.h"

/* The tempo the beat-synced effects (ECHO in beats, FLANGER/PHASER LFO periods, LOOP ROLL) follow. engine.c calls
 * it once per block with the tempo of the deck feeding the unit (or the sync leader); bpm <= 0 means "unknown"
 * and the beat-length parameters fall back to seconds, as in Mixxx (groupFeatures.beat_length absent). */
void fx_set_tempo(rx_fx_t *f, double bpm);

/* Per-channel tempo (channel 0..3 = decks, RX_DECKS = master); fx_set_tempo sets all of them. */
void fx_set_channel_tempo(rx_fx_t *f, int channel, double bpm);
