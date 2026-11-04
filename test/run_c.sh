#!/bin/sh
# reMixXXL: native (x86) logic test of the MIDI side: midi_xml.c, midi.c, midi_js.c (QuickJS), log.c with the stub
# control table (test/ctl_stub_c.c). Not a Force build: the Force binary is built by the release script. Usage:
#   sh test/run_c.sh            (from the repo root; needs the Mixxx checkout for the MC3000 files, see MIXXX below)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MIXXX=${MIXXX:-/home/claude/sunskiefer/mixxx}
OUT="$ROOT/build/test_c"
PLUG="$ROOT/build/test_plugin"
mkdir -p "$OUT" "$PLUG/Mappings"
cp "$MIXXX/res/controllers/Denon MC3000.midi.xml" "$MIXXX/res/controllers/Denon-MC3000-scripts.js" \
   "$MIXXX/res/controllers/common-controller-scripts.js" "$PLUG/Mappings/"
rm -f "$PLUG/midi_learn.txt"
CC=${CC:-cc}
CFLAGS="-O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$ROOT/src -I$ROOT/third_party -I$ROOT/third_party/quickjs"
QJS="$OUT/quickjs_native.a"
if [ ! -f "$QJS" ] || [ "$ROOT/third_party/quickjs/quickjs.c" -nt "$QJS" ]; then
    echo "building quickjs (native, once)"
    for f in quickjs libregexp libunicode dtoa; do
        $CC -O2 -std=gnu11 -w -I"$ROOT/third_party/quickjs" -c "$ROOT/third_party/quickjs/$f.c" -o "$OUT/$f.o"
    done
    ar rcs "$QJS" "$OUT/quickjs.o" "$OUT/libregexp.o" "$OUT/libunicode.o" "$OUT/dtoa.o"
fi
$CC $CFLAGS "$ROOT/test/test_midi.c" "$ROOT/test/ctl_stub_c.c" "$ROOT/src/midi.c" "$ROOT/src/midi_js.c" \
    "$ROOT/src/midi_xml.c" "$ROOT/src/log.c" "$QJS" -o "$OUT/test_midi" -lm -lpthread -ldl
"$OUT/test_midi" "$PLUG"
echo "diag log: $PLUG/remixxxl_diag.log"
