#!/usr/bin/env python3
"""reMixXXL's six pages (MPC tabs DECK, BROWSE, MIXER, FX, WHEELS, WHEEL) on MPC's 1280 x 628 plugin
screen: the builder that draws them and writes every control into the skin's TUI.json.

    python3 tools/remix_skin.py --layout                 writes layout.conf (the tabs, their popups and Q-Links, for gen_vst.py)
    python3 tools/remix_skin.py "<skin>/Plugin Skins"    builds every page into the skin gen_vst.py made
    python3 tools/remix_skin.py --preview "<skin>/Plugin Skins" <out dir>
                                                        composites every page into <out dir>/<page>.png (sample live text)
    python3 tools/remix_skin.py --overlays "<skin>/Plugin Skins" <out dir>
                                                        the wheel pages with every part's id drawn on it (Layout/*.png)

Where things sit comes from build/pages.json (tools/remix_pages.py): every box, part and button position is the user's;
nothing here moves them. The art is the user's: art/panel_source.jpg (the panel texture, every page's background),
art/platter_nobrand.png + art/control_map.json (the CDJ parts, cropped and pasted at their boxes on the wheel pages),
art/button_face.png (the plain button face the CDJ-style buttons are made of). Pixels are the source of truth: parts
are cropped and scaled (LANCZOS), never redrawn; the only paint on them is the orange "lit" tint of an ON state.

Text sizes (the standing rule): live labels are Titillium Web, names SemiBold 35.7 px in a 221 x 48 box where there is
room, values 37.4 px in 221 x 44; readouts / header text 26-34 px; list rows 26 px Bold; nothing under 26 px.
Baked text uses the same Titillium Web files (third_party/fonts) at the same sizes. A name that does not fit is
shortened (RESONANCE -> RES style), never shrunk.

Per-deck controls (DECK, WHEELS, WHEEL) are four stacked components at the same place, one per deck, each shown only
while the page's deck selector (deck_sel / wheel_l / wheel_r) is at that deck (MPC's IndexedEnabling condition).
The waveform boxes, VU boxes and the platter's centre label are left black: the live overlay (src/overlay.c) draws
there, finding the page by the 2 x 2 marker pixels baked at page_mark_tl / page_mark_br."""
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "third_party", "mpc-vst-plugins", "tools"))

sys.path.insert(0, os.path.join(ROOT, "tools"))
import ui_layout  # noqa: E402  the user's layout (art/ui/reMixXXL_layout.json)

W, H = 1280, 628
FONT_B = os.path.join(ROOT, "third_party", "fonts", "TitilliumWeb-Bold.ttf")
FONT_S = os.path.join(ROOT, "third_party", "fonts", "TitilliumWeb-SemiBold.ttf")
ART = os.path.join(ROOT, "art")
PAGES_JSON = os.path.join(ROOT, "build", "pages.json")
LAYOUT = os.path.join(ROOT, "layout.conf")

ACCENT = (201, 106, 60)            # the CDJ's orange (the cue ring): lit buttons, knob pointers, the selected deck
INK = (240, 240, 240)
INK_DIM = (170, 170, 170)
BLACK = (0, 0, 0)
STRIPE_DARK = 0.55                  # the top stripe: the texture, this much darker

# the text sizes (px = MPC font height; the same numbers for baked text)
NAME_PX, NAME_W, NAME_H = 35.7, 221, 48
VALUE_PX, VALUE_W, VALUE_H = 37.4, 221, 44
READ_PX = 26.0                      # readouts, list rows, the stripe's live text
HEAD_PX = 34                        # the page name (26 where the stripe is crowded: WHEELS)
BTN_PX = 28                         # CDJ-style button labels (Bold)
KNOB_NAME_PX = 28.0                 # a knob's name where a 221 px box has no room (mixer, fx)
FRAMES = 101
STRIP_MAX = 12288                   # MPC's limit on a picture's height: a filmstrip's frames * frame height

TABS = ["DECK", "BROWSE", "MIXER", "FX", "WHEELS", "WHEEL"]
DECKS = 4

# DECK rows: pages.json key -> our parameter (d<n>_...)
DECK_KEYS = {"play": "play", "cue_default": "cue", "beatsync": "sync", "slip_enabled": "slip", "reverse": "rev",
             "keylock": "keylock", "quantize": "quant", "vinyl_mode": "vinyl", "loop_in": "loop_in", "loop_out": "loop_out",
             "reloop_toggle": "reloop", "loop_halve": "halve", "loop_double": "double", "beatjump_backward": "bjback",
             "beatjump_forward": "bjfwd", "eject": "eject"}
for _h in range(1, 9):
    DECK_KEYS["hotcue_%d_activate" % _h] = "hc%d" % _h
DECK_LABELS = {"play": "PLAY", "cue": "CUE", "sync": "SYNC", "slip": "SLIP", "rev": "REV", "keylock": "KEYLOCK", "quant": "QUANT",
               "vinyl": "VINYL", "loop_in": "LOOP IN", "loop_out": "LOOP OUT", "reloop": "RELOOP", "halve": "LOOP /2",
               "double": "LOOP X2", "bjback": "< JUMP", "bjfwd": "JUMP >", "eject": "EJECT"}
for _h in range(1, 9):
    DECK_LABELS["hc%d" % _h] = "CUE %d" % _h
TOGGLES = {"play", "slip", "rev", "keylock", "quant", "vinyl", "master", "pfl", "fx1", "fx2"}   # the rest of the deck keys are momentary

# the wheel parts: id -> [(half, our key, kind)]; half: None (whole), "L", "R"
PART_CTL = {
    3: [(None, "cue", "trigger")], 4: [(None, "play", "toggle")], 5: [(None, "rate", "vfader")],
    6: [(None, "rate_reset", "trigger")], 7: [(None, "keylock", "toggle")], 8: [(None, "rate_range", "popup")],
    9: [("L", "bl4", "trigger"), ("R", "bl8", "trigger")], 10: [("L", "loop_in", "trigger"), ("R", "loop_out", "trigger")],
    11: [(None, "reloop", "trigger")], 12: [("L", "bjback", "trigger"), ("R", "bjfwd", "trigger")],
    13: [("L", "slip", "toggle"), ("R", "rev", "toggle")], 14: [("L", "prev", "trigger"), ("R", "next", "trigger")],
    15: [("L", "back", "trigger"), ("R", "fwd", "trigger")], 16: [(None, "jogfeel", "rknob")],
    17: [("L", "vinyl", "toggle")], 18: [("L", "sync", "trigger"), ("R", "master", "toggle")],
    19: [(None, "keysync", "trigger")]}

# the Q-Link readouts on the wheel pages (the bank's knobs that have no part of their own): short names
WHEEL_READ = [("filter", "FLT"), ("vol", "VOL"), ("gain", "GAIN"), ("hi", "HI"), ("mid", "MID"), ("low", "LOW")]
DECK_READ = [("rate", "TEMPO"), ("filter", "FILTER"), ("gain", "GAIN"), ("hi", "HIGH"), ("mid", "MID"), ("low", "LOW"),
             ("blsize", "LOOP"), ("bjsize", "JUMP")]

# popups: key -> (columns, option width, option height)
POPUPS = {"zoom": (1, 200, 46), "br_sort": (1, 200, 46), "xf_mode": (1, 200, 46)}
for _n in range(1, 5):
    POPUPS["d%d_rate_range" % _n] = (1, 200, 46)
for _u in (1, 2):
    for _s in (1, 2, 3):
        POPUPS["fx%d_%d_type" % (_u, _s)] = (3, 200, 46)


def pages():
    return json.load(open(PAGES_JSON))


# ------------------------------------------------------------------------------------------------ Q-Link banks
def qlink_sets(tab):
    d = lambda n, k: "d%d_%s" % (n, k)
    if tab == "DECK":
        return [("DECK %d" % n, [d(n, k) for k in ("rate", "filter", "gain", "hi", "mid", "low", "blsize", "bjsize")] + ["zoom"])
                for n in range(1, 5)]
    if tab == "BROWSE":
        return [("BROWSE", ["br_scroll", "br_row", "br_page", "br_sort"])]
    if tab == "MIXER":
        return [("MIXER 1", [d(n, "vol") for n in range(1, 5)] + [d(n, "filter") for n in range(1, 5)]),
                ("MIXER 2", [d(n, "gain") for n in range(1, 5)] + [d(n, "hi") for n in range(1, 5)]),
                ("MIXER 3", [d(n, "mid") for n in range(1, 5)] + [d(n, "low") for n in range(1, 5)]),
                ("MASTER", ["master_gain", "head_mix", "xfader"])]
    if tab == "FX":
        f = lambda u, s, k: "fx%d_%d_%s" % (u, s, k)
        return [("FX 1", [f(1, s, "meta") for s in (1, 2, 3)] + ["fx1_mix", "fx1_super"]),
                ("FX 2", [f(2, s, "meta") for s in (1, 2, 3)] + ["fx2_mix", "fx2_super"]),
                ("FX 1 PARAMS", [f(1, s, "p%d" % p) for s in (1, 2, 3) for p in (1, 2, 3, 4)]),
                ("FX 2 PARAMS", [f(2, s, "p%d" % p) for s in (1, 2, 3) for p in (1, 2, 3, 4)])]
    if tab in ("WHEELS", "WHEEL"):
        return [("%s %d" % (tab, n), [d(n, k) for k in ("rate", "jogfeel", "filter", "vol", "gain", "hi", "mid", "low")])
                for n in range(1, 5)]
    return []


def tab_popups(tab):
    if tab == "DECK":
        return ["zoom"]
    if tab == "BROWSE":
        return ["br_sort"]
    if tab == "MIXER":
        return ["xf_mode"]
    if tab == "FX":
        return ["fx%d_%d_type" % (u, s) for u in (1, 2) for s in (1, 2, 3)]
    if tab in ("WHEELS", "WHEEL"):
        return ["d%d_rate_range" % n for n in range(1, 5)]
    return []


