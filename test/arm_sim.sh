#!/usr/bin/env bash
# reMixXXL's offline tests as ARM code, 32-bit (armv7) and 64-bit (aarch64), run under qemu-user. They are built with the plugin's own
# CPU flags as static musl programs, so they run without an ARM glibc. No x86 build (the plugin is for ARM MPCs only).
#   decks    deck.c / mixer.c / fx.c logic (test_deck.c, the real control.c)
#   screen   screen.c + font.c drawing (test_screen.c)
#   analyze  decoder + beat / key analysis (test_analyze.c, qm-dsp, C++17)
#   midi     MIDI maps and the controller scripts (midi.c, midi_js.c = QuickJS, midi_xml.c)
#   layout   the analysis cache pieces are the same size on both CPUs (test_layout.c)
#   noovl    the wheel overlay and touch layer start where there is no display or touch device and fail soft (test_noovl.c)
# Needs qemu-user (qemu-arm, qemu-aarch64) and Zig; ./build.sh must have run (src/boxes.h, build/params.h).   test/arm_sim.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. ./build_srcs.sh
ZIG="${ZIG:-python3 -m ziglang}"
for q in qemu-arm qemu-aarch64; do command -v "$q" >/dev/null || { echo "arm_sim: $q not found (apt install qemu-user)"; exit 1; }; done
[ -f src/boxes.h ] || { echo "arm_sim: src/boxes.h is missing, run ./build.sh first"; exit 1; }
ROOT="$PWD"
FONT=third_party/fonts/TitilliumWeb-SemiBold.ttf
TINCS="$INCS -Isrc"

# sim <dir> <zig target + CPU flags> <qemu>
sim() {
  local D="$1" TGT="$2" QEMU="$3" f o OBJS pids=()
  rm -rf "$D" && mkdir -p "$D"
  local C="$ZIG cc $TGT -O2 -w -std=gnu11 $TINCS" CXX="$ZIG c++ $TGT -O2 -w -std=c++17"
  # objects, in parallel
  obj() { local src="$1"; shift; echo "$D/$(echo "$src" | tr / _).o"; }
  build_c() { $C "$@" -c "$1" -o "$(obj "$1")" ; }
  for f in src/deck.c src/mixer.c src/fx.c src/control.c src/screen.c src/font.c src/decode.c src/analyze.c src/log.c src/midi.c src/midi_xml.c \
           src/overlay.c src/touch.c test/test_deck.c test/test_screen.c test/test_analyze.c test/test_midi.c test/ctl_stub_c.c test/test_layout.c test/test_noovl.c; do
    ( $C -c "$f" -o "$(obj "$f")" ) & pids+=($!)
  done
  ( $C -D_GNU_SOURCE -c src/midi_js.c -o "$(obj src/midi_js.c)" ) & pids+=($!)
  for f in $QJS_SRCS; do ( $ZIG cc $TGT -O2 -w -std=gnu11 $QJS_FLAGS -Ithird_party/quickjs -c "$f" -o "$(obj "$f")" ) & pids+=($!); done
  for f in $QM_SRCS; do
    case "$f" in
      *.c) ( $ZIG cc $TGT -O2 -w -std=gnu11 $QM_INCS -Isrc -c "$f" -o "$(obj "$f")" ) & pids+=($!) ;;
      *) ( $CXX $QM_INCS -Isrc -c "$f" -o "$(obj "$f")" ) & pids+=($!) ;;
    esac
  done
  for p in "${pids[@]}"; do wait "$p"; done
  local QJS="" QM=""
  for f in $QJS_SRCS; do QJS="$QJS $(obj "$f")"; done
  for f in $QM_SRCS; do QM="$QM $(obj "$f")"; done
  link() { local out="$1"; shift; $ZIG c++ $TGT -static "$@" -lm -lpthread -o "$D/$out"; }
  link t_deck "$(obj test/test_deck.c)" "$(obj src/deck.c)" "$(obj src/mixer.c)" "$(obj src/fx.c)" "$(obj src/control.c)"
  link t_screen "$(obj test/test_screen.c)" "$(obj src/screen.c)" "$(obj src/font.c)"
  link t_analyze "$(obj test/test_analyze.c)" "$(obj src/decode.c)" "$(obj src/analyze.c)" $QM
  link t_midi "$(obj test/test_midi.c)" "$(obj test/ctl_stub_c.c)" "$(obj src/midi.c)" "$(obj src/midi_js.c)" "$(obj src/midi_xml.c)" "$(obj src/log.c)" $QJS
  link t_layout "$(obj test/test_layout.c)"
  link t_noovl "$(obj test/test_noovl.c)" "$(obj src/overlay.c)" "$(obj src/touch.c)" "$(obj src/screen.c)" "$(obj src/font.c)" "$(obj src/control.c)" "$(obj src/log.c)"
  echo "== $D ($QEMU)"
  mkdir -p "$D/plugin/Mappings" && cp mappings/*.midi.xml mappings/*.js "$D/plugin/Mappings/"
  local OUT="$D/out"; mkdir -p "$OUT"
  echo "-- decks";   "$QEMU" "$D/t_deck" | tail -2
  echo "-- screen";  "$QEMU" "$D/t_screen" "$OUT" "$FONT" | tail -2
  echo "-- analyze"; "$QEMU" "$D/t_analyze" | tail -2
  echo "-- midi";    "$QEMU" "$D/t_midi" "$ROOT/$D/plugin" | tail -2
  echo "-- layout";  "$QEMU" "$D/t_layout"
  echo "-- noovl";   "$QEMU" "$D/t_noovl"
}
sim build/armsim "-target arm-linux-musleabihf -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off" qemu-arm
# 0.2.0: the 64-bit MPCs (Live III, XL, One G2, Key G2)
sim build/arm64sim "-target aarch64-linux-musl -mcpu=generic -ffp-contract=off" qemu-aarch64
echo PASSED
