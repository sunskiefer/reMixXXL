#!/usr/bin/env python3
"""Adds the CONTROLLERS page (tab "MAPS", the second page of the BROWSE key) to an existing reMixXXL skin.

    python3 tools/patch_maps.py "<plugin folder>/Plugin Skins"

Additive: nothing on the other pages moves. The engine's 14 new parameters (tools/gen_params.py, appended at the end of
params.json) push the wrapper's hidden popup parameters (the old 505..517) up by 14, so every "Parameter N" with N >= 505 in
TUI.json is renumbered first. Then the page is cloned from BROWSE: the six map rows (radio rows over live text), the status
line, the page number, LOAD, NONE, PAGE < and PAGE >; the wheel/Q-Link bank (Tab 2, SubTab 2) follows the BROWSE one.
"""
import copy
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import remix_skin as R  # noqa: E402  (button faces, fonts, colours: the same code that drew the other pages)

if len(sys.argv) != 2:
    sys.exit(__doc__)
SK = sys.argv[1]
OLD_PARAMS = 505          # the first hidden popup parameter of 0.1.12
params = json.load(open(os.path.join(ROOT, "params.json")))["params"]
IDX = {p["key"]: i for i, p in enumerate(params)}
SHIFT = len(params) - OLD_PARAMS
assert SHIFT == 14, "expected 14 new parameters, params.json has %d more than 0.1.12" % SHIFT
for k in ("mp_row1", "mp_row6", "mp_sel", "mp_row", "mp_page", "mp_prev", "mp_next", "mp_load", "mp_none", "mp_status"):
    assert k in IDX and IDX[k] >= OLD_PARAMS, k

tui_p = os.path.join(SK, "TUI.json")
tui = json.load(open(tui_p))
pd = tui["pageData"]
if any(t["tabName"] == "MAPS" for t in pd["tabs"]):
    sys.exit("patch_maps: this skin already has the MAPS page")

# 1. renumber the hidden popup parameters
pat = re.compile(r"^Parameter (\d+)$")
n_ren = 0


def renum(o):
    global n_ren
    if isinstance(o, dict):
        for k, v in o.items():
            if isinstance(v, str):
                m = pat.match(v)
                if m and int(m.group(1)) >= OLD_PARAMS:
                    o[k] = "Parameter %d" % (int(m.group(1)) + SHIFT)
                    n_ren += 1
            else:
                renum(v)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            if isinstance(v, str):
                m = pat.match(v)
                if m and int(m.group(1)) >= OLD_PARAMS:
                    o[i] = "Parameter %d" % (int(m.group(1)) + SHIFT)
                    n_ren += 1
            else:
                renum(v)


renum(pd)
print("patch_maps: %d popup parameter references renumbered (+%d)" % (n_ren, SHIFT))

defs = pd["componentDefinitions"]["localComponentDefinitions"]
dmap = {d["key"]: d for d in defs}
browse = dmap["BROWSE|BROWSE"]["value"]

# 2. the new buttons' pictures and definitions (LOAD, NONE)
W, H = 150, 72
new_btn = {}
for tag, label in (("maps_load", "LOAD"), ("maps_none", "NONE")):
    on_n, off_n = "rx_btn_%s_%dx%d_on.png" % (tag, W, H), "rx_btn_%s_%dx%d_off.png" % (tag, W, H)
    R.button_face(W, H, label, True).save(os.path.join(SK, on_n))
    R.button_face(W, H, label, False).save(os.path.join(SK, off_n))
    base = dmap["rxBtn_rx_btn_load_1_150x72_on.png_rx_btn_load_1_150x72_off.png"]["value"]
    v = json.loads(json.dumps(base).replace("rx_btn_load_1_150x72_on.png", on_n).replace("rx_btn_load_1_150x72_off.png", off_n))
    key = "rxBtn_%s_%s" % (on_n, off_n)
    defs.append({"key": key, "value": v})
    new_btn[tag] = key

# 3. the page: BROWSE's components, re-pointed
src = browse["componentsData"]
by = {c["componentData"]["name"]: c for c in src}


