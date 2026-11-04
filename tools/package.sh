#!/usr/bin/env bash
# Package build/package/ as the real release zip (install.sh, uninstall.sh, ARMv7 build) with the escaped L'Cronx installers.
# The user's Tracks folder is user data: the installer carries it over to the new folder (0.1.1 forgot, and deleted it).
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:-$PWD/third_party/mpc-vst-plugins}"
VER="$(python3 -c "import json;v=json.load(open('vst.json'))['version'];print('%d.%d.%d'%(v//10000,(v//100)%100,v%100))")"
python3 "$MPC_VST/tools/release.py" --so build/package/*/remixxxl.so --skin "build/package/L'Cronx - VST - reMixXXL" \
  --entry build/pluginlist-entry.xml --version "$VER" --id remixxxl --repo sunskiefer/reMixXXL --license GPL-3.0-or-later \
  --about "reMixXXL by L'Cronx: four Mixxx-style DJ decks with CDJ wheels, a mixer, effects and a track browser for the Akai Force" --user-data Tracks -o dist
