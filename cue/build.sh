#!/usr/bin/env bash
# Build reMixXXL CUE (a small audio effect that plays reMixXXL's cue bus) for MPC OS, ARMv7, and package its real release zip into ../dist.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/../third_party/mpc-vst-plugins}"
ZIG="${ZIG:-python3 -m ziglang}"
NAME="L'Cronx - VST - reMixXXL CUE"
mkdir -p build/arm build/tools
[ -x build/tools/shadow_art ] || gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -x c -o build/tools/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
rm -rf build/skin
SHADOW_ART="$PWD/build/tools/shadow_art" python3 "$MPC_VST/tools/gen_vst.py" vst.json 2>&1 | grep -v -e "TOUCH" -e "EDGE" -e "MODULE_DIR" -e "QLINK" || true
[ -f build/params.h ] && [ -d "build/skin/$NAME/Plugin Skins" ]
python3 patch_skin.py "build/skin/$NAME/Plugin Skins" ../art/ui/knob.png
TGT="-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"
COMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu11"
OBJS=""
for f in src/cue_engine.c "$MPC_VST/wrapper/vst2_wrap.c"; do
  o="build/arm/$(basename "$f").o"
  $ZIG cc $TGT $COMMON -Ibuild -I../src -I"$MPC_VST/wrapper" -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > build/arm/exports.map
$ZIG cc $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script=build/arm/exports.map -Wl,--gc-sections -Wl,-s $OBJS -lm -ldl -lpthread -o build/arm/remixxxl_cue.so
PKG="build/package/$NAME"
rm -rf build/package && mkdir -p "$PKG"
cp -r "build/skin/$NAME/." "$PKG/"
cp build/arm/remixxxl_cue.so "$PKG/"
python3 "$MPC_VST/tools/release.py" --so "$PKG/remixxxl_cue.so" --skin "$PKG" --entry build/pluginlist-entry.xml --version 0.1.3 \
  --id remixxxl-cue --repo sunskiefer/reMixXXL --license GPL-3.0-or-later \
  --about "reMixXXL CUE by L'Cronx: plays reMixXXL's PFL cue (decks 1/3 left, 2/4 right) on an audio track routed to the Force's headphone or cue outputs" -o ../dist
