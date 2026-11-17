#!/usr/bin/env bash
# Build reMixXXL CUE (a small audio effect that plays reMixXXL's cue bus) for MPC OS, ARMv7 and aarch64. tools/package.sh makes the release zips.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/../third_party/mpc-vst-plugins}"
ZIG="${ZIG:-python3 -m ziglang}"
NAME="L'Cronx - VST - reMixXXL CUE"
mkdir -p build/arm build/arm64 build/tools
[ -x build/tools/shadow_art ] || gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -x c -o build/tools/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
rm -rf build/skin
SHADOW_ART="$PWD/build/tools/shadow_art" python3 "$MPC_VST/tools/gen_vst.py" vst.json 2>&1 | grep -v -e "TOUCH" -e "EDGE" -e "MODULE_DIR" -e "QLINK" || true
[ -f build/params.h ] && [ -d "build/skin/$NAME/Plugin Skins" ]
python3 patch_skin.py "build/skin/$NAME/Plugin Skins" ../art/ui/knob.png
COMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu11"
# build_so <dir> <zig target + CPU flags>: reMixXXL CUE as that CPU's code, <dir>/remixxxl_cue.so. The wrapper is reMixXXL's own copy
# (../src/wrapper/vst2_wrap.c, with mpc-vst-plugins #486/#487), so both plugins of the package behave alike.
build_so() {
  local D="$1" TGT="$2" OBJS="" f o
  mkdir -p "$D"
  for f in src/cue_engine.c ../src/wrapper/vst2_wrap.c; do
    o="$D/$(basename "$f").o"
    $ZIG cc $TGT $COMMON -Ibuild -I../src -I"$MPC_VST" -I"$MPC_VST/wrapper" -c "$f" -o "$o"
    OBJS="$OBJS $o"
  done
  printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > "$D/exports.map"
  $ZIG cc $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script="$D/exports.map" -Wl,--gc-sections -Wl,-s $OBJS -lm -ldl -lpthread -o "$D/remixxxl_cue.so"
}
build_so build/arm "-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"   # 32-bit MPCs and the Force
build_so build/arm64 "-target aarch64-linux-gnu.2.31 -mcpu=generic -ffp-contract=off"                               # 0.2.0: the 64-bit MPCs
PKG="build/package/$NAME"
rm -rf build/package && mkdir -p "$PKG"
cp -r "build/skin/$NAME/." "$PKG/"   # no .so here: tools/package.sh puts each CPU's into its own release zip
echo "-> $PKG, build/arm/remixxxl_cue.so, build/arm64/remixxxl_cue.so"