def clone(name, new_name=None, param=None, type_=None, bounds=None):
    c = copy.deepcopy(by[name])
    if new_name:
        c["componentData"]["name"] = new_name
    if param is not None:
        c["handle remapping"]["map"] = [{"key": "Data", "value": "Parameter %d" % IDX[param]}]
    if type_:
        c["componentData"]["type"] = type_
    if bounds:
        c["bounds"]["bounds"] = bounds
    return c


kids = [copy.deepcopy(by["Background"])]
kids[0]["componentData"]["data"]["image"] = "rx_bg_6.png"
kids.append(clone("br_path", "maps_status", "mp_status", bounds="160 4 690 40"))
kids.append(clone("br_page", "maps_page", "mp_page"))
for i in range(1, 7):                                     # the row texts first, the transparent radio rows above take the touch
    kids.append(clone("br_row%d" % i, "maps_row%d" % i, "mp_row%d" % i))
for i in range(1, 7):
    kids.append(clone("row %d" % i, "map row %d" % i, "mp_row"))
kids.append(clone("load1", "maps_load", "mp_load", type_=new_btn["maps_load"]))
kids.append(clone("load2", "maps_none", "mp_none", type_=new_btn["maps_none"]))
kids.append(clone("br_prev", "maps_prev", "mp_prev"))
kids.append(clone("br_next", "maps_next", "mp_next"))
page = copy.deepcopy(browse)
page["componentsData"] = kids
defs.append({"key": "MAPS|MAPS", "value": page})

# 4. the tab: the second page of the BROWSE key
pd["tabs"].insert([t["tabName"] for t in pd["tabs"]].index("BROWSE") + 1,
                  {"version": 3, "tabName": "MAPS", "fnKeyIndex": 1, "fnKeySubIndex": 1, "qlinkBoundsData": ["0 0 0 0"],
                   "componentName": "MAPS|MAPS", "initialSize": "0 0 1280 628", "scale": 1.0})
json.dump(tui, open(tui_p, "w"), indent=1)

# 5. the background: BROWSE's, with the title and the sort field painted out, MAPS and a hint put on
from PIL import Image, ImageDraw  # noqa: E402
bg = Image.open(os.path.join(SK, "rx_bg_1.png")).convert("RGB")
bg.paste(bg.crop((330, 0, 330 + 330, 47)), (0, 0))                  # the empty title bar
bg.paste(bg.crop((1122, 466, 1272, 538)), (1122, 545))                # plain background where BROWSE has its sort field
bg.paste(bg.crop((332, 466, 772, 538)), (332, 545))           # (the hint goes here)
d = ImageDraw.Draw(bg)
d.text((17, 24), "MAPS", font=R.font(34, True), fill=R.INK, anchor="lm")
d.text((340, 565), "MAP ANY CONTROLLER:", font=R.font(26, True), fill=R.INK_DIM, anchor="lm")
d.text((340, 597), "companion page, then /map", font=R.font(26, True), fill=R.INK_DIM, anchor="lm")
bg.save(os.path.join(SK, "rx_bg_6.png"))

# 6. the wheel / Q-Link bank of the new page (the BROWSE bank, with the page and the list row)
for fn in ("Q-Links.json", "Q-Links - 8by1.json"):
    p = os.path.join(SK, fn)
    q = json.load(open(p))
    m = q["Screen Mode Q-Links"]["map"]
    if any(e["Tab"] == 2 and e["SubTab"] == 2 for e in m):
        continue
    ql = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    ql["Q-Link 5"] = IDX["mp_page"]
    ql["Q-Link 9"] = IDX["mp_row"]
    pos = max(i for i, e in enumerate(m) if e["Tab"] == 2) + 1
    m.insert(pos, {"Tab": 2, "SubTab": 2, "Bank Direction": "Column", "Q-Links": ql})
    json.dump(q, open(p, "w"), indent=4)
print("patch_maps: MAPS page added (second page of the BROWSE key)")
