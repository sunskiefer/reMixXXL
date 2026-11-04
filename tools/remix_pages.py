#!/usr/bin/env python3
"""reMixXXL page geometry: the ONE place that says where every box, wheel and control sits on the plugin page
(MPC's 1280 x 628 plugin screen, under MPC's own header).

    python3 tools/remix_pages.py            writes src/boxes.h (for overlay.c / screen.c / touch.c) and build/pages.json
                                            (for tools/remix_skin.py, the skin builder)

Pages are MPC tabs, in this order (RX_PAGE_* in src/remix.h):
    0 OVERVIEW  four waveform strips with playheads (tap a strip -> DECK with that deck)
    1 DECK      Mixxx-style scrolling waveform + overview strip, transport, hot cues, loops; deck 1-4 buttons
    2 BROWSE    the track list: folder, 6 rows a page, < > pages, load 1-4
    3 MIXER     four channels: gain, hi/mid/low, filter, fader, cue; crossfader; master
    4 FX        effect unit 1 / 2: three slots, dry/wet, channel enables
    5 WHEELS    two CDJ decks side by side (wheels_layout_A), DECK 1-4 buttons per side
    6 WHEEL     one CDJ deck full size (wheels_layout_B), DECK 1-4 buttons

The wheel pages come from the user's part crops of the CDJ photo (art/control_map.json: id, x, y, w, h on
art/platter_nobrand.png) stacked as in the approved mock (art/wheels_layout_A.json / _B.json): left column
10,9,12,13,14,15,3,4; platter 1; right column 17,18,19,16,8,7,5,6. Positions are the user's; "move 5 to x,y" edits
the layout json, nothing here guesses.

Every drawn box has marker pixels (2 x 2, at its top-left and bottom-right) in the skin's background so overlay.c
can find the page and the box on the display: MARK_TL[page] / MARK_BR[page] colours, one pair per page, at
PAGE_MARK_TL / PAGE_MARK_BR (page corners). The boxes themselves are then known from the page origin."""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1280, 628
STRIPE_H = 48                                   # our own strip across the top: page name, deck buttons, readouts
PAGES = ["DECK", "BROWSE", "MIXER", "FX", "WHEELS", "WHEEL"]

# marker colours per page (page id in the colour; an unlikely combination in the art): 2 x 2 pixels each
PAGE_MARK_TL = (4, STRIPE_H + 2)                # where the top-left marker sits on every page
PAGE_MARK_BR = (W - 6, STRIPE_H + 2)            # the second marker: top-right, in the same row as the first. NOT at the page's bottom:
                                                # the Force's display is 1280 x 720 and MPC's header sits above the 628 px page, so the
                                                # bottom of the page can be off the display; a marker there is never seen (0.1.0/0.1.1: no waves)


def mark_colours(page):
    """four pixel colours (TL marker), and four (BR marker), for page 0..6: a base pattern plus the page id"""
    tl = [(23, 7, 91 + page * 12), (91 + page * 12, 7, 23), (7, 91 + page * 12, 23), (23, 91 + page * 12, 7)]
    br = [(91 + page * 12, 23, 7), (7, 23, 91 + page * 12), (61 + page * 12, 7, 61), (7, 61 + page * 12, 61)]
    return tl, br


# ------------------------------------------------------------------------------------------------ OVERVIEW
OV_X, OV_W = 88, 1176
OV_Y0, OV_H, OV_PITCH = STRIPE_H + 8, 128, 140   # four strips: 56, 196, 336, 476 .. 604
OV_LABEL_W = 80                                  # deck number / title tile left of each strip

# ------------------------------------------------------------------------------------------------ DECK
DK_WAVE = (16, STRIPE_H + 8, 1248, 232)          # the scrolling waveform (56..288)
DK_OVER = (16, 296, 1248, 40)                    # the overview strip (296..336)
DK_ROW1 = 348                                    # transport row: PLAY CUE SYNC SLIP REV < > KEYLOCK, h 60
DK_ROW2 = 420                                    # hot cues 1-8, h 60
DK_ROW3 = 492                                    # loop in/out/reloop, beatloop size - +, beatjump < >, h 60
DK_ROW_H = 60
DK_BTN_W = 148

# ------------------------------------------------------------------------------------------------ BROWSE
BR_ROW_Y0, BR_ROW_H, BR_ROWS = STRIPE_H + 8, 78, 6   # 56 .. 524
BR_BTN_Y = 540                                   # load 1..4, folder, < > ; h 72 (to 612)
BR_BTN_H = 72

