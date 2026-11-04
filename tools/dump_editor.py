#!/usr/bin/env python3
"""Everything the UI editor needs, read from the real page builder (tools/remix_skin.py), not copied by hand:

    python3 tools/dump_editor.py [out.json]        default: build/Layout/editor_data.json

For each of the seven pages: its elements (id, kind, name, parameter, x y w h, label, the picture of how it looks now, the
four per-deck copies merged into one) and the clean panel texture the page sits on. The editor (tools/make_editor.py) turns
this into one HTML page; what the user does there comes back as layout JSON (x y w h per element id, plus the added ones)."""
import base64
import io
import json
import os
import re
import sys
import tempfile

os.environ["RX_NO_LAYOUT"] = "1"      # the editor starts from the builder's own places, not from a saved layout
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import remix_skin as S  # noqa: E402
import remix_pages as P  # noqa: E402
from PIL import Image  # noqa: E402

PREVIEW = os.path.join(ROOT, "build", "preview")
PNAME = ["deck", "browse", "mixer", "fx", "wheels", "wheel"]


def rect_of(kid):
    return [int(v) for v in kid["bounds"]["bounds"].split()]


def data_idx(kid):
    for m in kid["handle remapping"]["map"]:
        if m["key"] == "Data":
            return int(m["value"].split()[-1])
    return None


def cond_deck(kid):
    c = kid["bounds"]["additionalInvalidatingHandles"]
    if not c:
        return None
    m = re.match(r"IndexedEnabling/(\d+)/(\d+)/", c[0])
    return int(m.group(1)) + 1 if m and int(m.group(2)) == 4 else None


def b64(im, fmt="PNG"):
    bio = io.BytesIO()
    if fmt == "JPEG":
        im.convert("RGB").save(bio, "JPEG", quality=86)
    else:
        im.save(bio, "PNG", optimize=True)
    return "data:image/%s;base64,%s" % ("jpeg" if fmt == "JPEG" else "png", base64.b64encode(bio.getvalue()).decode())


