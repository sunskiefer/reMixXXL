reMixXXL and reMixXXL CUE for the 64-bit MPCs (Live III, XL, One G2, Key G2) and for Akai's plugin folder format (MPC OS 3.10.1), as well as the Akai Force and the 32-bit MPCs. The code, the pages, the wheels and the cue are the 0.1.25 code (tested on a Force); what is new is how it is built and packed: both CPUs, three zips per plugin.

**Pick your zips** (reMixXXL, and reMixXXL CUE if you use the cue; same three for each)

| Zip | For |
|---|---|
| `reMixXXL-<version>-mpc-armv7.zip` | Akai Force and the 32-bit MPCs (X, Live, Live II, One, Key 61 / 37), modded, MPC OS 3.x |
| `reMixXXL-<version>-mpc-aarch64.zip` | the 64-bit MPCs (Live III, XL, One G2, Key G2), modded, MPC OS 3.x |
| `reMixXXL-<version>-mpc-sdk.zip` | any MPC on MPC OS 3.10.1 or later: copy the folder into `VST2 Plugins`, no mod needed |
| `reMixXXL-CUE-<version>-mpc-armv7.zip` / `-mpc-aarch64.zip` / `-mpc-sdk.zip` | the same three, for the cue effect |

A pre-release until it has been tried on a device (`tested.json`). **0.2.0 has been built and tested offline only (as ARM code, 32-bit and 64-bit, under qemu, 2026-11-17); it has not been tried on a device yet.** The 0.1.25 code it carries was tested on an Akai Force (MPC OS 3.9.1). Not verified on a 64-bit MPC or on MPC OS 3.10.1: the wheel screen and touch, the cue on outputs 3/4, MIDI. If something does not work, `remixxxl_diag.log` and `remixxxl_overlay.log` (next to the plugin) say why. The full list of changes is in [CHANGELOG.md](https://github.com/sunskiefer/reMixXXL/blob/main/CHANGELOG.md); install steps in the [README](https://github.com/sunskiefer/reMixXXL#install).