# ------------------------------------------------------------------------------------------------ MIXER
MX_CH_X0, MX_CH_W = 24, 236                      # four channel strips: 24, 260, 496, 732 (+236)
MX_Y0 = STRIPE_H + 8
MX_VU = [(MX_CH_X0 + i * MX_CH_W + MX_CH_W - 30, MX_Y0 + 10, 18, 420) for i in range(4)]   # overlay VU per channel
MX_MASTER_X = 972                                # master column (gain, VU, crossfader curve)
MX_MASTER_VU = (MX_MASTER_X + 200, MX_Y0 + 10, 18, 420)
MX_XF = (24, 560, 920, 48)                       # crossfader line

# ------------------------------------------------------------------------------------------------ FX
FX_SLOT_X0, FX_SLOT_W = 24, 400                  # three slots: 24, 424, 824
FX_SLOT_Y, FX_SLOT_H = STRIPE_H + 8, 380
FX_ROW_Y = 456                                   # unit controls row: ON, MIX, 1 2 3 4 MASTER enables, h 64
FX_ROW2_Y = 536                                  # unit 1 / unit 2 select, next/prev effect per slot, h 64

# ------------------------------------------------------------------------------------------------ WHEELS / WHEEL
LEFT_COL = [10, 9, 12, 13, 14, 15, 3, 4]
RIGHT_COL = [17, 18, 19, 16, 8, 7, 5, 6]
PLATTER_ID = 1
PLATTER_CIRCLE = (455, 290, 262)                 # centre and radius on art/platter_nobrand.png
LABEL_R = 86                                     # the centre label disc radius (the spinning part), on the photo
GAP = 6                                          # between stacked parts (1:1)
COL_GAP = 8                                      # between a column and the platter (1:1)


# the user's controls sheet (art/ui/controls/controls_user.png, 913 x 606) cut into parts (art/ui/controls/controls_map.json: id -> box on
# the sheet) and where the user put them on the WHEEL page (their picture, wheel_page_mock_user.png: the sheet at 1:1 shifted by
# SHEET_OFF, the ring 574 px across centred at RING_C)
SHEET_DIR = os.path.join(ROOT, "art", "ui", "controls")
SHEET_OFF = (197, 22)
RING_D, RING_C = 574, (664, 333)
STATIC_IDS = {31: "cue/loop call, delete, memory (art only)", 32: "tempo scale (art only)", 33: "LOOP line (art only)", 34: "BEAT LOOP line (art only)"}


def sheet_boxes():
    m = json.load(open(os.path.join(SHEET_DIR, "controls_map.json")))
    out = {int(k): tuple(v) for k, v in m["boxes"].items()}
    out[31], out[32] = tuple(m["static"]["cueloop"]), tuple(m["static"]["scale"])
    out[33], out[34] = tuple(m["static"]["loop"]), tuple(m["static"]["beatloop"])
    return out