def main(out):
    tmp = tempfile.mkdtemp(prefix="rx_editor_")
    sk = S.Skin(tmp, S.all_params())
    inv = {i: k for k, i in sk.index.items()}
    parts_by_page = {5: P.pages() if False else None}
    pg = sk.pg
    pages_out = []
    clean_bg = None
    for no, (name, fn) in enumerate(zip(S.TABS, S.PAGE_FN)):
        p = S.Page(sk, no)
        if clean_bg is None:
            clean_bg = p.bg.copy()
        els, seen, depth = [], set(), [0]

        def add(kind, nm, key, rect, label="", px=None, deck=None, extra=None):
            e = {"kind": kind, "name": nm, "key": key, "x": int(rect[0]), "y": int(rect[1]), "w": int(rect[2]), "h": int(rect[3]),
                 "label": label, "px": px, "decks": [deck] if deck else []}
            if extra:
                e.update(extra)
            els.append(e)

        def kid_el(kid, kind, label=""):
            seen.add(id(kid))
            idx = data_idx(kid)
            add(kind, kid["componentData"]["name"], inv.get(idx, ""), rect_of(kid), label, None, cond_deck(kid))

        def wrap(fn_, kind, newkids_from="kids"):
            def w(self, *a, **k):
                n0 = len(self.kids)
                depth[0] += 1
                try:
                    r = fn_(self, *a, **k)
                finally:
                    depth[0] -= 1
                new = self.kids[n0:]
                if kind == "popup":
                    new = new[:1]
                for kid in new:
                    if id(kid) not in seen:
                        lab = ""
                        if kind == "button":
                            lab = (a[2] if len(a) > 2 else k.get("label", "")) or ""
                        kid_el(kid, "field" if kind == "popup" else kind, lab)
                return r
            return w

        orig = {m: getattr(S.Page, m) for m in ("text", "black", "paste", "live", "momentary", "radio", "radio_boxes", "knob", "fader",
                                                  "readout", "popup")}

        def text(self, x, y, s, px=S.READ_PX, bold=True, colour=S.INK, anchor="lm"):
            if depth[0] == 0:
                f = S.font(px, bold)
                w_ = int(f.getlength(s)) + 4
                h_ = int(px * 1.3)
                ax = {"l": x, "m": x - w_ / 2, "r": x - w_}[anchor[0]]
                add("text", s, "", (ax, y - h_ / 2, w_, h_), s, px)
            return orig["text"](self, x, y, s, px, bold, colour, anchor)

        def black(self, rect, radius=0):
            if depth[0] == 0:
                add("box", "black box (live picture drawn here)", "", rect)
            return orig["black"](self, rect, radius)

        def paste(self, im, x, y):
            if depth[0] == 0:
                add("part", "part", "", (x, y, im.size[0], im.size[1]))
            return orig["paste"](self, im, x, y)

        def live(self, key, rect, *a, **k):
            n0 = len(self.kids)
            depth[0] += 1
            try:
                r = orig["live"](self, key, rect, *a, **k)
            finally:
                depth[0] -= 1
            for kid in self.kids[n0:]:
                if id(kid) not in seen:
                    kid_el(kid, "readout")
            return r

        def readout(self, key, rect, nm, *a, **k):
            n0 = len(self.kids)
            depth[0] += 1
            try:
                r = orig["readout"](self, key, rect, nm, *a, **k)
            finally:
                depth[0] -= 1
            for kid in self.kids[n0:]:
                seen.add(id(kid))
            add("readout", "%s (name + value)" % nm, key, rect, nm, S.READ_PX, cond_deck(self.kids[-1]) if self.kids else None)
            return r

        S.Page.text, S.Page.black, S.Page.paste = text, black, paste
        S.Page.live, S.Page.readout = live, readout
        S.Page.momentary = wrap(orig["momentary"], "button")
        S.Page.toggle = S.Page.momentary
        S.Page.radio = wrap(orig["radio"], "option")
        S.Page.radio_boxes = wrap(orig["radio_boxes"], "option")
        S.Page.knob = wrap(orig["knob"], "knob")
        S.Page.fader = wrap(orig["fader"], "slider")
        S.Page.popup = wrap(orig["popup"], "popup")
        try:
            fn(p)
        finally:
            for m, f in orig.items():
                setattr(S.Page, m, f)
            S.Page.toggle = orig["momentary"]
        for kid in p.kids:                                    # placed straight into kids (rows, tempo faders, jog feel)
            if id(kid) not in seen:
                t = kid["componentData"]["type"]
                kind = "row" if t.startswith("rxRow") else "slider" if t.startswith("rxTempo") else "knob" if t.startswith("rxJog") else "button"
                kid_el(kid, kind)

        # the wheel parts own the controls that sit on them
        parts = [e for e in els if e["kind"] == "part"]
        if parts:
            pm = {}
            for side in ("left", "right", "centre"):
                for key in ("wheels", "wheel"):
                    if side in pg.get(key, {}):
                        for q in pg[key][side]["parts"]:
                            pm[(q["x"], q["y"])] = q["id"]
            for e in parts:
                pid = pm.get((e["x"], e["y"]))
                e["pid"] = pid
                e["name"] = "part %s: %s" % (pid, P.PART_NAMES.get(pid, ""))
            kept = []
            for e in els:
                if e["kind"] in ("button", "slider", "knob", "field") and any(
                        q["x"] <= e["x"] and q["y"] <= e["y"] and e["x"] + e["w"] <= q["x"] + q["w"] and e["y"] + e["h"] <= q["y"] + q["h"] for q in parts):
                    continue                                   # moves with its part
                kept.append(e)
            els = kept

        # merge the four per-deck copies (same place, same look) into one
        merged, idx = [], {}
        for e in els:
            base = re.sub(r"\bd\d_", "d*_", e["key"] or e["name"])
            mk = (e["kind"], e["x"], e["y"], e["w"], e["h"], e["label"] or base)
            if mk in idx:
                idx[mk]["decks"] = sorted(set(idx[mk]["decks"] + e["decks"]))
                continue
            idx[mk] = e
            merged.append(e)
        # a name for each
        im = Image.open(os.path.join(PREVIEW, PNAME[no] + ".png")).convert("RGB")
        for i, e in enumerate(merged):
            e["id"] = "%s.%03d" % (PNAME[no], i + 1)
            if e["kind"] in ("button", "option", "slider", "knob", "readout", "field", "row", "text", "part", "box"):
                nm = e["key"] or e["name"]
                e["name"] = re.sub(r"\bd(\d)_", "d*_", nm) if e["key"] else e["name"]
            x0, y0 = max(0, e["x"]), max(0, e["y"])
            x1, y1 = min(S.W, e["x"] + e["w"]), min(S.H, e["y"] + e["h"])
            if x1 > x0 and y1 > y0:
                e["img"] = b64(im.crop((x0, y0, x1, y1)), "PNG" if e["w"] * e["h"] < 60000 else "JPEG")
        pages_out.append({"name": name, "elements": merged})
        print("%-8s %3d elements" % (name, len(merged)))
    data = {"version": 1, "size": [S.W, S.H], "bg": b64(clean_bg, "JPEG"), "pages": pages_out,
            "min_text_px": 26}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(data, open(out, "w"))
    print("dump_editor: %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "Layout", "editor_data.json"))
