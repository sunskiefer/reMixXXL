#!/bin/sh
# reMixXXL -- native logic test of deck.c / mixer.c / fx.c (links the real control.c; decode/analyze/log are stubbed)
set -e
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/rx_test_deck
cc -O1 -g -std=gnu11 -w -Isrc -Ithird_party -o "$OUT" test/test_deck.c src/deck.c src/mixer.c src/fx.c src/control.c -lm -lpthread
"$OUT"
