#!/usr/bin/env python3
"""reMixXXL CUE: swap the generated knob for reMixXXL's own (art/ui/knob.png turned in place, 101 frames, the same as the
reMixXXL knobs that work on the Force). The generated strip was 127 x 128 = 16256 px tall; MPC's limit is 12288."""
import json, os, sys
from PIL import Image
skin, art = sys.argv[1], sys.argv[2]
DIA, FRAMES, POINTER, CELL_W = 92, 101, -17.0, 221
src = Image.open(art).convert("RGBA")
big = Image.new("RGBA", (src.width * 3, src.height * 3), (0, 0, 0, 0))
big.alpha_composite(src.resize((src.width * 3, src.height * 3), Image.BICUBIC), (0, 0))
strip = Image.new("RGBA", (DIA, DIA * FRAMES), (0, 0, 0, 0))
for k in range(FRAMES):
    ang = -135 + 270 * k / (FRAMES - 1)
    fr = big.rotate(-(ang - POINTER), resample=Image.BICUBIC, center=(big.width / 2, big.height / 2))
    strip.alpha_composite(fr.resize((DIA, DIA), Image.LANCZOS), (0, k * DIA))
assert strip.height <= 12288
old = os.path.join(skin, "sh_knob_r59.png")
if os.path.exists(old): os.remove(old)
strip.save(os.path.join(skin, "rx_knob_92.png"))
p = os.path.join(skin, "TUI.json"); d = json.load(open(p))
cd = d["pageData"]["componentDefinitions"]["localComponentDefinitions"]
parts = cd[0]["value"]["componentsData"]
def bounds(c, b): c["bounds"]["bounds"] = b
bounds(parts[0], "0 0 %d 188" % CELL_W)
k = parts[1]["componentData"]["data"]; k["filmStrip"] = "rx_knob_92.png"; k["numFrames"] = FRAMES
bounds(parts[1], "%d 0 %d %d" % ((CELL_W - DIA) // 2, DIA, DIA))
parts[2]["componentData"]["data"]["textStyle"]["font"]["height"] = 35.7; bounds(parts[2], "0 94 %d 48" % CELL_W)
parts[3]["componentData"]["data"]["textStyle"]["font"]["height"] = 37.4; bounds(parts[3], "0 144 %d 44" % CELL_W)
for c in cd[1]["value"]["componentsData"]:
    if c["componentData"]["name"] == "CUE LEVEL": bounds(c, "%d 230 %d 188" % (640 - CELL_W // 2, CELL_W))
json.dump(d, open(p, "w"), indent=2)
print("patched knob:", strip.size)
