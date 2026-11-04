#!/usr/bin/env bash
# Build reMixXXL for MPC OS (armv7-a hard-float, glibc <= 2.31 so it also loads on MPC OS 2.x).
#   ./build.sh                   the whole plugin (MPC_VST=<path> to use another framework checkout)
#   ./build.sh --skin-only       the skin, its checks and the page previews only (no compile): build/preview/<page>.png
# Needs: python3 + Pillow and numpy, a host gcc (for shadow_art), and Zig (pip install ziglang). No Docker.
# Output: build/package/L'Cronx - VST - reMixXXL/  (skin, remixxxl.so, Fonts, Mappings, Tracks, Screen, Layout)
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

TGT="-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"
COMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu11"
OBJS=""
for f in $PLUGIN_SRCS src/wrapper/vst2_wrap.c; do
  [ -f "$f" ] || { echo "build.sh: missing source $f"; exit 1; }
  o="build/arm/$(echo "$f" | tr / _).o"
  $ZIG cc $TGT $COMMON -Ibuild -I"$MPC_VST" -I"$MPC_VST/wrapper" $INCS -c "$f" -o "$o"   # "wrapper/engine.h" resolves in the framework checkout
  OBJS="$OBJS $o"
done
for f in $QJS_SRCS; do   # QuickJS (C)
  o="build/arm/$(echo "$f" | tr / _).o"
  $ZIG cc $TGT $COMMON $QJS_FLAGS -Ithird_party/quickjs -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
CXXCOMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=c++17"
for f in $QM_SRCS; do    # qm-dsp (C++17; kissfft is C)
  o="build/arm/$(echo "$f" | tr / _).o"
  case "$f" in
    *.c) $ZIG cc $TGT $COMMON $QM_INCS -c "$f" -o "$o" ;;
    *) $ZIG c++ $TGT $CXXCOMMON $QM_INCS -c "$f" -o "$o" ;;
  esac
  OBJS="$OBJS $o"
done
printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > build/arm/exports.map
# zig c++ for the final link: it brings libc++ for the qm-dsp objects (statically, so the Force needs no libstdc++)
$ZIG c++ $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script=build/arm/exports.map -Wl,--gc-sections -Wl,-s \
  $OBJS -lm -ldl -lpthread -o build/arm/remixxxl.so

PKG="build/package/$NAME"
rm -rf build/package && mkdir -p "$PKG"
cp -r "build/skin/$NAME/." "$PKG/"
cp build/arm/remixxxl.so "$PKG/"
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
