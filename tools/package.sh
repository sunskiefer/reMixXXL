#!/usr/bin/env bash
# Package the real release zips into dist/ with the open-mpc release tools (third_party/open-mpc-release, the L'Cronx apostrophe shell-escaped):
# for reMixXXL and for reMixXXL CUE, each as
#   <name>-<version>-mpc-armv7.zip    32-bit MPCs and the Force (install.sh)
#   <name>-<version>-mpc-aarch64.zip  64-bit MPCs: Live III, XL, One G2, Key G2 (install.sh)
#   <name>-<version>-mpc-sdk.zip      one folder for every MPC on MPC OS 3.10.1 or later (Akai's plugin folder format)
# Run ./build.sh and ./cue/build.sh first. The user's Tracks folder is user data: the installers carry it over (0.1.1 forgot, and deleted it).
set -euo pipefail
cd "$(dirname "$0")/.."
R=third_party/open-mpc-release
VER="$(python3 -c "import json;v=json.load(open('vst.json'))['version'];print('%d.%d.%d'%(v//10000,(v//100)%100,v%100))")"
rm -rf dist && mkdir -p dist
for A in arm arm64; do
  python3 "$R/release.py" --so build/$A/remixxxl.so --skin "build/package/L'Cronx - VST - reMixXXL" \
    --entry build/pluginlist-entry.xml --version "$VER" --id remixxxl --repo sunskiefer/reMixXXL --license GPL-3.0-or-later \
    --asset-name reMixXXL --user-data Tracks \
    --about "reMixXXL by L'Cronx: four Mixxx-style DJ decks with CDJ wheels, a mixer, effects and a track browser for the Akai Force and MPCs" \
    -o dist
  python3 "$R/release.py" --so cue/build/$A/remixxxl_cue.so --skin "cue/build/package/L'Cronx - VST - reMixXXL CUE" \
    --entry cue/build/pluginlist-entry.xml --version "$VER" --id remixxxl-cue --repo sunskiefer/reMixXXL --license GPL-3.0-or-later \
    --asset-name reMixXXL-CUE \
    --about "reMixXXL CUE by L'Cronx: plays reMixXXL's PFL cue (decks 1/3 left, 2/4 right) on an audio track routed to the headphone or cue outputs (3/4)" \
    -o dist
done
for N in reMixXXL reMixXXL-CUE; do
  python3 "$R/sdk_zip.py" "dist/$N-$VER-mpc-sdk.zip" "dist/$N-$VER-mpc-armv7.zip" "dist/$N-$VER-mpc-aarch64.zip"
done
ls -la dist
