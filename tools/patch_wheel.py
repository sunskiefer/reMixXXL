#!/usr/bin/env python3
"""reMixXXL 0.1.11: mount the user's new WHEEL page art (b2aa3a67 picture, transparent) on the 0.1.10 skin.
usage: python3 tools/patch_wheel.py "<0.1.10 plugin folder>/Plugin Skins" art/ui/controls/wheel_page_user_0.1.11.png
The picture's controls are cut at the boxes in tools/wheel_geom.py (skin = picture + OFF); the page background is rebuilt from
tools/remix_skin.py's clean background with the picture on it; the part components are re-pointed to the new boxes. Run it on the
extracted 0.1.10 release to get the 0.1.11 skin (the engine file is the 0.1.10 one, unchanged)."""
import sys, os, json, re, glob, copy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wheel_geom import *
sys.argv_saved = sys.argv; sys.argv = sys.argv[:1]
import remix_skin as R, remix_pages
from PIL import Image, ImageDraw
skin, pic = sys.argv_saved[1], sys.argv_saved[2]
B = Image.open(pic).convert("RGBA")
ox, oy = OFF
tui_p = os.path.join(skin, "TUI.json")
tui = json.load(open(tui_p))
defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
dm = {e["key"]: e for e in defs}

# ---- images
for f in glob.glob(os.path.join(skin, "rx_part_C_*.png")):
    os.remove(f)
newdef = {}      # key (name suffix) -> new def key
sizes = {}
for pid, lst in PARTS.items():
    for half, key, kind, (x, y, w, h) in lst:
        crop = B.crop((x, y, x + w, y + h))
        if kind == "rknob":
            continue
        tag = "C_%d%s_%dx%d" % (pid, half or "", w, h)
        crop.save(os.path.join(skin, "rx_part_%s_off.png" % tag))
        R.tint_lit(crop).save(os.path.join(skin, "rx_part_%s_on.png" % tag))
        sizes[key] = (pid, half, kind, (x, y, w, h), tag)

# jog adjust: the knob turning in place, from the user's picture
x, y, w, h = PARTS[16][0][3]
cx, cy, r = KNOB
src = B.crop((x, y, x + w, y + h))
import numpy as np
SS = 4
big = src.resize((w * SS, h * SS), Image.LANCZOS)
m = Image.new("L", big.size, 0)
ImageDraw.Draw(m).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=255)
disc = big.copy(); disc.putalpha(m)
base = big.copy()
ImageDraw.Draw(base).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=(0, 0, 0, 0))
old_strip = Image.open(os.path.join(skin, "rx_jogfeel_106x108.png"))
FR = old_strip.height // 108
strip = Image.new("RGBA", (w, h * FR), (0, 0, 0, 0))
for k in range(FR):
    ang = -135 + 270 * k / (FR - 1)
    rot = disc.rotate(-ang, resample=Image.BICUBIC, center=(cx * SS, cy * SS))
    fr = base.copy(); fr.alpha_composite(rot)
    strip.alpha_composite(fr.resize((w, h), Image.LANCZOS), (0, k * h))
os.remove(os.path.join(skin, "rx_jogfeel_106x108.png"))
strip.save(os.path.join(skin, "rx_jogfeel_%dx%d.png" % (w, h)))

# ---- background: clean, then the picture
bgp = os.path.join(skin, "rx_bg_5.png")
bg = Image.open(bgp).convert("RGBA")
clean = R.background(remix_pages.pages(), 5).convert("RGBA")
bg.paste(clean.crop((230, 48, bg.width, bg.height)), (230, 48))
bg.alpha_composite(B, (ox, oy))
bg.convert("RGB").save(bgp)

# ---- the master tempo lamp (a 20 px picture over the red lamp)
lamp_xy = (int(round(LAMP_C[0] + ox - 10)), int(round(LAMP_C[1] + oy - 10)))

# ---- TUI
def clone_def(old_key, new_key, repl):
    e = copy.deepcopy(dm[old_key]); e["key"] = new_key
    s = json.dumps(e)
    for a, b in repl:
        s = s.replace(a, b)
    e = json.loads(s); defs.append(e); dm[new_key] = e
    return new_key

