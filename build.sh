#!/usr/bin/env bash
# Build reMixXXL for MPC OS: armv7-a hard-float (32-bit MPCs and the Force) and aarch64 (64-bit MPCs), both glibc <= 2.31 so they also load on MPC OS 2.x.
#   ./build.sh                   the whole plugin (MPC_VST=<path> to use another framework checkout)
#   ./build.sh --skin-only       the skin, its checks and the page previews only (no compile): build/preview/<page>.png
# Needs: python3 + Pillow and numpy, a host gcc (for shadow_art), and Zig (pip install ziglang). No Docker.
# Output: build/arm/remixxxl.so, build/arm64/remixxxl.so and build/package/L'Cronx - VST - reMixXXL/ (skin, Fonts, Mappings, Tracks, Screen, Layout;
#   no .so: tools/package.sh puts each CPU's into its own release zip)
# The vendor name carries an apostrophe: every use of NAME / PKG below is double-quoted. Keep it so.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/third_party/mpc-vst-plugins}"
MIXXX_RES="${MIXXX_RES:-$PWD/mappings}"   # Mixxx res/controllers files (GPL-2.0-or-later), vendored in mappings/
ZIG="${ZIG:-python3 -m ziglang}"
NAME="L'Cronx - VST - reMixXXL"
SKIN_ONLY=0
[ "${1:-}" = "--skin-only" ] && SKIN_ONLY=1
. ./build_srcs.sh

python3 tools/gen_params.py
python3 tools/gen_web.py                                 # src/web_page.h (the companion web page)
python3 tools/remix_pages.py
python3 tools/remix_skin.py --layout
python3 tools/gen_platter_frames.py                      # build/Screen/platter_frames.png (the spinning centre label)
python3 tools/gen_ring.py                                # build/Screen/ring_574 + ring_420 and their lights (the WHEEL and WHEELS pages' rings, turned freely by the screen)

mkdir -p build/tools build/arm
[ -x build/tools/shadow_art ] || gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -x c -o build/tools/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
rm -rf build/skin
SHADOW_ART="$PWD/build/tools/shadow_art" \
  python3 "$MPC_VST/tools/gen_vst.py" vst.json 2>&1 | grep -v -e "TOUCH" -e "EDGE" -e "MODULE_DIR" -e "QLINK" || true
[ -f build/params.h ] && [ -d "build/skin/$NAME/Plugin Skins" ]
python3 tools/remix_skin.py "build/skin/$NAME/Plugin Skins"       # every page, from tools/remix_skin.py
python3 tools/check_skin.py "build/skin/$NAME"                     # touch boxes, edges, Q-Links, as MPC loads it (open lists excepted)
rm -rf build/preview build/Layout && mkdir -p build/Layout
python3 tools/remix_skin.py --preview "build/skin/$NAME/Plugin Skins" build/preview
python3 tools/remix_skin.py --overlays "build/skin/$NAME/Plugin Skins" build/Layout
cp build/pages.json build/Layout/pages.json
if [ "$SKIN_ONLY" = 1 ]; then
  echo "-> skin only: build/skin/$NAME, previews in build/preview, overlays in build/Layout"
  exit 0
fi

# build_so <dir> <zig target + CPU flags>: the plugin as that CPU's code, <dir>/remixxxl.so (glibc <= 2.31 for both)
build_so() {
  local D="$1" TGT="$2" OBJS="" f o
  local COMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu11"
  local CXXCOMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=c++17"
  mkdir -p "$D"
  for f in $PLUGIN_SRCS src/wrapper/vst2_wrap.c; do
    [ -f "$f" ] || { echo "build.sh: missing source $f"; exit 1; }
    o="$D/$(echo "$f" | tr / _).o"
    $ZIG cc $TGT $COMMON -Ibuild -I"$MPC_VST" -I"$MPC_VST/wrapper" $INCS -c "$f" -o "$o"   # "wrapper/engine.h" resolves in the framework checkout
    OBJS="$OBJS $o"
  done
  for f in $QJS_SRCS; do   # QuickJS (C)
    o="$D/$(echo "$f" | tr / _).o"
    $ZIG cc $TGT $COMMON $QJS_FLAGS -Ithird_party/quickjs -c "$f" -o "$o"
    OBJS="$OBJS $o"
  done
  for f in $QM_SRCS; do    # qm-dsp (C++17; kissfft is C)
    o="$D/$(echo "$f" | tr / _).o"
    case "$f" in
      *.c) $ZIG cc $TGT $COMMON $QM_INCS -c "$f" -o "$o" ;;
      *) $ZIG c++ $TGT $CXXCOMMON $QM_INCS -c "$f" -o "$o" ;;
    esac
    OBJS="$OBJS $o"
  done
  printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > "$D/exports.map"
  # zig c++ for the final link: it brings libc++ for the qm-dsp objects (statically, so the MPC needs no libstdc++)
  $ZIG c++ $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script="$D/exports.map" -Wl,--gc-sections -Wl,-s \
    $OBJS -lm -ldl -lpthread -o "$D/remixxxl.so"
}
# 32-bit MPCs: MPC X, Live, Live II, One, Key 61/37 and the Force (as 0.1.25)
build_so build/arm "-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"
# 0.2.0: the 64-bit MPCs (Live III, XL, One G2, Key G2): the same code
build_so build/arm64 "-target aarch64-linux-gnu.2.31 -mcpu=generic -ffp-contract=off"

PKG="build/package/$NAME"
rm -rf build/package && mkdir -p "$PKG"
cp -r "build/skin/$NAME/." "$PKG/"
mkdir -p "$PKG/Fonts" && cp third_party/fonts/TitilliumWeb-SemiBold.ttf third_party/fonts/TitilliumWeb-Bold.ttf third_party/fonts/OFL.txt "$PKG/Fonts/"
mkdir -p "$PKG/Mappings"
for f in "Denon MC3000.midi.xml" "Denon-MC3000-scripts.js" "common-controller-scripts.js" "lodash.mixxx.js"; do
  cp "$MIXXX_RES/$f" "$PKG/Mappings/"
done
mkdir -p "$PKG/Tracks"
printf 'Put WAV, FLAC or MP3 files here (the Tracks folder of the plugin), or pick any folder on the BROWSE page.\n' > "$PKG/Tracks/README.txt"
mkdir -p "$PKG/Screen" && cp build/Screen/* "$PKG/Screen/"
mkdir -p "$PKG/Layout" && cp build/Layout/* "$PKG/Layout/"
echo "-> $PKG"