def write_layout():
    """layout.conf for gen_vst.py: the tabs in order, the popups (so the wrapper gets their hidden <key>__open params),
    invisible 12 px knobs carrying every Q-Linked key, and the qlinks banks. remix_skin.py then replaces every control."""
    L = ["# reMixXXL -- the tabs, popups and Q-Links for mpc-vst-plugins (GENERATED by tools/remix_skin.py --layout: edit that).",
         "# tools/remix_skin.py builds every control of the seven pages itself; the knobs below only carry the Q-Links,",
         "# the popup lines only ask the wrapper for the hidden <key>__open parameters.",
         "theme_bg=101010", "theme_panel=101010", "theme_ink=f0f0f0", "theme_accent=c96a3c", ""]
    for name in TABS:
        L.append("[tab %s]" % name)
        keys = []
        for _, ks in qlink_sets(name):
            for k in ks:
                if k not in keys:
                    keys.append(k)
        for i, k in enumerate(keys):
            L.append('knob cx=%d cy=%d r=12 label="" key=%s ns=0 bw=30' % (24 + (i % 30) * 40, 110 + (i // 30) * 40, k))
        for i, k in enumerate(tab_popups(name)):
            L.append('popup cx=%d cy=%d w=150 h=30 label="" key=%s' % (100 + i * 160, 300, k))
        for qn, ks in qlink_sets(name):
            L.append('qlinks "%s" = %s' % (qn, ",".join(ks)))
        L.append("")
    open(LAYOUT, "w").write("\n".join(L))
    print("layout.conf written")


def all_params():
    """params.json plus the hidden popup params gen_vst.py appends (same call, same order: the indices match build/params.h)"""
    import shadow_skin
    plist = json.load(open(os.path.join(ROOT, "params.json")))["params"]
    return plist + shadow_skin.popup_params(LAYOUT, plist)


# ------------------------------------------------------------------------------------------------ drawing helpers
def _pil():
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    return Image, ImageDraw, ImageFilter, ImageFont


_fonts = {}


def font(px, bold=True):
    _, _, _, ImageFont = _pil()
    k = (int(round(px)), bold)
    if k not in _fonts:
        _fonts[k] = ImageFont.truetype(FONT_B if bold else FONT_S, k[0])
    return _fonts[k]


def fits(text_, w, px, bold=True):
    return font(px, bold).getlength(text_) <= w


_art = {}


def art_img(name):
    Image, _, _, _ = _pil()
    if name not in _art:
        _art[name] = Image.open(os.path.join(ART, name)).convert("RGBA")
    return _art[name]


def control_map():
    import remix_pages
    return remix_pages.control_map()      # the user's controls sheet: id -> box on the sheet


SHEET_IMG = os.path.join(ART, "ui", "controls", "controls_user.png")


def part_crop(pid, w, h, pg=None):
    """part id's crop of the user's controls sheet (transparent round it), scaled to w x h; the platter (id 1) is drawn by jog_wheel"""
    Image, _, _, _ = _pil()
    b = control_map()[pid]
    src = Image.open(SHEET_IMG).convert("RGBA")
    return src.crop((b["x"], b["y"], b["x"] + b["w"], b["y"] + b["h"])).resize((w, h), Image.LANCZOS)


def tint_lit(im, half=None, colour=ACCENT):
    """the ON look of a part: its bright pixels (the printed symbols, lamps and rings) turned orange, plus a thin orange
    frame round the part so a dark part (a lamp that is off in the photo) reads as lit too; one half only when the part
    is two buttons. The pixels under it are the user's crop: nothing else is changed."""
    import numpy as np
    Image, ImageDraw, _, _ = _pil()
    a = np.asarray(im.convert("RGBA")).astype(np.float32)
    lum = a[..., :3].mean(axis=2)
    w = a.shape[1]
    sel = lum > 95
    t = np.clip((lum - 95) / 120.0, 0, 1)[..., None]
    col = np.array(colour, np.float32)
    out = a.copy()
    out[..., :3] = np.where(sel[..., None], a[..., :3] * (1 - t) + col * t * (lum[..., None] / 180.0).clip(0.6, 1.4), a[..., :3])
    out = np.clip(out, 0, 255).astype("uint8")
    return Image.fromarray(out, "RGBA")          # no frame round it: the lit look is the art's own bright pixels turned orange


def button_face(w, h, label, on, px=BTN_PX, bold=True):
    """a CDJ-style button: art/button_face.png scaled to the box, the label baked on it; ON: orange text and edge"""
    Image, ImageDraw, _, _ = _pil()
    im = art_img("button_face.png").resize((w, h), Image.LANCZOS).copy()
    d = ImageDraw.Draw(im)
    edge = ACCENT if on else (70, 70, 70)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=6, outline=edge + (255,), width=2)
    if label:
        p = px
        while p > 26 and not fits(label, w - 12, p, bold):
            p -= 1
        if not fits(label, w - 12, p, bold):
            sys.exit("remix_skin: button label %r does not fit %d px at 26 px: shorten it" % (label, w))
        d.text((w / 2, h / 2 + 1), label, font=font(p, bold), fill=(ACCENT if on else INK) + (255,), anchor="mm")
    return im


KNOB_ART = os.path.join(ART, "ui", "knob.png")       # the user's knob (the pointer at about -17 degrees from straight up)
KNOB_ART_POINTER = -17.0


def knob_strip_art(dia, frames=FRAMES):
    """the user's own knob picture turned in place: -135 .. +135 degrees over the frames (the whole picture turns)"""
    Image, _, _, _ = _pil()
    src = Image.open(KNOB_ART).convert("RGBA")
    big = Image.new("RGBA", (src.width * 3, src.height * 3), (0, 0, 0, 0))
    big.alpha_composite(src.resize((src.width * 3, src.height * 3), Image.BICUBIC), (0, 0))
    strip = Image.new("RGBA", (dia, dia * frames), (0, 0, 0, 0))
    for k in range(frames):
        ang = -135 + 270 * k / (frames - 1)
        fr = big.rotate(-(ang - KNOB_ART_POINTER), resample=Image.BICUBIC, center=(big.width / 2, big.height / 2))
        strip.alpha_composite(fr.resize((dia, dia), Image.LANCZOS), (0, k * dia))
    return strip


def knob_strip(dia, frames=FRAMES):
    """a CDJ-style knob turning in place: the user's picture (art/ui/knob.png) if present, else a dark disc with an orange pointer"""
    if os.path.exists(KNOB_ART):
        return knob_strip_art(dia, frames)
    Image, ImageDraw, _, _ = _pil()
    SS = 4
    s = dia * SS
    strip = Image.new("RGBA", (dia, dia * frames), (0, 0, 0, 0))
    for k in range(frames):
        fr = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        d = ImageDraw.Draw(fr)
        d.ellipse([0, 0, s - 1, s - 1], fill=(14, 14, 14, 255))
        d.ellipse([s * 0.07, s * 0.07, s * 0.93, s * 0.93], fill=(38, 38, 38, 255), outline=(60, 60, 60, 255), width=SS)
        d.ellipse([s * 0.18, s * 0.18, s * 0.82, s * 0.82], fill=(24, 24, 24, 255))
        ang = math.radians(-135 + 270 * k / (frames - 1))
        cx = cy = s / 2
        x1, y1 = cx + math.sin(ang) * s * 0.44, cy - math.cos(ang) * s * 0.44
        x0, y0 = cx + math.sin(ang) * s * 0.14, cy - math.cos(ang) * s * 0.14
        d.line([(x0, y0), (x1, y1)], fill=ACCENT + (255,), width=int(2.2 * SS))
        strip.alpha_composite(fr.resize((dia, dia), Image.LANCZOS), (0, k * dia))
    return strip


def fader_strip(w, h, vertical, frames):
    """a fader with a cap moving on a dark track: the cap at the low end in frame 0"""
    Image, ImageDraw, _, _ = _pil()
    strip = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    capw, caph = (int(w * 0.72), 34) if vertical else (34, int(h * 0.72))
    for k in range(frames):
        fr = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(fr)
        if vertical:
            d.rounded_rectangle([w / 2 - 4, 8, w / 2 + 4, h - 9], radius=4, fill=(8, 8, 8, 255), outline=(50, 50, 50, 255))
            y = (h - caph / 2 - 6) - (h - caph - 12) * k / (frames - 1)
            d.rounded_rectangle([w / 2 - capw / 2, y - caph / 2, w / 2 + capw / 2, y + caph / 2], radius=4,
                                fill=(30, 30, 30, 255), outline=(70, 70, 70, 255), width=2)
            d.line([(w / 2 - capw / 2 + 4, y), (w / 2 + capw / 2 - 4, y)], fill=(235, 235, 235, 255), width=3)
        else:
            d.rounded_rectangle([8, h / 2 - 4, w - 9, h / 2 + 4], radius=4, fill=(8, 8, 8, 255), outline=(50, 50, 50, 255))
            x = capw / 2 + 6 + (w - capw - 12) * k / (frames - 1)
            d.rounded_rectangle([x - capw / 2, h / 2 - caph / 2, x + capw / 2, h / 2 + caph / 2], radius=4,
                                fill=(30, 30, 30, 255), outline=(70, 70, 70, 255), width=2)
            d.line([(x, h / 2 - caph / 2 + 4), (x, h / 2 + caph / 2 - 4)], fill=(235, 235, 235, 255), width=3)
        strip.alpha_composite(fr, (0, k * h))
    return strip


def cdj_fader_strip(w, h, frames):
    """the tempo fader (part 5) as frames: the cap (the lighter bar in the middle of the photo's crop, found by
    brightness) composited at frames positions along the track; + at the bottom (frame frames-1 = the cap at the bottom)"""
    Image, _, _, _ = _pil()
    import numpy as np
    cm = control_map()[5]
    src = Image.open(SHEET_IMG).convert("RGBA").crop((cm["x"], cm["y"], cm["x"] + cm["w"], cm["y"] + cm["h"]))
    a = np.asarray(src.convert("RGB")).astype(np.float32).mean(axis=2)
    col = a[:, int(a.shape[1] * 0.2):int(a.shape[1] * 0.95)].mean(axis=1)
    peak = int(col.argmax())                         # the cap's white line
    top, bot = peak - 24, peak + 18                  # the cap's rows
    xc = 0                                           # the scale (dots, -, 0, +) is its own picture beside the fader on the sheet: the whole width moves
    cap = src.crop((xc, top, src.width, bot))
    # the empty rail: the crop with the cap rows filled from the rows just above it
    track = src.copy()
    fill = src.crop((xc, top - (bot - top), src.width, top))
    track.paste(fill, (xc, top))
    # the rail: between the scale's ends (an inset from the crop's ends)
    y_min, y_max = int(src.height * 0.08), int(src.height * 0.92) - (bot - top)
    strip = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for k in range(frames):
        fr = track.copy()
        y = int(round(y_min + (y_max - y_min) * k / (frames - 1)))
        fr.paste(cap, (xc, y))
        strip.alpha_composite(fr.resize((w, h), Image.LANCZOS), (0, k * h))
    return strip


def cdj_knob_strip(w, h, frames=FRAMES):
    """the jog feel knob (part 16) turning in place: the knob's disc (found round the pointer: its centre is at the
    pointer's lower end) is cut out, the hole under it filled with the disc's own dark rim, and the disc pasted back
    turned -135..+135 degrees"""
    Image, ImageDraw, ImageFilter, _ = _pil()
    import numpy as np
    cm = control_map()[16]
    src = Image.open(SHEET_IMG).convert("RGBA").crop((cm["x"], cm["y"], cm["x"] + cm["w"], cm["y"] + cm["h"]))
    cx, cy, r = 53, 47, 23                   # the knob's body on the sheet's JOG ADJUST part (the ticks and the labels stay still)
    SS = 4
    big = src.resize((src.width * SS, src.height * SS), Image.LANCZOS)
    m = Image.new("L", big.size, 0)
    ImageDraw.Draw(m).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=255)
    disc = big.copy()
    disc.putalpha(m)
    base = big.copy()
    rim = tuple(int(v) for v in np.asarray(src.convert("RGB"))[min(src.height - 1, cy + r - 2), cx]) + (255,)
    ImageDraw.Draw(base).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=(0, 0, 0, 0))   # the hole under the turning disc: clear (the sheet is transparent)
    strip = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for k in range(frames):
        ang = -135 + 270 * k / (frames - 1)
        rot = disc.rotate(-ang, resample=Image.BICUBIC, center=(cx * SS, cy * SS))
        fr = base.copy()
        fr.alpha_composite(rot)
        strip.alpha_composite(fr.resize((w, h), Image.LANCZOS), (0, k * h))
    return strip