OLD_BTN = {}   # name key -> old def key (taken from a placed component)
unused_types = set()
tabs = [e for e in defs if e["key"].startswith("WHEEL|")]
made = {}
popup_old = (993, 202)
for e in tabs:
    comps = e["value"]["componentsData"]
    keep = []
    for c in comps:
        nm = c["componentData"]["name"]
        ty = c["componentData"]["type"]
        if nm in ("cue/loop call art C", "tempo scale art C", "loop line art C", "beat loop line art C"):
            unused_types.add(ty); continue
        mm = re.match(r"^d\d_(\w+?)( C| field)?$", nm)
        key = mm.group(1) if mm else None
        if key in sizes and (nm.endswith(" C") or nm.endswith(" field")):
            pid, half, kind, (x, y, w, h), tag = sizes[key]
            old_ty = ty
            if key not in made:
                if kind == "popup":
                    nk = clone_def(old_ty, "rxPopPart_new_%s" % tag, [("rx_part_C_8_96x56_on.png", "rx_part_%s_on.png" % tag), ("rx_part_C_8_96x56_off.png", "rx_part_%s_off.png" % tag), ("0 0 96 56", "0 0 %d %d" % (w, h))])
                else:
                    ow, oh = re.search(r"(\d+)x(\d+)_on\.png", old_ty).groups()
                    otag = re.search(r"rx_part_(C_[0-9A-Z]+_\d+x\d+)", old_ty).group(1)
                    nk = clone_def(old_ty, "rxBtn_new_%s" % tag, [(otag, tag), ("0 0 %s %s" % (ow, oh), "0 0 %d %d" % (w, h))])
                made[key] = nk
            unused_types.add(old_ty)
            c["componentData"]["type"] = made[key]
            c["bounds"]["bounds"] = "%d %d %d %d" % (x + ox, y + oy, w, h)
        elif key == "jogfeel" and nm.endswith(" C"):
            x, y, w, h = PARTS[16][0][3]
            if "jogfeel" not in made:
                made["jogfeel"] = clone_def(ty, "rxJogFeel_%dx%d" % (w, h), [("rx_jogfeel_106x108.png", "rx_jogfeel_%dx%d.png" % (w, h)), ("0 0 106 108", "0 0 %d %d" % (w, h))])
            unused_types.add(ty)
            c["componentData"]["type"] = made["jogfeel"]
            c["bounds"]["bounds"] = "%d %d %d %d" % (x + ox, y + oy, w, h)
        elif nm == "master tempo lamp":
            c["bounds"]["bounds"] = "%d %d 20 20" % lamp_xy
        elif re.match(r"^d\d_rate_range (list|\d+%|WIDE)$", nm):
            nx, ny = PARTS[8][0][3][0] + ox, PARTS[8][0][3][1] + oy
            bx = [int(v) for v in c["bounds"]["bounds"].split()]
            bx[0] += nx - popup_old[0]; bx[1] += ny - popup_old[1]
            c["bounds"]["bounds"] = " ".join(str(v) for v in bx)
        keep.append(c)
    e["value"]["componentsData"] = keep

# drop the definitions nothing uses any more, and their pictures
used = set()
for e in defs:
    if e["key"].startswith(("WHEEL|", "WHEELS|")) or "|" in e["key"]:
        for c in e["value"]["componentsData"]:
            used.add(c["componentData"]["type"])
for e in defs:
    if not "|" in e["key"]:
        for c in e["value"].get("componentsData", []):
            used.add(c["componentData"]["type"])
defs[:] = [e for e in defs if "|" in e["key"] or e["key"] in used]
json.dump(tui, open(tui_p, "w"), indent=1)
# pictures that no definition or background names
txt = open(tui_p).read()
for f in os.listdir(skin):
    if f.endswith(".png") and f not in txt and not f.startswith("rx_bg_"):
        os.remove(os.path.join(skin, f)); print("removed unused", f)
print("patched", len(made), "parts; lamp at", lamp_xy)
