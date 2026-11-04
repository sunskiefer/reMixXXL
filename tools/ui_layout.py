#!/usr/bin/env python3
"""The user's layout (art/ui/reMixXXL_layout.json, saved by the UI editor) applied to the page builder.

The file is the one place positions live: every control of DECK, BROWSE, MIXER and FX sits where the user put it in the
editor (x y w h per element), deleted ones are not drawn, added ones are built from art/ui/bindings.json (which parameter an
added button / readout drives), and the user's own knob picture replaces the knob art. Nothing here moves a part by itself:
an element the layout does not mention keeps its place.

An element is found by (kind, its original x y w h, its parameter or text), the same triple the editor's dump used, so the
file keeps working while the builder code stays as it is.

WHEELS and WHEEL are NOT applied yet (the user is still editing the jog wheel panels): APPLY_PAGES says which pages are."""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAYOUT = os.path.join(ROOT, "art", "ui", "reMixXXL_layout.json")
BINDINGS = os.path.join(ROOT, "art", "ui", "bindings.json")
APPLY_PAGES = {"DECK", "BROWSE", "MIXER", "FX"}
BLACK_NAME = "black box (live picture drawn here)"


def base(k):
    return re.sub(r"\bd\d_", "d*_", k or "")


class Layout:
    def __init__(self, path=LAYOUT):
        self.ok = os.path.exists(path) and not os.environ.get("RX_NO_LAYOUT")
        self.by, self.added, self.knobs = {}, {}, {}
        if not self.ok:
            return
        d = json.load(open(path))
        for pg, items in d["pages"].items():
            by, add = {}, []
            for i in items:
                if i.get("added"):
                    add.append(i)
                else:
                    by[(i["kind"], tuple(i["orig"]), i["name"])] = i
            self.by[pg], self.added[pg] = by, add
            self.knobs[pg] = self._pair_knobs(by, add)
            self._tidy(pg, items, by, add, self.knobs[pg])
        self.bind = json.load(open(BINDINGS)) if os.path.exists(BINDINGS) else {}

    # -- the user's layout is a sketch: line it up (same x in a column, same y in a row, even gaps) around where the user put things
    @staticmethod
    def _tidy(pg, items, by, add, pairs):
        live = [i for i in items if not i.get("deleted")]

        def nm(i): return i.get("name", "")

        def med(v):
            v = sorted(v); return v[len(v) // 2]

        if pg == "MIXER":
            col = sorted([i for i in live if i["kind"] == "knob" and nm(i) in ("d*_gain", "d*_hi", "d*_mid", "d*_low") and i["x"] < 250],
                         key=lambda i: i["y"])
            if len(col) >= 3:
                x = int(med([i["x"] for i in col])); y0, y1 = col[0]["y"], col[-1]["y"]
                for k, i in enumerate(col):
                    i["x"] = x; i["y"] = int(round(y0 + (y1 - y0) * k / (len(col) - 1)))
            xf = [i for i in live if nm(i) == "xfader"]
            if xf:
                cy = xf[0]["y"] + xf[0]["h"] / 2.0
                for i in live:
                    if (i["kind"] == "text" and nm(i) == "XFADER") or (i.get("added") and i["kind"] == "button" and i["y"] > 540):
                        i["y"] = int(round(cy - i["h"] / 2.0))
        if pg == "FX":
            def row(n):
                names = {"UNIT %d" % n, "fx%d_on" % n, "fx%d_mix" % n, "fx%d_super" % n, "fx%d_master" % n, "d*_fx%d" % n}
                r = [i for i in live if nm(i) in names or (nm(i) == "DECKS" and i["kind"] == "text" and (i["y"] < 400) == (n == 1))]
                return sorted(r, key=lambda i: (i["x"], i["y"]))
            r1, r2 = row(1), row(2)
            main1 = [i for i in r1 if nm(i) != "d*_fx1"]
            if len(main1) >= 3:   # even gaps between the first and the last of the unit row
                x0 = main1[0]["x"]; x1 = main1[-1]["x"] + main1[-1]["w"]
                gap = (x1 - x0 - sum(i["w"] for i in main1)) / float(len(main1) - 1)
                x = float(x0)
                for i in main1:
                    i["x"] = int(round(x)); x += i["w"] + gap
            if len(r1) == len(r2):   # unit 2's row is unit 1's row, lower by the same step the user gave its knobs
                for a, b in zip(r1, r2):
                    b["x"], b["w"], b["h"] = a["x"], a["w"], a["h"]; b["y"] = a["y"] + 276
        if pg == "BROWSE":   # each row's text sits exactly on its row button, and the first row is not left without text
            rows = sorted([i for i in items if i["kind"] == "row" and not i.get("deleted")], key=lambda i: i["y"])
            for k, r in enumerate(rows):
                for i in items:
                    if i["kind"] == "readout" and nm(i) == "br_row%d" % (k + 1):
                        i.update(x=r["x"] + 14, y=r["y"], w=r["w"] - 28, h=r["h"], deleted=False)
        # the user's knob picture sits centred on its knob, all of one size at the same height from the knob
        groups = {}
        for k, pic in pairs.items():
            it = by[k]
            if not it.get("deleted"):
                groups.setdefault(int(pic["w"]), []).append((it, pic))
        for w, lst in groups.items():
            off = med([pic["y"] - it["y"] for it, pic in lst])
            for it, pic in lst:
                pic["x"] = it["x"] + (it["w"] - pic["w"]) // 2
                pic["y"] = it["y"] + off

    @staticmethod
    def _pair_knobs(by, add):
        """the user's picture over each knob: nearest centre, one picture per knob, within 60 px"""
        imgs = [a for a in add if a["kind"] == "image" and not a.get("deleted") and 40 <= a["w"] <= 110 and 40 <= a["h"] <= 110
                and "knob" in (a.get("label") or a.get("name") or "").lower()]
        knobs = [(k, it) for k, it in by.items() if k[0] == "knob" and not it.get("deleted")]
        pairs = []
        for k, it in knobs:
            kx, ky = it["x"] + it["w"] / 2, it["y"] + 28
            for j, im in enumerate(imgs):
                dx, dy = im["x"] + im["w"] / 2 - kx, im["y"] + im["h"] / 2 - ky
                pairs.append((dx * dx + dy * dy, k, j))
        pairs.sort()
        out, used_k, used_i = {}, set(), set()
        for dd, k, j in pairs:
            if dd > 60 * 60:
                break
            if k in used_k or j in used_i:
                continue
            used_k.add(k); used_i.add(j)
            out[k] = imgs[j]
        return out

    # -- lookups
    def item(self, page, kind, orig, name):
        if not self.ok or page not in APPLY_PAGES:
            return None
        return self.by.get(page, {}).get((kind, tuple(int(v) for v in orig), name))

    def rect(self, page, kind, name, rect):
        """-> the user's rect for this element, the same rect if the layout does not mention it, None if deleted"""
        it = self.item(page, kind, rect, name)
        if not it:
            return tuple(int(v) for v in rect)
        if it.get("deleted"):
            return None
        return (it["x"], it["y"], it["w"], it["h"])

    def knob(self, page, orig, name):
        """-> (picture item or None, layout item or None)"""
        it = self.item(page, "knob", orig, name)
        if not it:
            return None, None
        return self.knobs.get(page, {}).get(("knob", tuple(it["orig"]), it["name"])), it


LAY = None


def get():
    global LAY
    if LAY is None:
        LAY = Layout()
    return LAY


def apply_pages(out):
    """the geometry the overlay shares with the skin (pages.json -> src/boxes.h): the DECK waveform and overview boxes"""
    L = get()
    if not L.ok:
        return
    for key, name in (("wave", "wave"), ("over", "over")):
        b = out["deck"][key]
        it = L.item("DECK", "box", (b["x"], b["y"], b["w"], b["h"]), BLACK_NAME)
        if it and not it.get("deleted"):
            b.update(x=it["x"], y=it["y"], w=it["w"], h=it["h"])


# ------------------------------------------------------------------------------------------------ the page-builder hooks
def install(S):
    """wrap remix_skin.Page so every placement asks the layout first (see the module doc)"""
    L = get()
    if not L.ok:
        return
    Page = S.Page
    orig = {m: getattr(Page, m) for m in ("text", "black", "momentary", "radio", "radio_boxes", "knob", "fader", "readout", "live", "popup")}
    depth = [0]

    def active(p):
        return depth[0] == 0 and p.name in APPLY_PAGES

    def inv(p):
        if not hasattr(p.sk, "_inv"):
            p.sk._inv = {i: k for k, i in p.sk.index.items()}
        return p.sk._inv

    def rect_of(kid):
        return [int(v) for v in kid["bounds"]["bounds"].split()]

    def patch_kids(p, n0, kind):
        """after a call that appended components: move / drop each by the layout (by its own original rect and parameter)"""
        keep = []
        for kid in p.kids[n0:]:
            idx = None
            for m in kid["handle remapping"]["map"]:
                if m["key"] == "Data":
                    idx = int(m["value"].split()[-1])
            nm = base(inv(p).get(idx, ""))
            r = rect_of(kid)
            nr = L.rect(p.name, kind, nm, r)
            if nr is None:
                continue
            kid["bounds"]["bounds"] = S.B(*nr)
            keep.append(kid)
        p.kids[n0:] = keep

    def kids_wrap(name, kind):
        fn = orig[name]

        def w(self, *a, **k):
            act = active(self)
            n0 = len(self.kids)
            depth[0] += 1
            try:
                r = fn(self, *a, **k)
            finally:
                depth[0] -= 1
            if act:
                patch_kids(self, n0, "field" if kind == "popup" else kind)
            return r
        return w

    def text(self, x, y, s, px=S.READ_PX, bold=True, colour=S.INK, anchor="lm"):
        if active(self):
            f = S.font(px, bold)
            w_, h_ = int(f.getlength(s)) + 4, int(px * 1.3)
            ax = {"l": x, "m": x - w_ / 2, "r": x - w_}[anchor[0]]
            r = (int(ax), int(y - h_ / 2), w_, h_)
            it = L.item(self.name, "text", r, s)
            if it:
                if it.get("deleted"):
                    return
                if it.get("label") not in (None, s):
                    s = it["label"]
                if it.get("px"):
                    px = max(S.READ_PX, float(it["px"]))
                nx, ny, nw, nh = it["x"], it["y"], it["w"], it["h"]
                f2 = S.font(px, bold)
                w2 = int(f2.getlength(s)) + 4 if it.get("label") not in (None, "") else nw
                x = {"l": nx, "m": nx + w2 / 2, "r": nx + w2}[anchor[0]]
                y = ny + nh / 2
        return orig["text"](self, x, y, s, px, bold, colour, anchor)

    def black(self, rect, radius=0):
        if active(self):
            nr = L.rect(self.name, "box", BLACK_NAME, rect)
            if nr is None:
                return
            rect = nr
        return orig["black"](self, rect, radius)

    def live(self, key, rect, *a, **k):
        if active(self):
            nr = L.rect(self.name, "readout", base(key), rect)
            if nr is None:
                return
            rect = nr
        depth[0] += 1
        try:
            return orig["live"](self, key, rect, *a, **k)
        finally:
            depth[0] -= 1

    def readout(self, key, rect, nm, *a, **k):
        if active(self):
            nr = L.rect(self.name, "readout", base(key), rect)
            if nr is None:
                return
            rect = nr
        depth[0] += 1
        try:
            return orig["readout"](self, key, rect, nm, *a, **k)
        finally:
            depth[0] -= 1

    def popup(self, key, rect, cond=None, list_at=None, field_img=None, caption=None):
        if active(self):
            nr = L.rect(self.name, "field", base(key + "__open"), rect)
            if nr is None:
                return
            if list_at is not None:
                list_at = (list_at[0] + nr[0] - rect[0], list_at[1] + nr[1] - rect[1])
            rect = nr
        depth[0] += 1
        try:
            return orig["popup"](self, key, rect, cond=cond, list_at=list_at, field_img=field_img, caption=caption)
        finally:
            depth[0] -= 1

    def knob(self, key, x, y, dia=56, name=None, live_name=False, cond=None, cell_w=None, value=False, art=None):
        if active(self):
            cw = cell_w or max(dia + 40, 96)
            h = dia + 2 + 30 + (30 if value else 0)
            pic, it = L.knob(self.name, (x, y, cw, h), base(key))
            m = re.match(r"d([2-4])_(gain|hi|mid|low)$", key)
            if self.name == "MIXER" and m and pic is None:
                # decks 2-4 go exactly like deck 1: same knob picture, same place inside its column (FILTER stays as it is)
                n = int(m.group(1))
                chs = self.sk.pg["mixer"]["channels"]
                dx = chs[n - 1]["x"] - chs[0]["x"]
                pic1, it1 = L.knob(self.name, (x - dx, y, cw, h), base(key))
                if it1 is not None and pic1 is not None:
                    x, y = it1["x"] + dx, it1["y"]
                    art = (int(pic1["x"]) + dx, int(pic1["y"]), int(pic1["w"]))
                    pic, it = None, None
            if it is not None:
                if it.get("deleted"):
                    return
                x, y = it["x"], it["y"]
                if pic is not None:                         # the user's picture sits where the user put it
                    art = (int(pic["x"]), int(pic["y"]), int(pic["w"]))
        depth[0] += 1
        try:
            return orig["knob"](self, key, x, y, dia, name, live_name, cond, cell_w, value, art)
        finally:
            depth[0] -= 1

    Page.text, Page.black, Page.live, Page.readout, Page.popup, Page.knob = text, black, live, readout, popup, knob
    Page.momentary = kids_wrap("momentary", "button")
    Page.toggle = Page.momentary
    Page.radio = kids_wrap("radio", "option")
    Page.radio_boxes = kids_wrap("radio_boxes", "option")
    Page.fader = kids_wrap("fader", "slider")


def at(p, kind, name, rect):
    """for the places that add components straight to p.kids (the browse rows): the user's rect, or None if deleted"""
    if p.name not in APPLY_PAGES:
        return tuple(rect)
    return get().rect(p.name, kind, base(name), rect)


def added(p, S):
    """build this page's added items (art/ui/bindings.json says what each drives); reports the ones with nothing to drive"""
    L = get()
    if not L.ok or p.name not in APPLY_PAGES:
        return
    for it in L.added.get(p.name, []):
        if it.get("deleted"):
            continue
        rect = (int(it["x"]), int(it["y"]), int(it["w"]), int(it["h"]))
        b = L.bind.get(it["id"])
        kind = it["kind"]
        if kind == "image":
            if L.knobs and any(v is it for v in L.knobs.get(p.name, {}).values()):
                continue                                    # the knob picture: used by its knob
            if "knob" in (it.get("label") or "").lower():
                print("ui_layout: %s: knob picture %s sits over no knob (left out)" % (p.name, it["id"]))
                continue
            src = it.get("src", "")
            path = os.path.join(ROOT, "art", "ui", "assets", src.split("/_blob/")[-1] + ".png") if src else ""
            if path and os.path.exists(path):
                from PIL import Image
                im = Image.open(path).convert("RGBA").resize((rect[2], rect[3]), Image.LANCZOS)
                p.paste(im, rect[0], rect[1])
            continue
        if not b:
            print("ui_layout: %s: added %s %r at %s has nothing to drive (art/ui/bindings.json); left out. note: %r" % (
                p.name, kind, it.get("label"), rect, it.get("note")))
            continue
        t, key = b["type"], b["param"]
        if t == "button":
            p.momentary(key, rect, it.get("label") or b.get("label", ""))
        elif t == "live":
            p.black(rect)
            p.live(key, rect, S.READ_PX, "horizontallyCentred verticallyCentred", "Bold", kind="Value")
        elif t == "popup":
            p.popup(key, rect, caption=b.get("caption"))
        elif t == "text":
            p.text(rect[0], rect[1] + rect[3] / 2, it.get("label", ""), float(it.get("px") or S.READ_PX))