def control_map():
    """id -> {x, y, w, h}: the box of each part on the sheet (the platter, id 1: the ring on the WHEEL page)"""
    cm = {i: {"id": i, "x": b[0], "y": b[1], "w": b[2], "h": b[3]} for i, b in sheet_boxes().items()}
    cm[PLATTER_ID] = {"id": PLATTER_ID, "x": RING_C[0] - RING_D // 2 - SHEET_OFF[0], "y": RING_C[1] - RING_D // 2 - SHEET_OFF[1], "w": RING_D, "h": RING_D}
    return cm


PART_NAMES = {31: "cue/loop call art", 32: "tempo scale art", 33: "loop line art", 34: "beat loop line art", 1: "jog wheel", 2: "jog centre display", 3: "cue", 4: "play", 5: "tempo fader", 6: "tempo reset",
              7: "master tempo", 8: "tempo range", 9: "beat loop 4 / 8", 10: "loop in / out adjust", 11: "loop",
              12: "beat jump", 13: "slip rev", 14: "track search", 15: "search", 16: "jog feel",
              17: "vinyl / jog mode", 18: "beat sync / master", 19: "key sync"}

# what each part does (Mixxx controls); a two-button part has left/right halves
PART_CONTROLS = {
    3: [("cue_default", "trigger_hold")],              # cue: Mixxx cue_default (press/release)
    4: [("play", "toggle")],
    5: [("rate", "vfader_inv")],                       # the tempo fader: + at the bottom like the CDJ (rate -1..1)
    6: [("rate_set_default", "trigger")],
    7: [("keylock", "toggle")],
    8: [("rateRange_next", "trigger")],                # cycles 6 / 10 / 16 / wide (ours)
    9: [("beatloop_4_activate", "trigger"), ("beatloop_8_activate", "trigger")],
    10: [("loop_in", "trigger"), ("loop_out", "trigger")],
    11: [("reloop_toggle", "trigger")],
    12: [("beatjump_backward", "trigger"), ("beatjump_forward", "trigger")],
    13: [("slip_enabled", "toggle"), ("reverse", "toggle")],
    14: [("track_prev", "trigger"), ("track_next", "trigger")],
    15: [("back", "hold"), ("fwd", "hold")],
    16: [("jog_feel", "knob")],                        # ours: jog sensitivity 0..1
    17: [("vinyl_mode", "toggle")],                    # ours: vinyl (scratch on top) / CDJ (nudge)
    18: [("beatsync", "trigger"), ("sync_master", "toggle")],
    19: [("key_sync", "trigger")],                     # ours: match key to the other deck (keylock + pitch)
}


def stack_panel(scale_w, scale_h):
    """the deck panel as the user arranged it (the sheet's parts at their places round the ring, 1:1 in the sheet's frame, cropped to
    the parts' box), then the scale that fits scale_w x scale_h"""
    cm = control_map()
    x0 = min(c["x"] for c in cm.values()); y0 = min(c["y"] for c in cm.values())
    x1 = max(c["x"] + c["w"] for c in cm.values()); y1 = max(c["y"] + c["h"] for c in cm.values())
    W1, H1 = x1 - x0, y1 - y0
    s = min(scale_w / W1, scale_h / H1, 1.0)
    order = [31, 32, 33, 34] + [i for i in cm if i not in (1, 31, 32, 33, 34)] + [1]
    parts = [{"id": i, "part": PART_NAMES.get(i, "part %d" % i), "x": cm[i]["x"] - x0, "y": cm[i]["y"] - y0, "w": cm[i]["w"], "h": cm[i]["h"]} for i in order]
    return parts, W1, H1, s


def scaled(parts, s, ox, oy):
    out = []
    for p in parts:
        q = dict(p)
        q.update(x=int(round(ox + p["x"] * s)), y=int(round(oy + p["y"] * s)), w=int(round(p["w"] * s)), h=int(round(p["h"] * s)))
        out.append(q)
    return out


def wheel_geom(parts, s):
    """the platter circle (centre, outer radius, top-zone radius) and the label disc for the touch and the overlay"""
    p = [q for q in parts if q["id"] == PLATTER_ID][0]
    cx, cy = p["x"] + p["w"] // 2, p["y"] + p["h"] // 2
    r = p["w"] // 2
    r_top = int(r * 0.80)                            # inside: the "top" of the platter (vinyl/scratch); outside: the ring (bend)
    return {"cx": cx, "cy": cy, "r": r, "r_top": r_top, "label_r": int(r * 0.34)}


def deck_buttons(x0, n=4, w=96, h=40, gap=10, y=4, decks=None):
    """the deck buttons in a row; decks = which decks (0-based) get one, in the middle of the n slots (default: all n)"""
    decks = list(range(n)) if decks is None else decks
    first = (n - len(decks)) // 2
    return [{"deck": d, "x": x0 + (first + k) * (w + gap), "y": y, "w": w, "h": h} for k, d in enumerate(decks)]


def pages():
    out = {"W": W, "H": H, "stripe_h": STRIPE_H, "pages": PAGES, "page_mark_tl": PAGE_MARK_TL, "page_mark_br": PAGE_MARK_BR,
           "marks": [mark_colours(i) for i in range(len(PAGES))]}
    # OVERVIEW
    out["overview"] = {"strips": [{"deck": i, "x": OV_X, "y": OV_Y0 + i * OV_PITCH, "w": OV_W, "h": OV_H,
                                   "label": {"x": 0, "y": OV_Y0 + i * OV_PITCH, "w": OV_LABEL_W, "h": OV_H}} for i in range(4)]}
    # DECK
    row = lambda y, keys: [{"key": k, "x": 16 + i * (DK_BTN_W + 8), "y": y, "w": DK_BTN_W, "h": DK_ROW_H} for i, k in enumerate(keys)]
    out["deck"] = {"wave": dict(zip("xywh", DK_WAVE)), "over": dict(zip("xywh", DK_OVER)),
                   "deck_buttons": deck_buttons(292),
                   "row1": row(DK_ROW1, ["play", "cue_default", "beatsync", "slip_enabled", "reverse", "keylock", "quantize", "vinyl_mode"]),
                   "row2": row(DK_ROW2, ["hotcue_%d_activate" % i for i in range(1, 9)]),
                   "row3": row(DK_ROW3, ["loop_in", "loop_out", "reloop_toggle", "loop_halve", "loop_double", "beatjump_backward", "beatjump_forward", "eject"])}
    # BROWSE
    out["browse"] = {"rows": [{"row": i, "x": 16, "y": BR_ROW_Y0 + i * BR_ROW_H, "w": 1248, "h": BR_ROW_H - 4} for i in range(BR_ROWS)],
                     "buttons": [{"key": k, "x": 16 + i * 158, "y": BR_BTN_Y, "w": 150, "h": BR_BTN_H}
                                 for i, k in enumerate(["load_1", "load_2", "load_3", "load_4", "folder", "page_prev", "page_next", "sort"])]}
    # MIXER
    out["mixer"] = {"channels": [{"deck": i, "x": MX_CH_X0 + i * MX_CH_W, "y": MX_Y0, "w": MX_CH_W - 12, "h": 490} for i in range(4)],
                    "vu": [dict(zip("xywh", v)) for v in MX_VU], "master_vu": dict(zip("xywh", MX_MASTER_VU)),
                    "master": {"x": MX_MASTER_X, "y": MX_Y0, "w": 290, "h": 490}, "xfader": dict(zip("xywh", MX_XF))}
    # FX
    out["fx"] = {"slots": [{"slot": i, "x": FX_SLOT_X0 + i * FX_SLOT_W, "y": FX_SLOT_Y, "w": FX_SLOT_W - 16, "h": FX_SLOT_H} for i in range(3)],
                 "row": {"y": FX_ROW_Y, "h": 64}, "row2": {"y": FX_ROW2_Y, "h": 64}}
    # WHEELS (A): two panels
    parts, W1, H1, s = stack_panel((W // 2) - 12, H - STRIPE_H - 8)
    pw, ph = int(W1 * s), int(H1 * s)
    xl, xr = 8, W // 2 + 4
    y0 = STRIPE_H + 4 + (H - STRIPE_H - 8 - ph) // 2
    L, R = scaled(parts, s, xl, y0), scaled(parts, s, xr, y0)
    out["wheels"] = {"scale": round(s, 4), "panel_1to1": {"w": W1, "h": H1},
                     "left": {"parts": L, "wheel": wheel_geom(L, s), "deck_buttons": deck_buttons(xl + (pw - 4 * 106 + 10) // 2)},
                     "right": {"parts": R, "wheel": wheel_geom(R, s), "deck_buttons": deck_buttons(xr + (pw - 4 * 106 + 10) // 2)}}
    # WHEEL (B): one panel, centred
    parts, W1, H1, s = stack_panel(W - 16, H - STRIPE_H - 8)
    s = 1.0                                          # the user's picture of this page: the sheet 1:1, shifted by SHEET_OFF
    cmm = control_map()
    x0 = SHEET_OFF[0] + min(c["x"] for c in cmm.values())
    y0 = SHEET_OFF[1] + min(c["y"] for c in cmm.values())
    C = scaled(parts, s, x0, y0)
    out["wheel"] = {"scale": round(s, 4), "panel_1to1": {"w": W1, "h": H1},
                    "centre": {"parts": C, "wheel": wheel_geom(C, s), "deck_buttons": deck_buttons(W - 4 * 106 - 6)}}
    try:
        import ui_layout
        ui_layout.apply_pages(out)                       # the user's DECK waveform / overview boxes (art/ui/reMixXXL_layout.json)
    except ImportError:
        pass
    out["part_controls"] = {str(k): v for k, v in PART_CONTROLS.items()}
    out["part_names"] = {str(k): v for k, v in PART_NAMES.items()}
    out["platter_label_px"] = {"cx": PLATTER_CIRCLE[0], "cy": PLATTER_CIRCLE[1], "r": LABEL_R}
    return out


def write_boxes_h(p, path):
    L = ["/* GENERATED by tools/remix_pages.py: the page geometry (skin coordinates, 1280 x 628). Do not edit. */",
         "#pragma once", "#define RX_STRIPE_H %d" % STRIPE_H,
         "#define RX_PAGE_MARK_TL_X %d" % PAGE_MARK_TL[0], "#define RX_PAGE_MARK_TL_Y %d" % PAGE_MARK_TL[1],
         "#define RX_PAGE_MARK_BR_X %d" % PAGE_MARK_BR[0], "#define RX_PAGE_MARK_BR_Y %d" % PAGE_MARK_BR[1],
         "static const unsigned char RX_MARK_TL[%d][4][3] = {" % len(PAGES)]
    for tl, _ in p["marks"]:
        L.append("    {" + ", ".join("{%d, %d, %d}" % c for c in tl) + "},")
    L += ["};", "static const unsigned char RX_MARK_BR[%d][4][3] = {" % len(PAGES)]
    for _, br in p["marks"]:
        L.append("    {" + ", ".join("{%d, %d, %d}" % c for c in br) + "},")
    L += ["};"]
    ov = p["overview"]["strips"]
    L += ["#define RX_OV_X %d" % ov[0]["x"], "#define RX_OV_W %d" % ov[0]["w"], "#define RX_OV_Y0 %d" % ov[0]["y"],
          "#define RX_OV_H %d" % ov[0]["h"], "#define RX_OV_PITCH %d" % (ov[1]["y"] - ov[0]["y"])]
    d = p["deck"]
    L += ["#define RX_DK_WAVE_X %d\n#define RX_DK_WAVE_Y %d\n#define RX_DK_WAVE_W %d\n#define RX_DK_WAVE_H %d" % (d["wave"]["x"], d["wave"]["y"], d["wave"]["w"], d["wave"]["h"]),
          "#define RX_DK_OVER_X %d\n#define RX_DK_OVER_Y %d\n#define RX_DK_OVER_W %d\n#define RX_DK_OVER_H %d" % (d["over"]["x"], d["over"]["y"], d["over"]["w"], d["over"]["h"])]
    m = p["mixer"]
    L += ["static const int RX_MX_VU[4][4] = {" + ", ".join("{%d, %d, %d, %d}" % (v["x"], v["y"], v["w"], v["h"]) for v in m["vu"]) + "};",
          "static const int RX_MX_MASTER_VU[4] = {%d, %d, %d, %d};" % (m["master_vu"]["x"], m["master_vu"]["y"], m["master_vu"]["w"], m["master_vu"]["h"])]
    for name, key, sides in (("WHEELS", "wheels", ("left", "right")), ("WHEEL", "wheel", ("centre",))):
        for k, side in enumerate(sides):
            w = p[key][side]["wheel"]
            L.append("#define RX_%s_%d_CX %d\n#define RX_%s_%d_CY %d\n#define RX_%s_%d_R %d\n#define RX_%s_%d_RTOP %d\n#define RX_%s_%d_LABEL_R %d"
                     % (name, k, w["cx"], name, k, w["cy"], name, k, w["r"], name, k, w["r_top"], name, k, w["label_r"]))
    # the touchable parts on the wheel pages: page, side, part id, box
    L.append("typedef struct { int page, side, id, x, y, w, h; } rx_part_box_t;")
    L.append("static const rx_part_box_t RX_PART_BOXES[] = {")
    for pg, key, sides in ((4, "wheels", ("left", "right")), (5, "wheel", ("centre",))):
        for k, side in enumerate(sides):
            for q in p[key][side]["parts"]:
                if q["id"] in STATIC_IDS:
                    continue
                L.append("    {%d, %d, %d, %d, %d, %d, %d}," % (pg, k, q["id"], q["x"], q["y"], q["w"], q["h"]))
            for b in p[key][side]["deck_buttons"]:
                L.append("    {%d, %d, %d, %d, %d, %d, %d}," % (pg, k, 100 + b["deck"], b["x"], b["y"], b["w"], b["h"]))   # 100+n: deck select n
    L.append("};")
    L.append("#define RX_PART_BOXES_N (sizeof RX_PART_BOXES / sizeof RX_PART_BOXES[0])")
    # deck page buttons (touch fallback is MPC's own; these are for the skin only) and overview strips as boxes for tap
    L.append("static const int RX_DECK_BUTTONS[4][4] = {" + ", ".join("{%d, %d, %d, %d}" % (b["x"], b["y"], b["w"], b["h"]) for b in d["deck_buttons"]) + "};")
    open(path, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    p = pages()
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    json.dump(p, open(os.path.join(ROOT, "build", "pages.json"), "w"), indent=1)
    write_boxes_h(p, os.path.join(ROOT, "src", "boxes.h"))
    print("remix_pages: WHEELS scale %.3f (panel %dx%d), WHEEL scale %.3f" % (p["wheels"]["scale"], p["wheels"]["panel_1to1"]["w"],
          p["wheels"]["panel_1to1"]["h"], p["wheel"]["scale"]))
    print("remix_pages: wrote src/boxes.h and build/pages.json")