# ------------------------------------------------------------------------------------------------ the WHEEL page's right-hand side (0.1.20)
# The WHEEL page's right-hand column is cut from the user's own picture (art/ui/controls/wheel_page_user_0.1.11.png, 1:1; skin = picture + OFF)
# instead of the controls sheet squeezed into the picture's boxes (that made VINYL / CDJ, BEAT SYNC, the knob ... half size and dropped the
# CUE/LOOP CALL / DELETE / MEMORY art and the tempo scale). Only these parts and the static art right of RIGHT_X0 change; the left side is as before.
WHEEL_PIC = os.path.join(ART, "ui", "controls", "wheel_page_user_0.1.11.png")
RIGHT_IDS = (6, 7, 8, 17, 18, 19)                 # the picture's own crops (the knob, 16, is made below; the fader, 5, is as before)
RIGHT_X0 = 880                                    # on the picture: the static art from here to its right edge
KNOB_BOX = (1072, 441, 114, 108)                  # the JOG ADJUST block on the picture (title, dial, LIGHT / HEAVY)
KNOB_BODY = (55.5, 60, 23)                        # the dial in that box (cx, cy, r)
KNOB_SCALE = 1.1                                  # the knob block's size on the page, against the picture's
KNOB_RIGHT, KNOB_BOTTOM = 988 + 43, 626                # its right edge / bottom on the page: left of the pitch slider, under TEMPO RESET
_wheel_pic = {}


def wheel_pic():
    Image, _, _, _ = _pil()
    if "p" not in _wheel_pic:
        _wheel_pic["p"] = Image.open(WHEEL_PIC).convert("RGBA")
    return _wheel_pic["p"]


def wheel_pic_crop(x, y, w, h):
    """a crop of the picture at page coordinates (x, y, w, h) of a right-hand part (skin = picture + OFF + RIGHT_SHIFT)"""
    import wheel_geom as G
    ox, oy = G.OFF
    ox += G.RIGHT_SHIFT
    return wheel_pic().crop((x - ox, y - oy, x - ox + w, y - oy + h))


def right_static():
    """the picture's right side as ONE picture over the ring (the labels, the CUE/LOOP CALL / DELETE / MEMORY art, the tempo scale ...); the knob
    block and the fader are cleared (they are drawn live, in their own places)"""
    import wheel_geom as G
    Image, _, _, _ = _pil()
    B_ = wheel_pic().copy()
    clear = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    for (x, y, w, h) in (KNOB_BOX, G.FADER):
        B_.paste(Image.new("RGBA", (w, h), (0, 0, 0, 0)), (x, y))
    return B_.crop((RIGHT_X0, 0, B_.width, B_.height)), G.OFF[0] + G.RIGHT_SHIFT + RIGHT_X0, G.OFF[1]


def knob_strip_pic(w, h, frames=FRAMES):
    """the JOG ADJUST block from the picture, scaled to w x h, the dial turning in place -135..+135 degrees (the title and the labels stay still)"""
    Image, ImageDraw, _, _ = _pil()
    x, y, bw, bh = KNOB_BOX
    cx, cy, r = KNOB_BODY
    src = wheel_pic().crop((x, y, x + bw, y + bh))
    SS = 4
    big = src.resize((bw * SS, bh * SS), Image.LANCZOS)
    m = Image.new("L", big.size, 0)
    ImageDraw.Draw(m).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=255)
    disc = big.copy(); disc.putalpha(m)
    base = big.copy()
    ImageDraw.Draw(base).ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=(0, 0, 0, 0))
    strip = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for k in range(frames):
        ang = -135 + 270 * k / (frames - 1)
        rot = disc.rotate(-ang, resample=Image.BICUBIC, center=(cx * SS, cy * SS))
        fr = base.copy(); fr.alpha_composite(rot)
        strip.alpha_composite(fr.resize((w, h), Image.LANCZOS), (0, k * h))
    return strip


# ------------------------------------------------------------------------------------------------ TUI.json pieces
def B(x, y, w, h):
    return "%d %d %d %d" % (round(x), round(y), round(w), round(h))


def sub(name, typ, data, b, visible="Always"):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": []},
            "bounds": {"version": 2, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show", "whenVisible": visible,
                       "boundsType": "Absolute", "bounds": b, "additionalInvalidatingHandles": []}}


def focus(w, h):
    """MPC's focus ring, made invisible"""
    return sub("Focus", "Focus", {"version": 1, "backgroundColour": "00000000", "outlineColour": "00000000",
                                  "backgroundInset": 2.0, "outlineThickness": 0.0}, B(0, 0, w, h), "WhenFocussed")


def action(on, handler, extra=""):
    return {"version": 2, "onAction": on, "handler": handler, "handleName": "" if handler == "Show Overlay" else "Data",
            "additionalData": extra, "handle remapping": {"version": 1, "map": []}}


KNOB_ACTIONS = [action("Mouse Down", "Q-Link"), action("Double Click", "Show Overlay", "knob overlay"),
                action("Enter Pressed", "Show Overlay", "knob overlay")]
TOGGLE_ACTIONS = [action("Mouse Down", "Q-Link"), action("Enter Pressed", "Toggle Switch")]
RADIO_ACTIONS = [action("Mouse Down", "Q-Link")]
OPEN_ACTIONS = [action("Mouse Down", "Toggle Switch"), action("Enter Pressed", "Toggle Switch")]


def definition(key, actions, parts, ignore=False):
    return {"key": key, "value": {
        "version": 4, "actions": actions,
        "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                           "unfocussed": {"version": 1, "colour": "0", "image": ""}},
        "ignoreMousePresses": ignore, "disableCoarseDataWheel": False, "repeats": 1, "hideQLinkBounds": True,
        "componentsData": parts}}


def filmstrip(img, frames, w, h, drag="Horizontal", x=0, y=0, invert=False):
    return sub("Knob", "Knob", {"version": 5, "knobType": "FilmStrip", "filmStrip": img, "numFrames": frames,
                                "invert": invert, "dragOrientation": drag, "handleName": "Data"}, B(x, y, w, h))


def button(on_img, off_img, bid, n, w, h, x=0, y=0):
    return sub("Button", "Button", {"version": 2, "onImage": on_img, "offImage": off_img, "buttonId": bid,
                                    "numButtonsInGroup": n, "handleName": "Data", "gestureBehaviour": "Instant"},
               B(x, y, w, h))


def text_label(kind, x, y, w, h, px, colour="ffffff", just="horizontallyCentred verticallyCentred", handle="Data",
               style="SemiBold"):
    """MPC's own text (Titillium Web): the bound parameter's value ("Value") or name ("Name")"""
    return sub(kind, "Label", {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web",
                                                                               "style": style, "height": px},
                                                         "colour": "ff" + colour, "justification": just, "case": "Original"},
                               "type": kind, "handleName": handle}, B(x, y, w, h))


def image(img, x, y, w, h):
    return sub("Image", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": img}, B(x, y, w, h))


def placed(dkey, name, index, rect, focus_on=True, extra=None, cond=None):
    x, y, w, h = rect
    m = [{"key": "Data", "value": "Parameter %d" % index}]
    for hname, hidx in (extra or {}).items():
        m.append({"key": hname, "value": "Parameter %d" % hidx})
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": dkey, "data": {"version": 1, "handleName": "Data"}},
            "handle remapping": {"version": 1, "map": m},
            "bounds": {"version": 2, "acceptsHWFocus": "Yes" if focus_on else "No", "showWhenDataModelInvalid": "Hide",
                       "whenVisible": "Always", "boundsType": "Absolute", "bounds": B(x, y, w, h),
                       "additionalInvalidatingHandles": [cond] if cond else []}}


def hexcol(c):
    return "%02x%02x%02x" % c


# ------------------------------------------------------------------------------------------------ the builder
class Skin:
    def __init__(self, skin, params):
        self.skin = skin
        self.index = {p["key"]: i for i, p in enumerate(params)}
        self.params = {p["key"]: p for p in params}
        self.defs = {}
        self.files = set()
        self.pg = pages()

    def save(self, img, name):
        if name not in self.files:
            img.save(os.path.join(self.skin, name))
            self.files.add(name)
        return name

    def opts(self, key):
        return self.params[key].get("options") or []

    def cond(self, when):
        """"deck_sel:2" -> MPC's IndexedEnabling handle (option index 1 of 4 of deck_sel)"""
        if not when:
            return None
        k, o = when.split(":")
        opts = self.opts(k)
        return "IndexedEnabling/%d/%d/Parameter %d" % (opts.index(o), len(opts), self.index[k])

    def define(self, key, actions, parts, ignore=False):
        if key not in self.defs:
            self.defs[key] = definition(key, actions, parts, ignore)
        return key

    def idx(self, key):
        if key not in self.index:
            sys.exit("remix_skin: no parameter %r" % key)
        return self.index[key]


class Page:
    """one page being built: its background (drawn into) and its components"""

    def __init__(self, sk, no):
        Image, ImageDraw, _, _ = _pil()
        self.sk, self.no, self.name = sk, no, TABS[no]
        self.bg = background(sk.pg, no)
        self.d = ImageDraw.Draw(self.bg)
        self.kids, self.top = [], []

    # -- baked
    def text(self, x, y, s, px=READ_PX, bold=True, colour=INK, anchor="lm"):
        self.d.text((x, y), s, font=font(px, bold), fill=colour + (255,), anchor=anchor)

    def black(self, rect, radius=0):
        x, y, w, h = rect
        self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=(0, 0, 0, 255))

    def paste(self, im, x, y):
        self.bg.alpha_composite(im, (int(x), int(y)))

    # -- live
    def live(self, key, rect, px=READ_PX, just="left verticallyCentred", style="Bold", cond=None, kind="Value", colour=INK):
        """MPC's own text of a parameter's value (a text param, a knob's value...) or name"""
        sk = self.sk
        x, y, w, h = rect
        dk = sk.define("rxLive_%s_%dx%d_%g_%s_%s" % (kind, w, h, px, just.split()[0], hexcol(colour)), [],
                       [text_label(kind, 0, 0, w, h, px, hexcol(colour), just, style=style)])
        self.kids.append(placed(dk, key, sk.idx(key), rect, False, cond=cond))

    def momentary(self, key, rect, label, cond=None, name=None, face=None):
        """a trigger: a button that fires the parameter (the wrapper drops it again)"""
        sk = self.sk
        x, y, w, h = rect
        if face:
            on, off = face
        else:
            on = sk.save(button_face(w, h, label, True), "rx_btn_%s_%dx%d_on.png" % (slug(label), w, h))
            off = sk.save(button_face(w, h, label, False), "rx_btn_%s_%dx%d_off.png" % (slug(label), w, h))
        dk = sk.define("rxBtn_%s_%s" % (on, off), TOGGLE_ACTIONS, [focus(w, h), button(on, off, 1, 1, w, h)])
        self.kids.append(placed(dk, name or key, sk.idx(key), rect, True, cond=cond))

    toggle = momentary   # the same component: Enter toggles, a tap sets (MPC's toggle button); the wrapper keeps a toggle

    def radio(self, key, rect, labels, cond=None, gap=6, faces=None, px=HEAD_PX):
        """an option parameter as a row of buttons (buttonId i of n), the current option lit"""
        sk = self.sk
        x, y, w, h = rect
        opts = sk.opts(key)
        n = len(opts)
        rw = int((w - gap * (n - 1)) / n)
        for o in range(n):
            lab = labels[o] if labels else opts[o]
            if faces:
                on, off = faces[o]
            else:
                on = sk.save(button_face(rw, h, lab, True, px), "rx_opt_%s_%dx%d_on.png" % (slug(lab), rw, h))
                off = sk.save(button_face(rw, h, lab, False, px), "rx_opt_%s_%dx%d_off.png" % (slug(lab), rw, h))
            dk = sk.define("rxOpt_%s_%s_%d_%d" % (on, off, o, n), RADIO_ACTIONS, [focus(rw, h), button(on, off, o, n, rw, h)])
            self.kids.append(placed(dk, "%s %s" % (key, opts[o]), sk.idx(key), (x + o * (rw + gap), y, rw, h), o == 0, cond=cond))

    def radio_boxes(self, key, boxes, labels, cond=None, px=HEAD_PX):
        """a radio whose buttons sit at given boxes (the deck buttons of pages.json)"""
        sk = self.sk
        opts = sk.opts(key)
        n = len(opts)
        for j, b in enumerate(boxes):
            o = b.get("deck", j)             # the option this box picks (the wheels pages give a side only two of the four decks)
            x, y, w, h = b["x"], b["y"], b["w"], b["h"]
            lab = labels[o]
            on = sk.save(button_face(w, h, lab, True, px), "rx_opt_%s_%dx%d_on.png" % (slug(lab), w, h))
            off = sk.save(button_face(w, h, lab, False, px), "rx_opt_%s_%dx%d_off.png" % (slug(lab), w, h))
            dk = sk.define("rxOpt_%s_%s_%d_%d" % (on, off, o, n), RADIO_ACTIONS, [focus(w, h), button(on, off, o, n, w, h)])
            self.kids.append(placed(dk, "%s %s" % (key, opts[o]), sk.idx(key), (x, y, w, h), o == 0, cond=cond))

    def knob(self, key, x, y, dia=56, name=None, live_name=False, cond=None, cell_w=None, value=False, art=None):
        """an in-place turning knob with its name under it (baked, or MPC's live Name for dynamic names).
        art = (x, y, diameter): the user's picture sits there (its own place and size); the cell, the name and the touch box
        keep the layout's cell, and the touch box is the cell joined with the picture"""
        sk = self.sk
        cw = cell_w or max(dia + 40, 96)
        nh = 30
        ch = dia + 2 + nh + (30 if value else 0)
        ax, ay, ad = art if art else (x + (cw - dia) // 2, y, dia)
        nx, nw = x, cw                     # the name's box (MPC's Name label when live)
        if art and not live_name and re.fullmatch(r"fx\d_(mix|super)", key) and ay + ad > y + dia + 2:
            live_name = True               # FX MIX / SUPER: the baked name would sit under the picture; MPC's Name label draws on top
            nw = cw; nx = x
        ux, uy = min(x, ax, nx), min(y, ay)
        ur, ub = max(x + cw, ax + ad, nx + nw), max(y + ch - (2 if art else 0), ay + ad)
        uw, uh = ur - ux, ub - uy
        strip = sk.save(knob_strip(ad), "rx_knob_%d.png" % ad)
        parts = [focus(uw, uh), filmstrip(strip, FRAMES, ad, ad, "Vertical", ax - ux, ay - uy)]
        if live_name:
            parts.append(text_label("Name", nx - ux, y + dia + 2 - uy, nw, nh, READ_PX if cw < 110 else KNOB_NAME_PX, hexcol(INK), style="SemiBold"))
            dk = sk.define("rxKnobN_%d_%d_%d_%d_%d_%d_%d" % (ad, cw, ax - ux, ay - uy, x - ux, y - uy, nw), KNOB_ACTIONS, parts)
        else:
            nm = name or sk.params[key]["name"]
            npx = KNOB_NAME_PX if fits(nm, cw, KNOB_NAME_PX, False) else READ_PX   # 26 px is the floor, never under
            if not fits(nm, cw, npx, False):
                sys.exit("remix_skin: knob name %r does not fit %d px at 26 px: shorten it" % (nm, cw))
            self.text(x + cw / 2, y + dia + 2 + nh / 2, nm, npx, False, anchor="mm")
            if value:
                parts.append(text_label("Value", x - ux, y + dia + 2 + nh - uy, cw, 30, READ_PX, hexcol(INK_DIM)))
            dk = sk.define("rxKnob_%d_%d_%d_%d%s" % (ad, cw, ax - ux, ay - uy, "v" if value else ""), KNOB_ACTIONS, parts)
        self.kids.append(placed(dk, key, sk.idx(key), (ux, uy, uw, uh), True, cond=cond))

    def fader(self, key, rect, vertical=True, cond=None, invert=False):
        """a fader filmstrip; a long horizontal one is drawn at half width (MPC scales the frame into the box) so the
        strip stays small in memory"""
        sk = self.sk
        x, y, w, h = rect
        fw, fh = w, h                    # a film strip frame is drawn 1:1: the strip is as wide as the fader
        frames = min(FRAMES, STRIP_MAX // fh)
        strip = sk.save(fader_strip(fw, fh, vertical, frames), "rx_fader_%dx%d.png" % (fw, fh))
        dk = sk.define("rxFader_%dx%d" % (w, h), KNOB_ACTIONS,
                       [focus(w, h), filmstrip(strip, frames, w, h, "Vertical" if vertical else "Horizontal", invert=invert)])
        self.kids.append(placed(dk, key, sk.idx(key), rect, True, cond=cond))

    def readout(self, key, rect, name, cond=None, name_w=None, just="left"):
        """a baked name on the left and the live value after it: a Q-Linked parameter with no control of its own"""
        x, y, w, h = rect
        nw = name_w or int(font(READ_PX).getlength(name) + 8)
        self.text(x, y + h / 2, name, READ_PX, True, INK_DIM)
        self.live(key, (x + nw, y, w - nw, h), READ_PX, just + " verticallyCentred", "SemiBold", cond=cond)

    def popup(self, key, rect, cond=None, list_at=None, field_img=None, caption=None):
        """a field showing the option (tap: the list opens); the list is a radio over the page, shown while <key>__open"""
        Image, ImageDraw, _, _ = _pil()
        sk = self.sk
        x, y, w, h = rect
        opts = sk.opts(key)
        oi = sk.idx(key + "__open")
        if field_img:
            on, off = field_img
            dk = sk.define("rxPopPart_%s" % off, OPEN_ACTIONS, [focus(w, h), button(on, off, 1, 1, w, h)])
            self.kids.append(placed(dk, key + " field", oi, rect, True, cond=cond))
        else:
            self.d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=6, fill=(0, 0, 0, 255), outline=(70, 70, 70, 255), width=2)
            cw_ = 0
            if caption:
                self.text(x + 10, y + h / 2, caption, READ_PX, True, INK_DIM)
                cw_ = int(font(READ_PX).getlength(caption)) + 16
            self.d.polygon([(x + w - 26, y + h / 2 - 5), (x + w - 12, y + h / 2 - 5), (x + w - 19, y + h / 2 + 5)], fill=ACCENT + (255,))
            dk = sk.define("rxPopField_%d_%d_%d" % (w, h, cw_), OPEN_ACTIONS,
                           [focus(w, h), text_label("Value", 10 + cw_, 0, w - 40 - cw_, h, READ_PX, hexcol(INK),
                                                    "left verticallyCentred", handle="Text", style="Bold")])
            self.kids.append(placed(dk, key + " field", oi, rect, True, extra={"Text": sk.idx(key)}, cond=cond))
        cols, ow, oh = POPUPS[key]
        rows = math.ceil(len(opts) / cols)
        pw, ph = cols * (ow + 6) + 14, rows * (oh + 6) + 14
        lx, ly = list_at or (x, y + h + 4)
        lx = min(max(4, lx), W - pw - 4)
        ly = min(max(52, ly), H - ph - 4)
        shown = "IndexedEnabling/1/2/Parameter %d" % oi
        panel = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
        ImageDraw.Draw(panel).rounded_rectangle([0, 0, pw - 1, ph - 1], radius=10, fill=(10, 10, 10, 252), outline=ACCENT + (255,), width=2)
        pimg = sk.save(panel, "rx_pop_%dx%d.png" % (pw, ph))
        pk = sk.define("rxPopPanel_%dx%d" % (pw, ph), [], [image(pimg, 0, 0, pw, ph)])
        parts = [placed(pk, key + " list", oi, (lx, ly, pw, ph), False, cond=shown)]
        for o, name in enumerate(opts):
            ox, oy = lx + 7 + (o % cols) * (ow + 6), ly + 7 + (o // cols) * (oh + 6)
            on = sk.save(button_face(ow, oh, name, True, BTN_PX), "rx_po_%s_%dx%d_on.png" % (slug(name), ow, oh))
            off = sk.save(button_face(ow, oh, name, False, BTN_PX), "rx_po_%s_%dx%d_off.png" % (slug(name), ow, oh))
            okey = sk.define("rxPopOpt_%s_%d_%d" % (off, o, len(opts)), RADIO_ACTIONS, [button(on, off, o, len(opts), ow, oh)])
            parts.append(placed(okey, "%s %s" % (key, name), sk.idx(key), (ox, oy, ow, oh), False, cond=shown))
        for p_ in parts:
            p_["bounds"]["showWhenDataModelInvalid"] = "Show"
        self.top += parts

    def finish(self):
        """the marker pixels last: nothing baked may cover them"""
        bake_marks(self.bg, self.sk.pg, self.no)
        return self.bg, self.kids + self.top


def slug(s):
    """a file-name-safe form of a label; every label gets its own ("< JUMP" and "JUMP >" must not collide)"""
    s = s.replace("<", "lt").replace(">", "gt").replace("/", "sl").replace("%", "pc").replace("|", "bar")
    return "".join(c if c.isalnum() else "_" for c in s).strip("_").lower() or "x"


def background(pg, page_no):
    """the panel texture (art/panel_source.jpg: inside the deck body, a 16:9-ish crop scaled to fill 1280 x 628), the
    top stripe darker, the page name at the left"""
    Image, ImageDraw, _, _ = _pil()
    src = art_img("panel_source.jpg").convert("RGB")
    cw, ch = 1080, 530                                   # 1080/530 = 2.04 = 1280/628
    x0, y0 = 60, 30
    bg = src.crop((x0, y0, x0 + cw, y0 + ch)).resize((W, H), Image.LANCZOS).convert("RGBA")
    import numpy as np
    a = np.asarray(bg).astype(np.float32)
    a[:pg["stripe_h"], :, :3] *= STRIPE_DARK
    bg = Image.fromarray(a.astype("uint8"), "RGBA")
    d = ImageDraw.Draw(bg)
    d.line([(0, pg["stripe_h"] - 1), (W, pg["stripe_h"] - 1)], fill=(60, 60, 60, 255), width=1)
    return bg


def bake_marks(bg, pg, page_no):
    tl, br = pg["marks"][page_no]
    for (x0, y0), cols in ((pg["page_mark_tl"], tl), (pg["page_mark_br"], br)):
        for k, c in enumerate(cols):
            bg.putpixel((x0 + k % 2, y0 + k // 2), tuple(c) + (255,))


def page_name(p, name, px=HEAD_PX):
    p.text(16, p.sk.pg["stripe_h"] / 2, name, px, True, INK)


def deck_cond(sk, selector, n):
    return sk.cond("%s:%d" % (selector, n))


# ------------------------------------------------------------------------------------------------ the pages
def page_deck(p):
    sk, pg = p.sk, p.sk.pg
    d = pg["deck"]
    page_name(p, "DECK")
    p.popup("zoom", (112, 4, 172, 40), list_at=(112, 52))
    p.radio_boxes("deck_sel", d["deck_buttons"], ["1", "2", "3", "4"])
    for n in range(1, 5):
        c = deck_cond(sk, "deck_sel", n)
        p.live("d%d_title" % n, (716, 4, 246, 40), READ_PX, cond=c)
        p.live("d%d_bpm" % n, (966, 4, 92, 40), READ_PX, "right verticallyCentred", cond=c)
        p.live("d%d_time" % n, (1062, 4, 110, 40), READ_PX, "right verticallyCentred", cond=c)
        p.live("d%d_ratetxt" % n, (1176, 4, 100, 40), READ_PX, "right verticallyCentred", cond=c)
    for row in ("row1", "row2", "row3"):
        for b in d[row]:
            k = DECK_KEYS[b["key"]]
            for n in range(1, 5):
                key = "d%d_%s" % (n, k)
                p.momentary(key, (b["x"], b["y"], b["w"], b["h"]), DECK_LABELS[k], cond=deck_cond(sk, "deck_sel", n))
    # under the rows: the selected deck's Q-Link values (the bank's knobs have no control of their own here)
    y = d["row3"][0]["y"] + d["row3"][0]["h"] + 6      # 558
    for i, (k, nm) in enumerate(DECK_READ):
        b = d["row1"][i]
        x, w = b["x"], b["w"]
        p.text(x + 4, y + 14, nm, READ_PX, True, INK_DIM)
        for n in range(1, 5):
            p.live("d%d_%s" % (n, k), (x, y + 30, w, 32), READ_PX, "left verticallyCentred", "SemiBold", cond=deck_cond(sk, "deck_sel", n))


def page_browse(p):
    sk, pg = p.sk, p.sk.pg
    br = pg["browse"]
    page_name(p, "BROWSE")
    p.live("br_path", (160, 4, 730, 40), READ_PX)
    p.text(904, 24, "PAGE", READ_PX, True, INK_DIM)
    p.live("br_page", (980, 4, 110, 40), READ_PX, "left verticallyCentred", "SemiBold")
    p.fader("br_scroll", (1100, 4, 172, 40), vertical=False)
    # the rows: a radio on br_sel (0..5) with transparent faces (ON: a faint highlight), the row's text live over it
    hi = sk.save(_rect_img(br["rows"][0]["w"], br["rows"][0]["h"], (255, 255, 255, 34), ACCENT + (255,)), "rx_row_on.png")
    lo = sk.save(_rect_img(br["rows"][0]["w"], br["rows"][0]["h"], (0, 0, 0, 0), (50, 50, 50, 255)), "rx_row_off.png")
    for r in br["rows"]:   # the row texts first, so the transparent row buttons above them take the touch
        p.live("br_row%d" % (r["row"] + 1), (r["x"] + 14, r["y"], r["w"] - 28, r["h"]), READ_PX)
    for r in br["rows"]:
        i = r["row"]
        rect = (r["x"], r["y"], r["w"], r["h"])
        rrect = ui_layout.at(p, "row", "br_sel", rect)
        # the data wheel / Q-Link move "br_row": 1..6 are the rows, 0 and 7 run off the list (the engine scrolls and puts it back)
        dk = sk.define("rxRow_%d" % i, RADIO_ACTIONS, [focus(r["w"], r["h"]), button(hi, lo, i + 1, 8, r["w"], r["h"])])
        if rrect is not None:
            p.kids.append(placed(dk, "row %d" % (i + 1), sk.idx("br_row"), rrect, i == 0))
    keys = {"load_1": ("load1", "LOAD 1"), "load_2": ("load2", "LOAD 2"), "load_3": ("load3", "LOAD 3"), "load_4": ("load4", "LOAD 4"),
            "folder": ("br_folder", "FOLDER"), "page_prev": ("br_prev", "< PAGE"), "page_next": ("br_next", "PAGE >")}
    for b in br["buttons"]:
        rect = (b["x"], b["y"], b["w"], b["h"])
        if b["key"] == "sort":
            p.popup("br_sort", rect, list_at=(b["x"], b["y"] - 3 * 52 - 20), caption="")
        else:
            k, lab = keys[b["key"]]
            p.momentary(k, rect, lab)


def page_mixer(p):
    sk, pg = p.sk, p.sk.pg
    m = pg["mixer"]
    page_name(p, "MIXER")
    for ch in m["channels"]:
        n = ch["deck"] + 1
        x, y = ch["x"], ch["y"]
        p.text(x + 150, 24, "DECK %d" % n, READ_PX, True, INK_DIM, anchor="mm")
        for i, (k, nm) in enumerate((("gain", "GAIN"), ("hi", "HIGH"), ("mid", "MID"), ("low", "LOW"))):
            p.knob("d%d_%s" % (n, k), x, y + 4 + i * 90, 56, nm)
        p.knob("d%d_filter" % n, x + 104, y + 4, 56, "FILTER")
        p.toggle("d%d_pfl" % n, (x + 104, y + 96, 96, 44), "PFL")
        p.toggle("d%d_fx1" % n, (x + 104, y + 148, 96, 44), "FX 1")
        p.toggle("d%d_fx2" % n, (x + 104, y + 200, 96, 44), "FX 2")
        p.fader("d%d_vol" % n, (x + 104, y + 250, 96, 190))
        p.radio("d%d_xfside" % n, (x, y + 446, 200, 44), ["L", "MID", "R"], gap=4, px=BTN_PX)
    for v in m["vu"] + [m["master_vu"]]:
        p.black((v["x"], v["y"], v["w"], v["h"]))
    mx = m["master"]
    p.text(mx["x"] + 100, 24, "MASTER", READ_PX, True, INK_DIM, anchor="mm")
    p.knob("master_gain", mx["x"] + 20, mx["y"] + 4, 56, "MASTER", cell_w=120, value=True)
    p.knob("head_mix", mx["x"] + 20, mx["y"] + 130, 56, "CUE MIX", cell_w=120, value=True)
    xf = m["xfader"]
    p.fader("xfader", (xf["x"], xf["y"], xf["w"], xf["h"]), vertical=False)
    p.text(xf["x"] + xf["w"] + 16, xf["y"] + xf["h"] / 2, "XFADER", READ_PX, True, INK_DIM)


def page_fx(p):
    sk, pg = p.sk, p.sk.pg
    fx = pg["fx"]
    page_name(p, "FX")
    for u, y0 in ((1, 56), (2, 316)):
        for s in fx["slots"]:
            si = s["slot"] + 1
            x = s["x"]
            f = "fx%d_%d_" % (u, si)
            p.popup(f + "type", (x, y0, 258, 44), list_at=(x, y0 + 50), caption="%d.%d" % (u, si))
            p.toggle(f + "en", (x + 270, y0, 114, 44), "ON")
            p.knob(f + "meta", x, y0 + 52, 56, "META", cell_w=76)
            for k in range(1, 5):
                p.knob(f + "p%d" % k, x + 77 * k, y0 + 52, 56, live_name=True, cell_w=76)
        # the unit's row
        y = y0 + 146
        p.text(24, y + 33, "UNIT %d" % u, HEAD_PX, True, INK)
        p.toggle("fx%d_on" % u, (150, y + 11, 100, 44), "ON")
        p.knob("fx%d_mix" % u, 262, y - 6, 56, "MIX", cell_w=92)
        p.knob("fx%d_super" % u, 360, y - 6, 56, "SUPER", cell_w=92)
        p.toggle("fx%d_master" % u, (470, y + 11, 136, 44), "MASTER")
        p.text(630, y + 33, "DECKS", READ_PX, True, INK_DIM)
        for n in range(1, 5):
            p.toggle("d%d_fx%d" % (n, u), (720 + (n - 1) * 72, y + 11, 64, 44), str(n))


# ---- the jog wheel picture: the user's ring + knob art (art/ui/jog/ring_flat_user.png, no baked light, so it can turn) as a
# film strip of JOG_FRAMES frames over one full turn (the knob's whole range), bound to the deck's pitch d<n>_rate: touch it anywhere
# and drag to change the pitch; a sync / pitch match turns it; under it the still centre disc (centre_user.png); on it the BPM in DSEG14 digits (one picture a digit)
JOG_DIR = os.path.join(ART, "ui", "jog")
JOG_RING = os.path.join(JOG_DIR, "ring_flat_user.png")
JOG_CENTRE = os.path.join(JOG_DIR, "centre_user.png")
JOG_FONT = os.path.join(JOG_DIR, "DSEG14Classic-Italic.ttf")
DIGIT_W, DIGIT_H = 44, 72


def _jog_ring_square():
    """the ring picture padded to a square about its own centre (so it turns without wobbling)"""
    Image, _, _, _ = _pil()
    im = Image.open(JOG_RING).convert("RGBA")
    side = max(im.size)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.alpha_composite(im, ((side - im.width) // 2, (side - im.height) // 2))
    return sq


def _jog_hole_frac():
    """the ring's inner hole diameter / its outer diameter (the centre disc fills it)"""
    import numpy as np
    sq = _jog_ring_square()
    a = np.asarray(sq)[..., 3] > 10
    side = sq.width
    row = a[int(side * 0.5)]
    xs = np.where(row)[0]
    # from the outer left edge inwards: the rim ends where the picture turns transparent
    left = xs[0]
    x = left
    while row[x]:
        x += 1
    inner_left = x
    return (side - 2 * inner_left + 2 * left) / float(side - 2 * left) if False else (side - 2 * inner_left) / float(side - 2 * left)


JOG_FRAMES = 16               # = RX_JOG_FRAMES (src/remix.h)
JOG_DIMPLES = 24              # the ring repeats every 360 / 24 = 15 degrees, so a strip of one pitch spins it round for ever


def jog_frames(D):
    return JOG_FRAMES


def jog_strip(D):
    """the ring turning one dimple pitch (15 degrees) over the frames, at the wheel's own size (MPC draws a frame 1:1); the engine runs the
    frame round and round with the platter, which looks like a ring turning for ever"""
    Image, _, _, _ = _pil()
    sq = _jog_ring_square()
    N = JOG_FRAMES
    big = sq.resize((D * 2, D * 2), Image.LANCZOS)
    strip = Image.new("RGBA", (D, D * N), (0, 0, 0, 0))
    for k in range(N):
        fr = big.rotate(-(360.0 / JOG_DIMPLES) * k / N, resample=Image.BICUBIC, center=(D, D))
        strip.alpha_composite(fr.resize((D, D), Image.LANCZOS), (0, k * D))
    return strip


def jog_centre_img(d):
    Image, _, _, _ = _pil()
    return Image.open(JOG_CENTRE).convert("RGBA").resize((d, d), Image.LANCZOS)


def digit_strip(dw=DIGIT_W, dh=DIGIT_H):
    """BAZZLINE's display look in white: frames 0..9 = the digits 0..9 lit (DSEG14 Classic Italic, a soft glow), every digit over its
    unlit segments (the "~" glyph, all segments, dim); frame 10 = the unlit segments alone (no digit)"""
    Image, ImageDraw, ImageFilter, ImageFont = _pil()
    big = ImageFont.truetype(JOG_FONT, 400)
    def layer(ch, col):
        im = Image.new("RGBA", (700, 800), (0, 0, 0, 0))
        ImageDraw.Draw(im).text((350, 400), ch, font=big, fill=col, anchor="mm")
        return im
    bb = layer("~", (255, 255, 255, 255)).getbbox()               # the cell: every segment
    gw, gh = bb[2] - bb[0], bb[3] - bb[1]
    sc = min((dw - 8) / gw, (dh - 8) / gh)
    sz = (max(1, int(gw * sc)), max(1, int(gh * sc)))
    ghost = layer("~", (62, 62, 62, 255)).crop(bb).resize(sz, Image.LANCZOS)
    strip = Image.new("RGBA", (dw, dh * 11), (0, 0, 0, 0))
    ox, oy = (dw - sz[0]) // 2, (dh - sz[1]) // 2
    for k in range(11):
        fr = Image.new("RGBA", (dw, dh), (0, 0, 0, 0))
        fr.alpha_composite(ghost, (ox, oy))
        if k < 10:
            lit = layer(str(k), (255, 255, 255, 255)).crop(bb).resize(sz, Image.LANCZOS)
            glow = lit.filter(ImageFilter.GaussianBlur(3))
            g2 = Image.new("RGBA", sz, (255, 255, 255, 0)); g2.putalpha(glow.getchannel("A").point(lambda v: int(v * 0.45)))
            fr.alpha_composite(g2, (ox, oy))
            fr.alpha_composite(lit, (ox, oy))
        strip.alpha_composite(fr, (0, k * dh))
    return strip


QR_N, QR_BYTES, QR_MOD, QR_QUIET = 33, 5, 6, 2       # = RX_QR_N / RX_QR_BYTES (src/remix.h); 6 px a module fits the centre disc's inscribed square


def qr_row_strip():
    """one film strip for every row: 256 frames of 8 modules (frame k = the byte k, its top bit the leftmost module), dark = black"""
    Image, ImageDraw, _, _ = _pil()
    m = QR_MOD
    strip = Image.new("RGBA", (8 * m, m * 256), (0, 0, 0, 0))
    for k in range(256):
        d = ImageDraw.Draw(strip)
        for b in range(8):
            if k & (0x80 >> b):
                d.rectangle([b * m, k * m, b * m + m - 1, k * m + m - 1], fill=(0, 0, 0, 255))
    return strip


def jog_light(D):
    """a fixed, subtle light over the whole wheel (it does not turn with the ring): a soft highlight at the top left, a soft shadow at the
    bottom right, cut to the wheel's circle"""
    import numpy as np
    Image, _, _, _ = _pil()
    S = D * 2
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    c = (S - 1) / 2.0
    dx, dy = (xx - c) / c, (yy - c) / c
    r = np.sqrt(dx * dx + dy * dy)
    lit = np.clip((-(dx + dy) / 1.414 - 0.15) / 0.85, 0, 1) ** 1.6            # 1 at the top-left edge, 0 past the middle
    dark = np.clip(((dx + dy) / 1.414 - 0.0) / 0.9, 0, 1) ** 1.2            # 1 at the bottom-right edge
    spot = np.exp(-(((dx + 0.52) ** 2 + (dy + 0.52) ** 2) / 0.16))            # the light source: a soft glow up and left
    a_hi = np.clip(0.07 * lit + 0.12 * spot, 0, 0.16)
    a_lo = 0.55 * dark
    out = np.zeros((S, S, 4), np.float32)
    out[..., 0:3] = 255.0
    out[..., 3] = a_hi * 255
    lo = a_lo > 0
    # the shadow: black over the lit part's place (they never overlap much: one side each)
    out[..., 0:3] = np.where(lo[..., None], 0.0, out[..., 0:3])
    out[..., 3] = np.where(lo, a_lo * 255, out[..., 3])
    out[..., 3] *= (r <= 0.995)
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGBA").resize((D, D), Image.LANCZOS)


def jog_wheel(p, side, selector, q):
    """the platter part, bottom to top: the turning ring (a film strip on the deck's pitch, touch = pitch), the still centre disc, on the
    WHEEL page the companion page's QR code on a white tile and over it a tap switch (SHOW QR: the disc's own picture covers the code;
    one tap and it is clear), then the BPM digits (DSEG14, white, a picture a digit; pictures that take no touch)"""
    sk = p.sk
    x, y, w, h = q["x"], q["y"], q["w"], q["h"]
    D = min(w, h)
    cx, cy = x + w // 2, y + h // 2
    hole = int(round(D * _jog_hole_frac()))
    ring = sk.save(jog_strip(D), "rx_jog_ring_%d.png" % D)
    ox, oy = cx - D // 2, cy - D // 2
    dk = sk.define("rxJogRing_%d" % D, [], [filmstrip(ring, JOG_FRAMES, D, D, "Vertical")], ignore=True)    # a picture the engine turns: no touch (the four strips below take the finger)
    discimg = jog_centre_img(hole)
    disc = sk.save(discimg, "rx_jog_centre_%d.png" % hole)
    dkc = sk.define("rxJogCentre_%d" % hole, [], [image(disc, 0, 0, hole, hole)], ignore=True)
    for n in range(1, 5):
        p.kids.append(placed(dk, "d%d_jog spin %s" % (n, side), sk.idx("d%d_jog" % n), (ox, oy, D, D), False, cond=deck_cond(sk, selector, n)))
    p.kids.append(placed(dkc, "jog centre %s" % side, sk.idx("deck_sel"), (cx - hole // 2, cy - hole // 2, hole, hole), False))
    dw = int(D * 0.072); dh = int(dw * DIGIT_H / DIGIT_W)
    dgs = sk.save(digit_strip(dw, dh), "rx_digits_%dx%d.png" % (dw, dh))
    dkd = sk.define("rxDigit_%d" % dw, [], [filmstrip(dgs, 11, dw, dh, "Vertical")], ignore=True)
    for n in range(1, 5):
        c = deck_cond(sk, selector, n)
        for j, k in enumerate("htu"):
            p.kids.append(placed(dkd, "d%d_bpm_%s %s" % (n, k, side), sk.idx("d%d_bpm_%s" % (n, k)),
                                 (cx - int(1.5 * dw) + j * dw, cy - dh // 2, dw, dh), False, cond=c))
    # ONE transparent endless dial over the whole wheel square (over the plate, under the buttons round it): a sideways drag turns the
    # wheel, right = forward, left = back, with no end to run into; the art itself stays and turns with the platter
    tiny = sk.save(_clear(8, 16), "rx_jogstrip.png")
    sd_ = sk.define("rxJogDial_%d" % D, KNOB_ACTIONS, [focus(D, D), filmstrip(tiny, 2, D, D, "Horizontal")])
    for n in range(1, 5):
        p.kids.insert(0 if False else len(p.kids), placed(sd_, "d%d_jogl %s" % (n, side), sk.idx("d%d_jogl" % n), (ox, oy, D, D), True, cond=deck_cond(sk, selector, n)))
    if side == "C":
        qcond = "IndexedEnabling/1/2/Parameter %d" % sk.idx("qr_show")     # only while SHOW QR is on
        Image, ImageDraw, _, _ = _pil()
        m = QR_MOD
        tile = (QR_N + 2 * QR_QUIET) * m
        tim = Image.new("RGBA", (tile, tile), (0, 0, 0, 0))
        ImageDraw.Draw(tim).rounded_rectangle([0, 0, tile - 1, tile - 1], radius=m * 2, fill=(255, 255, 255, 255))
        tx, ty = cx - tile // 2, cy - tile // 2
        tk = sk.define("rxQrTile", [], [image(sk.save(tim, "rx_qr_tile.png"), 0, 0, tile, tile)], ignore=True)
        p.kids.append(placed(tk, "qr tile", sk.idx("deck_sel"), (tx, ty, tile, tile), False, cond=qcond))
        strip = sk.save(qr_row_strip(), "rx_qr_row.png")
        rk = sk.define("rxQrRow", [], [filmstrip(strip, 256, 8 * m, m, "Vertical")], ignore=True)
        for r in range(QR_N):
            for b in range(QR_BYTES):
                i = r * QR_BYTES + b
                p.kids.append(placed(rk, "qr %d" % i, sk.idx("q%03d" % i), (tx + QR_QUIET * m + b * 8 * m, ty + QR_QUIET * m + r * m, 8 * m, m), False, cond=qcond))
        sd = int(round(hole * 0.396))                  # the black circle of the centre picture: the only touch that opens the QR
        clear_s = sk.save(_clear(sd, sd), "rx_qr_clear_%d.png" % sd)
        sw = sk.define("rxQrSwitch_%d" % sd, TOGGLE_ACTIONS, [focus(sd, sd), button(clear_s, clear_s, 1, 1, sd, sd)])
        p.kids.append(placed(sw, "qr show", sk.idx("qr_show"), (cx - sd // 2, cy - sd // 2, sd, sd), True))


def grey_face(w, h, label, px=HEAD_PX):
    """a deck button greyed out: the button's own size, dark and dim, over the real one"""
    Image, ImageDraw, _, _ = _pil()
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=6, fill=(22, 22, 22, 245), outline=(44, 44, 44, 255), width=1)
    d.text((w / 2, h / 2), label, font=font(px), fill=(80, 80, 80, 255), anchor="mm")
    return im


def master_tempo_lamp(p, q, crop=None):
    """the MASTER TEMPO button's red lamp (part 7) flashing at the master deck's beat: a picture on the parameter mt_led (the engine lights it
    about 80 ms a beat), over the button; dark when off"""
    import numpy as np
    Image, ImageDraw, ImageFilter, _ = _pil()
    sk = p.sk
    crop = crop if crop is not None else part_crop(7, q["w"], q["h"])
    a = np.asarray(crop).astype(int)
    red = (a[..., 0] > 170) & (a[..., 1] < 110) & (a[..., 2] < 110) & (a[..., 3] > 200)
    ys, xs = np.where(red)
    if len(xs) < 4:
        return
    cx, cy, r = int(xs.mean()), int(ys.mean()), max(3, int(max(xs.max() - xs.min(), ys.max() - ys.min()) / 2 + 1))
    n = 2 * r + 10
    SS = 4
    def lamp(on):
        im = Image.new("RGBA", (n * SS, n * SS), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if on:
            d.ellipse([0, 0, n * SS - 1, n * SS - 1], fill=(255, 40, 30, 90))
            d.ellipse([5 * SS, 5 * SS, (n - 5) * SS, (n - 5) * SS], fill=(255, 55, 45, 255))
            d.ellipse([(n / 2 - r * 0.45) * SS, (n / 2 - r * 0.45) * SS, (n / 2 + r * 0.45) * SS, (n / 2 + r * 0.45) * SS], fill=(255, 190, 170, 255))
        else:
            d.ellipse([5 * SS, 5 * SS, (n - 5) * SS, (n - 5) * SS], fill=(70, 12, 12, 255))
        return im.resize((n, n), Image.LANCZOS)
    on, off = sk.save(lamp(True), "rx_mtlamp_on_%d.png" % n), sk.save(lamp(False), "rx_mtlamp_off_%d.png" % n)
    dk = sk.define("rxMtLamp_%d" % n, [], [button(on, off, 1, 1, n, n)], ignore=True)
    p.kids.append(placed(dk, "master tempo lamp", sk.idx("mt_led"), (q["x"] + cx - n // 2, q["y"] + cy - n // 2, n, n), False))


def vinyl_pair_faces(w, h, px, py):
    """0.1.20: the VINYL / CDJ + JOG MODE pair (picture box (px, py, w, h) + RIGHT_SHIFT on the page) as ONE button for the vinyl mode: ON = VINYL
    (the CDJ word dimmed), OFF = CDJ (the VINYL word dimmed). Only the two words' own coloured pixels are dimmed; everything else is the picture."""
    import numpy as np
    Image, _, _, _ = _pil()
    crop = wheel_pic().crop((px, py, px + w, py + h))
    a = np.asarray(crop).astype(np.float32)
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    left = np.zeros(r.shape, bool); left[:, : w // 2 + 4] = True
    vinyl = (b > 150) & (g > 120) & (r < 150) & (al > 120) & left & (np.arange(h)[:, None] < 31)
    cdj = (g > 150) & (r > 90) & (b < 130) & (al > 120) & left & (np.arange(h)[:, None] >= 31)
    def face(dim):
        o = a.copy()
        o[dim, :3] *= 0.30
        return Image.fromarray(np.clip(o, 0, 255).astype("uint8"), "RGBA")
    return face(cdj), face(vinyl)           # (on, off)


def wheel_side(p, side, selector, parts, deck_buttons, other=None):
    """one CDJ panel: the parts pasted at their boxes, and the controls over them (four stacked, by the side's deck)"""
    Image, ImageDraw, _, _ = _pil()
    sk, pg = p.sk, p.sk.pg
    pid = {q["id"]: q for q in parts}
    for q in parts:
        if q["id"] == 1:
            jog_wheel(p, side, selector, q)     # the jog wheel: the user's ring art on a film strip (drawn after the buttons below)
            continue
        if q["id"] >= 31:
            continue                            # art-only pieces: pictures above the ring, below
        if side == "C" and q["id"] == 16:
            continue                            # the knob: drawn live in its new place (below)
        im = wheel_pic_crop(q["x"], q["y"], q["w"], q["h"]) if (side == "C" and q["id"] in RIGHT_IDS) else part_crop(q["id"], q["w"], q["h"], pg)
        p.paste(im, q["x"], q["y"])
    if side == "C":                             # the right-hand side's static art: one picture over the ring
        rs, rx, ry = right_static()
        rimg = sk.save(rs, "rx_right_static_%dx%d.png" % rs.size)
        rk = sk.define("rxRightStatic_%dx%d" % rs.size, [], [image(rimg, 0, 0, rs.width, rs.height)], ignore=True)
        p.kids.append(placed(rk, "right-hand art C", sk.idx("deck_sel"), (rx, ry, rs.width, rs.height), False))
    for q in parts:                             # the art-only pieces (labels, lines, the scale) are pictures over the ring, which they overlap
        if q["id"] >= 31:
            img = sk.save(part_crop(q["id"], q["w"], q["h"], pg), "rx_art_%d_%dx%d.png" % (q["id"], q["w"], q["h"]))
            ak = sk.define("rxArt_%s" % img, [], [image(img, 0, 0, q["w"], q["h"])], ignore=True)
            p.kids.append(placed(ak, "%s %s" % (q["part"], side), sk.idx("deck_sel"), (q["x"], q["y"], q["w"], q["h"]), False))
    p.radio_boxes(selector, deck_buttons, ["1", "2", "3", "4"])
    if other:    # the two-wheel page: the deck that is on the other wheel is greyed out here (any other pair is fine)
        for b in deck_buttons:
            bw, bh = b["w"], b["h"]
            g = sk.save(grey_face(bw, bh, str(b["deck"] + 1)), "rx_opt_grey_%d_%dx%d.png" % (b["deck"] + 1, bw, bh))
            gk = sk.define("rxGrey_%s" % g, [], [image(g, 0, 0, bw, bh)])
            p.kids.append(placed(gk, "%s taken by %s %d" % (selector, other, b["deck"] + 1), sk.idx(selector), (b["x"], b["y"], bw, bh), False,
                                 cond=sk.cond("%s:%d" % (other, b["deck"] + 1))))
    for q in parts:
        i = q["id"]
        if i not in PART_CTL:
            continue
        x, y, w, h = q["x"], q["y"], q["w"], q["h"]
        if side == "C" and i == 17:             # VINYL / CDJ + JOG MODE: the whole pair is the vinyl-mode button (the old box was only the left half, then halved again)
            import wheel_geom as G
            px, py = G.PARTS[17][0][3][0], G.PARTS[17][0][3][1]
            pw_, ph_ = 150, G.PARTS[17][0][3][3]
            on_i, off_i = vinyl_pair_faces(pw_, ph_, px, py)
            tag = "C_17_%dx%d" % (pw_, ph_)
            on, off = sk.save(on_i, "rx_part_%s_on.png" % tag), sk.save(off_i, "rx_part_%s_off.png" % tag)
            rect = (x, y, pw_, ph_)
            for n in range(1, 5):
                p.momentary("d%d_vinyl" % n, rect, "", cond=deck_cond(sk, selector, n), name="d%d_vinyl %s" % (n, side), face=(on, off))
            continue
        crop = wheel_pic_crop(x, y, w, h) if (side == "C" and i in RIGHT_IDS) else part_crop(i, w, h, pg)
        for half, k, kind in PART_CTL[i]:
            if half == "L":
                rect, cimg = (x, y, w // 2, h), crop.crop((0, 0, w // 2, h))
            elif half == "R":
                rect, cimg = (x + w // 2, y, w - w // 2, h), crop.crop((w // 2, 0, w, h))
            else:
                rect, cimg = (x, y, w, h), crop
            tag = "%s_%d%s_%dx%d" % (side, i, half or "", rect[2], rect[3])
            if kind in ("trigger", "toggle"):
                off = sk.save(cimg, "rx_part_%s_off.png" % tag)
                on = sk.save(tint_lit(cimg), "rx_part_%s_on.png" % tag)
                for n in range(1, 5):
                    p.momentary("d%d_%s" % (n, k), rect, "", cond=deck_cond(sk, selector, n), name="d%d_%s %s" % (n, k, side), face=(on, off))
            elif kind == "vfader":
                frames = min(FRAMES, STRIP_MAX // h)
                strip = sk.save(cdj_fader_strip(w, h, frames), "rx_tempo_%dx%d.png" % (w, h))
                # the strip itself runs top (-) to bottom (+) like the CDJ's scale: frame 0 = rate -1 = the cap at the top
                # ("invert" is untested on a device, so the strip carries the direction, not the flag)
                dk = sk.define("rxTempo_%dx%d" % (w, h), KNOB_ACTIONS, [focus(w, h), filmstrip(strip, frames, w, h, "Vertical")])
                for n in range(1, 5):
                    p.kids.append(placed(dk, "d%d_rate %s" % (n, side), sk.idx("d%d_rate" % n), rect, True, cond=deck_cond(sk, selector, n)))
            elif kind == "rknob":
                if side == "C":                 # 0.1.20: from the picture, bigger, left of the pitch slider
                    w, h = int(round(KNOB_BOX[2] * KNOB_SCALE)), int(round(KNOB_BOX[3] * KNOB_SCALE))
                    x, y = KNOB_RIGHT - w, KNOB_BOTTOM - h
                    rect = (x, y, w, h)
                    strip = sk.save(knob_strip_pic(w, h), "rx_jogfeel_%dx%d.png" % (w, h))
                else:
                    strip = sk.save(cdj_knob_strip(w, h), "rx_jogfeel_%dx%d.png" % (w, h))
                dk = sk.define("rxJogFeel_%dx%d" % (w, h), KNOB_ACTIONS, [focus(w, h), filmstrip(strip, FRAMES, w, h, "Vertical")])
                for n in range(1, 5):
                    p.kids.append(placed(dk, "d%d_jogfeel %s" % (n, side), sk.idx("d%d_jogfeel" % n), rect, True, cond=deck_cond(sk, selector, n)))
            elif kind == "popup":
                off = sk.save(cimg, "rx_part_%s_off.png" % tag)
                on = sk.save(tint_lit(cimg), "rx_part_%s_on.png" % tag)
                for n in range(1, 5):
                    p.popup("d%d_%s" % (n, k), rect, cond=deck_cond(sk, selector, n), list_at=(x - 220, y), field_img=(on, off))
    for q in parts:
        if q["id"] == 7:
            master_tempo_lamp(p, q, wheel_pic_crop(q["x"], q["y"], q["w"], q["h"]) if side == "C" else None)       # last, so the lamp sits on top of its button


def page_wheels(p):
    sk, pg = p.sk, p.sk.pg
    page_name(p, "WHEELS", 26)
    w = pg["wheels"]
    wheel_side(p, "L", "wheel_l", w["left"]["parts"], w["left"]["deck_buttons"], other="wheel_r")
    wheel_side(p, "R", "wheel_r", w["right"]["parts"], w["right"]["deck_buttons"], other="wheel_l")
    p.live("touch_txt", (536, 4, 210, 40), READ_PX, "horizontallyCentred verticallyCentred")
    for side, sel, key, parts in (("L", "wheel_l", "wheel_l_txt", w["left"]["parts"]), ("R", "wheel_r", "wheel_r_txt", w["right"]["parts"])):
        pl = [q for q in parts if q["id"] == 1][0]
        p.live(key, (pl["x"], 52, pl["w"], 34), READ_PX, "horizontallyCentred verticallyCentred")
        x0 = 8 if side == "L" else W // 2 + 4
        # the six Q-Link readouts a side on two rows of three (six on one row at 105 px overlapped: names ran into
        # values and the row crossed the tempo fader column); the rows sit under the play button column
        for i, (k, nm) in enumerate(WHEEL_READ):
            for n in range(1, 5):
                p.readout("d%d_%s" % (n, k), (x0 + (i % 3) * 180, 558 + (i // 3) * 36, 176, 34), nm, cond=deck_cond(sk, sel, n), name_w=64)


def page_wheel(p):
    sk, pg = p.sk, p.sk.pg
    page_name(p, "WHEEL")
    w = pg["wheel"]
    wheel_side(p, "C", "wheel_l", w["centre"]["parts"], w["centre"]["deck_buttons"])
    p.live("wheel_l_txt", (130, 4, 340, 40), READ_PX)
    p.live("touch_txt", (480, 4, 360, 40), READ_PX)
    for i, (k, nm) in enumerate(WHEEL_READ):
        for n in range(1, 5):
            p.readout("d%d_%s" % (n, k), (16, 100 + i * 52, 250, 40), nm, cond=deck_cond(sk, "wheel_l", n), name_w=90)


ui_layout.install(sys.modules[__name__])
PAGE_FN = [page_deck, page_browse, page_mixer, page_fx, page_wheels, page_wheel]


def _clear(w, h):
    Image, _, _, _ = _pil()
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


def _rect_img(w, h, fill, outline):
    Image, ImageDraw, _, _ = _pil()
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle([0, 0, w - 1, h - 1], radius=6, fill=fill, outline=outline, width=1)
    return im


# ------------------------------------------------------------------------------------------------ main
def build(skin):
    tui_path = os.path.join(skin, "TUI.json")
    tui = json.load(open(tui_path))
    cdefs = tui["pageData"]["componentDefinitions"]
    defs = cdefs["localComponentDefinitions"]
    sk = Skin(skin, all_params())
    built = {}
    for n, (name, fn) in enumerate(zip(TABS, PAGE_FN)):
        p = Page(sk, n)
        fn(p)
        ui_layout.added(p, sys.modules[__name__])
        bg, kids = p.finish()
        bgname = "rx_bg_%d.png" % n
        bg.convert("RGB").save(os.path.join(skin, bgname))
        sk.files.add(bgname)
        built[name] = [sub("Background", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": bgname},
                           B(0, 0, W, H))] + kids
    keep = []
    for e in defs:
        tab = e["key"].split("|")[0]
        if "|" in e["key"] and tab in built:
            e["value"]["componentsData"] = built[tab]
            e["value"]["backgroundData"] = {"version": 1, "focussed": {"version": 1, "colour": "ff101010", "image": ""},
                                            "unfocussed": {"version": 1, "colour": "ff101010", "image": ""}}
            keep.append(e)
    cdefs["localComponentDefinitions"] = keep + list(sk.defs.values())
    json.dump(tui, open(tui_path, "w"), indent=1)
    for f in os.listdir(skin):
        if f.endswith(".png") and f not in sk.files:
            os.remove(os.path.join(skin, f))
    from PIL import Image
    tall = [(f, Image.open(os.path.join(skin, f)).size) for f in sk.files if f.endswith(".png")]
    over = [f for f, (w_, h_) in tall if h_ > STRIP_MAX or w_ > STRIP_MAX]
    if over:
        sys.exit("remix_skin: pictures over MPC's %d px: %s" % (STRIP_MAX, ", ".join(over)))
    print("remix_skin: %d definitions, %d pictures, components per page: %s" % (
        len(sk.defs), len(sk.files), ", ".join("%s %d" % (t, len(built[t])) for t in TABS)))
    print("remix_skin: pictures take %.0f MB once loaded" % (sum(w_ * h_ * 4 for _, (w_, h_) in tall) / 1e6))


# ------------------------------------------------------------------------------------------------ preview
SAMPLE = {"title": "ARTIST - TRACK NAME", "bpm": "128.00", "time": "-03:45", "ratetxt": "+2.5%", "key": "8A",
          "br_path": "/media/Tracks/House", "br_page": "3 / 12", "wheel_l_txt": "DECK 1  128.00  -03:45", "wheel_r_txt": "DECK 2  126.00  -02:10",
          "touch_txt": "TOUCH", "readout": "", "cpu": "12%", "midi_text": ""}


def sample_text(params, key, kind):
    p = params[key]
    if kind == "Name":
        return p["name"]
    if p["kind"] == "text":
        for k, v in SAMPLE.items():
            if key == k or key.endswith("_" + k):
                return v
        if key.startswith("br_row"):
            return "%s  Artist - Title %s  128.0" % (key[-1].zfill(2), key[-1])
        return key
    if p.get("options"):
        return p["options"][int(round(p["default"]))]
    if p["kind"] == "int":
        return str(int(p["default"]))
    return "50%"


def preview(skin, out_dir):
    """every page as MPC should draw it (its parameters at their defaults; the live text in Titillium Web with sample
    values), into <out_dir>/<page>.png; plus the pages with the popups open"""
    Image, ImageDraw, _, ImageFont = _pil()
    params = {p["key"]: p for p in all_params()}
    plist = list(params.values())
    tui = json.load(open(os.path.join(skin, "TUI.json")))["pageData"]
    defs = {d["key"]: d["value"] for d in tui["componentDefinitions"]["localComponentDefinitions"]}
    cache = {}

    def img(name):
        if name not in cache:
            cache[name] = Image.open(os.path.join(skin, name)).convert("RGBA")
        return cache[name]

    def value_of(i):
        p = plist[i]
        m_ = re.fullmatch(r"d\d_bpm_([htu])", p["key"])
        if m_:                                    # the jog centre's BPM number, as 128 in the pictures
            return {"h": 0.1, "t": 0.2, "u": 0.8}[m_.group(1)]
        if p.get("options"):
            return p["default"] / max(1, len(p["options"]) - 1)
        if p["kind"] == "int":
            return (p["default"] - p["min"]) / max(1, p["max"] - p["min"])
        return float(p["default"])

    def visible(c, opens):
        for h in c["bounds"].get("additionalInvalidatingHandles", []):
            if h.startswith("IndexedEnabling/"):
                _, i, n, pp = h.split("/")
                pi = int(pp.split()[1])
                v = 1.0 if pi in opens else value_of(pi)
                if round(v * (int(n) - 1)) != int(i):
                    return False
        return True

    os.makedirs(out_dir, exist_ok=True)
    written = []
    seen = set()
    for t in tui["tabs"]:
        name = t["componentName"].split("|")[0]
        if name in seen:
            continue
        seen.add(name)
        comps = defs[t["componentName"]]["componentsData"]
        opens_all = {int(c["handle remapping"]["map"][0]["value"].split()[1]) for c in comps if c["componentData"]["type"].startswith("rxPop") and c["componentData"]["name"].endswith(" field")}
        for suffix, opens in (("", set()), ("_open", opens_all)):
            if suffix and not opens:
                continue
            page = Image.new("RGBA", (W, H), (0, 0, 0, 255))
            dd = ImageDraw.Draw(page)
            for c in comps:
                if not visible(c, opens):
                    continue
                x, y, w, h = (int(float(v)) for v in c["bounds"]["bounds"].split())
                cd = c["componentData"]
                hmap = {m["key"]: int(m["value"].split()[1]) for m in c.get("handle remapping", {}).get("map", [])}
                if cd["type"] == "Image":
                    page.alpha_composite(img(cd["data"]["image"]).resize((w, h)), (x, y))
                    continue
                for part in defs[cd["type"]]["componentsData"]:
                    pdx = part["componentData"]
                    px, py, pw, ph = (int(float(v)) for v in part["bounds"]["bounds"].split())
                    data = pdx["data"]
                    pi = hmap.get(data.get("handleName", "Data"), hmap.get("Data"))
                    if pdx["type"] == "Image":
                        page.alpha_composite(img(data["image"]).resize((pw, ph)), (x + px, y + py))
                    elif pdx["type"] == "Knob":
                        strip = img(data["filmStrip"])
                        n = data["numFrames"]
                        fh = strip.height // n
                        v = value_of(pi)
                        k = max(0, min(n - 1, round(v * (n - 1))))
                        page.alpha_composite(strip.crop((0, k * fh, strip.width, (k + 1) * fh)).resize((pw, ph)), (x + px, y + py))
                    elif pdx["type"] == "Button":
                        n = data["numButtonsInGroup"]
                        v = 1.0 if pi in opens else value_of(pi)
                        on = round(v * (n - 1)) == data["buttonId"] if n > 1 else v > 0.5
                        page.alpha_composite(img(data["onImage" if on else "offImage"]).resize((pw, ph)), (x + px, y + py))
                    elif pdx["type"] == "Label":
                        st = data["textStyle"]
                        f = ImageFont.truetype(FONT_B if st["font"]["style"] == "Bold" else FONT_S, int(round(st["font"]["height"])))
                        col = "#" + st["colour"][2:]
                        text = sample_text(params, plist[pi]["key"], data["type"])
                        ty = y + py + ph / 2
                        just = st["justification"]
                        if just.startswith("left"):
                            dd.text((x + px, ty), text, font=f, fill=col, anchor="lm")
                        elif just.startswith("right"):
                            dd.text((x + px + pw, ty), text, font=f, fill=col, anchor="rm")
                        else:
                            dd.text((x + px + pw / 2, ty), text, font=f, fill=col, anchor="mm")
            path = os.path.join(out_dir, "%s%s.png" % (name.lower(), suffix))
            page.convert("RGB").save(path)
            written.append(path)
    print("preview: " + ", ".join(written))
    return written


def overlays(skin, out_dir):
    """the wheel pages' backgrounds with every part's id drawn on its box (Layout/wheels_overlay.png, wheel_overlay.png)"""
    Image, ImageDraw, _, _ = _pil()
    pg = pages()
    os.makedirs(out_dir, exist_ok=True)
    out = []
    for no, key, sides, fname in ((4, "wheels", ("left", "right"), "wheels_overlay.png"), (5, "wheel", ("centre",), "wheel_overlay.png")):
        im = Image.open(os.path.join(skin, "rx_bg_%d.png" % no)).convert("RGBA")
        d = ImageDraw.Draw(im)
        for side in sides:
            for q in pg[key][side]["parts"]:
                d.rectangle([q["x"], q["y"], q["x"] + q["w"] - 1, q["y"] + q["h"] - 1], outline=ACCENT + (255,), width=2)
                d.text((q["x"] + 3, q["y"] + 1), str(q["id"]), font=font(26), fill=(255, 255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0, 255))
            for b in pg[key][side]["deck_buttons"]:
                d.rectangle([b["x"], b["y"], b["x"] + b["w"] - 1, b["y"] + b["h"] - 1], outline=ACCENT + (255,), width=2)
                d.text((b["x"] + 3, b["y"]), "D%d" % (b["deck"] + 1), font=font(26), fill=(255, 255, 255, 255), stroke_width=2, stroke_fill=(0, 0, 0, 255))
        path = os.path.join(out_dir, fname)
        im.convert("RGB").save(path)
        out.append(path)
    print("overlays: " + ", ".join(out))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["--layout"]:
        write_layout()
    elif a[:1] == ["--preview"] and len(a) == 3:
        preview(a[1], a[2])
    elif a[:1] == ["--overlays"] and len(a) == 3:
        overlays(a[1], a[2])
    elif len(a) == 1:
        build(a[0])
    else:
        sys.exit(__doc__)
