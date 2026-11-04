#!/usr/bin/env python3
"""The single source of reMixXXL's plugin parameters: writes params.json (for mpc-vst-plugins' gen_vst.py) and
src/remix_params.h (the index of every key, for engine.c).

    python3 tools/gen_params.py

MPC stores a project's values by parameter index: once released, parameters are only ever appended.

Key scheme: d<n>_<x> for deck n = 1..4 (Mixxx [Channel<n>]), fx<u>_<s>_<x> for effect unit u (1..2) slot s (1..3),
the rest global. Each entry carries "ctl": the Mixxx control it drives ("[Channel%d],play": %d is the deck), so
engine.c's adapter is a table, not code. Kinds: knob (continuous), toggle, trigger (momentary), enum (options), text
(read-only string the engine writes), int."""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RATE_RANGES = ["6%", "10%", "16%", "WIDE"]
EFFECTS = ["NONE", "ECHO", "FILTER", "REVERB", "FLANGER", "PHASER", "BITCRUSH", "MOOG LP", "LOOP ROLL"]
SORTS = ["NAME", "BPM", "ARTIST"]
DECKS = ["1", "2", "3", "4"]
PAGE_ZOOMS = ["2 S", "4 S", "8 S", "16 S", "32 S"]
MAPPINGS_MAX = 16

P = []


def add(key, name, kind="knob", default=0.0, lo=0.0, hi=1.0, options=None, ctl=None, deck=None, display=None, **extra):
    e = {"key": key, "name": name, "default": default, "min": lo, "max": hi, "kind": kind}
    if options:
        e["options"] = options
        e["max"] = len(options) - 1
        e["display"] = "int"
    if kind == "trigger":
        e["momentary"] = True
    if kind == "text":
        e["display"] = "string"
        e["max"] = 0
        e["min"] = 0
        e["poll"] = True
    if kind == "int":
        e["display"] = "int"
    if display:
        e["display"] = display
    if ctl:
        e["ctl"] = ctl
    if deck is not None:
        e["deck"] = deck
    e.update(extra)
    P.append(e)
    return e


# ---- global: page state, browse, master, MIDI
add("deck_sel", "DECK", "enum", 0, options=DECKS, ctl="[reMixXXL],deck_select")
add("wheel_l", "WHEEL L DECK", "enum", 0, options=DECKS, ctl="[reMixXXL],wheel_deck_l")
add("wheel_r", "WHEEL R DECK", "enum", 1, options=DECKS, ctl="[reMixXXL],wheel_deck_r")
add("zoom", "ZOOM", "enum", 2, options=PAGE_ZOOMS, ctl="[reMixXXL],zoom")
add("xfader", "CROSSFADER", "knob", 0.5, ctl="[Master],crossfader")        # 0..1 -> -1..1
add("master_gain", "MASTER", "knob", 0.5, ctl="[Master],gain")
add("head_mix", "CUE MIX", "knob", 0.5, ctl="[Master],headMix")
add("readout", "READOUT", "text", ctl="[reMixXXL],readout")
add("cpu", "CPU", "text", ctl="[reMixXXL],cpu_text")
# browse
for i in range(1, 7):
    add("br_row%d" % i, "ROW %d" % i, "text", ctl="[reMixXXL],browse_row%d" % i)
add("br_sel", "SELECT", "int", 0, 0, 5, ctl="[reMixXXL],browse_sel")       # the row (0..5) on this page
add("br_page", "PAGE", "int", 0, 0, 999, ctl="[reMixXXL],browse_page", display="int")
add("br_prev", "PAGE <", "trigger", ctl="[reMixXXL],browse_page_prev")
add("br_next", "PAGE >", "trigger", ctl="[reMixXXL],browse_page_next")
add("br_folder", "FOLDER", "trigger", ctl="[reMixXXL],browse_folder")     # opens the folder list (rows become folders)
add("br_up", "UP", "trigger", ctl="[reMixXXL],browse_up")
add("br_sort", "SORT", "enum", 0, options=SORTS, ctl="[reMixXXL],browse_sort")
add("br_scroll", "SCROLL", "knob", 0.0, ctl="[reMixXXL],browse_scroll")      # Q-Link: 0..1 over the whole list
add("br_path", "PATH", "text", ctl="[reMixXXL],browse_path")
for n in range(1, 5):
    add("load%d" % n, "LOAD %d" % n, "trigger", ctl="[Channel%d],LoadSelectedTrack" % n, deck=n)
for n in range(1, 5):
    add("ov_tap%d" % n, "DECK %d" % n, "trigger", ctl="[reMixXXL],overview_tap%d" % n, deck=n)   # tap a strip on OVERVIEW
add("midi_map", "MIDI MAP", "enum", 0, options=["NONE"] + ["MAP %d" % i for i in range(1, MAPPINGS_MAX)], ctl="[reMixXXL],midi_map", dynamic_display=True)
add("midi_learn", "MIDI LEARN", "toggle", ctl="[reMixXXL],midi_learn")
add("midi_text", "MIDI", "text", ctl="[reMixXXL],midi_text")

# ---- decks
for n in range(1, 5):
    c = "[Channel%d]," % n
    d = "d%d_" % n
    add(d + "play", "PLAY", "toggle", ctl=c + "play", deck=n)
    add(d + "cue", "CUE", "trigger", ctl=c + "cue_default", deck=n)
    add(d + "sync", "SYNC", "trigger", ctl=c + "beatsync", deck=n)
    add(d + "master", "MASTER", "toggle", ctl=c + "sync_master", deck=n)
    add(d + "slip", "SLIP", "toggle", ctl=c + "slip_enabled", deck=n)
    add(d + "rev", "REV", "toggle", ctl=c + "reverse", deck=n)
    add(d + "keylock", "KEYLOCK", "toggle", ctl=c + "keylock", deck=n)
    add(d + "quant", "QUANT", "toggle", 1, ctl=c + "quantize", deck=n)
    add(d + "vinyl", "VINYL", "toggle", 1, ctl=c + "vinyl_mode", deck=n)
    for h in range(1, 9):
        add(d + "hc%d" % h, "CUE %d" % h, "trigger", ctl=c + "hotcue_%d_activate" % h, deck=n)
    add(d + "hcclear", "CLR CUE", "toggle", ctl=c + "hotcue_clear_mode", deck=n)          # ours: next hot cue tap clears
    add(d + "loop_in", "LOOP IN", "trigger", ctl=c + "loop_in", deck=n)
    add(d + "loop_out", "LOOP OUT", "trigger", ctl=c + "loop_out", deck=n)
    add(d + "reloop", "RELOOP", "trigger", ctl=c + "reloop_toggle", deck=n)
    add(d + "halve", "LOOP /2", "trigger", ctl=c + "loop_halve", deck=n)
    add(d + "double", "LOOP X2", "trigger", ctl=c + "loop_double", deck=n)
    add(d + "bl4", "LOOP 4", "trigger", ctl=c + "beatloop_4_activate", deck=n)
    add(d + "bl8", "LOOP 8", "trigger", ctl=c + "beatloop_8_activate", deck=n)
    add(d + "blsize", "LOOP SIZE", "enum", 5, options=["1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32"], ctl=c + "beatloop_size_idx", deck=n)
    add(d + "bjback", "JUMP <", "trigger", ctl=c + "beatjump_backward", deck=n)
    add(d + "bjfwd", "JUMP >", "trigger", ctl=c + "beatjump_forward", deck=n)
    add(d + "bjsize", "JUMP SIZE", "enum", 5, options=["1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32"], ctl=c + "beatjump_size_idx", deck=n)
    add(d + "back", "<< START", "trigger", ctl=c + "start", deck=n)                 # jumps to the very start of the track
    add(d + "fwd", "SEARCH >>", "trigger", ctl=c + "fwd_tap", deck=n)
    add(d + "prev", "|< TRACK", "trigger", ctl=c + "track_prev", deck=n)
    add(d + "next", "TRACK >|", "trigger", ctl=c + "track_next", deck=n)
    add(d + "eject", "EJECT", "trigger", ctl=c + "eject", deck=n)
    add(d + "rate", "TEMPO", "knob", 0.5, ctl=c + "rate", deck=n, dynamic_display=True)      # 0..1 -> -1..1 of the range
    add(d + "rate_reset", "TEMPO RESET", "trigger", ctl=c + "rate_set_default", deck=n)
    add(d + "rate_range", "RANGE", "enum", 1, options=RATE_RANGES, ctl=c + "rateRange_idx", deck=n)
    add(d + "jogfeel", "JOG FEEL", "knob", 0.5, ctl=c + "jog_feel", deck=n)
    add(d + "keysync", "KEY SYNC", "trigger", ctl=c + "key_sync", deck=n)
    add(d + "gain", "GAIN", "knob", 0.5, ctl=c + "pregain", deck=n)                      # 0..1 -> Mixxx 0..4 (0.5 = 1)
    add(d + "hi", "HIGH", "knob", 0.5, ctl="[EqualizerRack1_[Channel%d]_Effect1],parameter3" % n, deck=n)
    add(d + "mid", "MID", "knob", 0.5, ctl="[EqualizerRack1_[Channel%d]_Effect1],parameter2" % n, deck=n)
    add(d + "low", "LOW", "knob", 0.5, ctl="[EqualizerRack1_[Channel%d]_Effect1],parameter1" % n, deck=n)
    add(d + "filter", "FILTER", "knob", 0.5, ctl="[QuickEffectRack1_[Channel%d]],super1" % n, deck=n)
    add(d + "vol", "VOLUME", "knob", 1.0, ctl=c + "volume", deck=n)
    add(d + "pfl", "PFL", "toggle", ctl=c + "pfl", deck=n)
    add(d + "xfside", "XF SIDE", "enum", 1, options=["LEFT", "MID", "RIGHT"], ctl=c + "orientation", deck=n)
    add(d + "fx1", "FX 1", "toggle", ctl="[EffectRack1_EffectUnit1],group_[Channel%d]_enable" % n, deck=n)
    add(d + "fx2", "FX 2", "toggle", ctl="[EffectRack1_EffectUnit2],group_[Channel%d]_enable" % n, deck=n)
    add(d + "bpm", "BPM", "text", ctl=c + "bpm_text", deck=n)
    add(d + "time", "TIME", "text", ctl=c + "time_text", deck=n)
    add(d + "title", "TITLE", "text", ctl=c + "title_text", deck=n)
    add(d + "key", "KEY", "text", ctl=c + "key_text", deck=n)
    add(d + "ratetxt", "TEMPO", "text", ctl=c + "rate_text", deck=n)

# ---- effects
for u in (1, 2):
    g = "[EffectRack1_EffectUnit%d]," % u
    f = "fx%d_" % u
    add(f + "on", "FX %d ON" % u, "toggle", ctl=g + "enabled")
    add(f + "mix", "FX %d MIX" % u, "knob", 1.0, ctl=g + "mix")
    add(f + "super", "FX %d SUPER" % u, "knob", 0.5, ctl=g + "super1")
    add(f + "master", "FX %d MASTER" % u, "toggle", ctl=g + "group_[Master]_enable")
    for s in (1, 2, 3):
        gs = "[EffectRack1_EffectUnit%d_Effect%d]," % (u, s)
        fs = "fx%d_%d_" % (u, s)
        add(fs + "type", "EFFECT", "enum", 0, options=EFFECTS, ctl=gs + "effect_idx")
        add(fs + "en", "ON", "toggle", 1, ctl=gs + "enabled")
        add(fs + "meta", "META", "knob", 0.5, ctl=gs + "meta")
        for p in range(1, 5):
            add(fs + "p%d" % p, "P%d" % p, "knob", 0.5, ctl=gs + "parameter%d" % p, dynamic_name=True, dynamic_display=True)

# ---- the wheel pages' extra text
add("wheel_l_txt", "LEFT", "text", ctl="[reMixXXL],wheel_l_text")
add("wheel_r_txt", "RIGHT", "text", ctl="[reMixXXL],wheel_r_text")
add("touch_txt", "TOUCH", "text", ctl="[reMixXXL],touch_text")

# ---- appended in 0.1.1 (MPC stores values by index: only ever append)
XFADER_MODES = ["SLOW", "SMOOTH", "FAST", "CUT", "REV SLOW", "REV FAST"]
add("xf_mode", "XFDR MODE", "enum", 0, options=XFADER_MODES, ctl="[reMixXXL],xf_mode")
# ---- appended in 0.1.2: the list row for the data wheel. 1..6 = the six rows of the page; 0 and 7 are the wheel running off the top / the
# bottom, which the engine turns into one entry up / down (and a page turn) and then puts back, so the wheel scrolls the whole list
add("br_row", "LIST ROW", "int", 1, 0, 7, ctl="[reMixXXL],browse_row", display="int")
# ---- appended in 0.1.2: the jog centre's BPM number: three digit pictures a deck (0..9 = frames 0..9, frame 10 blank), moved by the engine
for n in range(1, 5):
    for k, nm in (("h", "HUNDREDS"), ("t", "TENS"), ("u", "UNITS")):
        add("d%d_bpm_%s" % (n, k), "BPM " + nm, "knob", 1.0, ctl="[Channel%d],bpm_%s" % (n, k), deck=n)

# ---- appended in 0.1.2: the companion page's address as a QR code on the WHEEL page's centre disc: SHOW QR (0 = the BPM, 1 = the code;
# a tap on the disc toggles) and the code's 33 rows x 5 bytes (a film-strip frame a byte: frame = value * 255, the skin draws 8 modules)
add("qr_show", "SHOW QR", "toggle", 0, ctl="[reMixXXL],qr_show")
for i in range(33 * 5):
    add("q%03d" % i, "QR %d" % (i + 1), "knob", 0.0, ctl="[reMixXXL],qr%03d" % i)

# ---- appended in 0.1.2: the MASTER TEMPO lamp blinks at the master deck's beat (0/1, moved by the engine)
add("mt_led", "MASTER TEMPO LAMP", "toggle", 0, ctl="[reMixXXL],mt_led")
# ---- appended in 0.1.2: the jog wheel's spin: a frame of the ring's film strip, moved by the engine with the platter (not a control)
for n in range(1, 5):
    add("d%d_jog" % n, "JOG SPIN", "knob", 0.0, ctl="[Channel%d],jog_disp" % n, deck=n)
# ---- appended in 0.1.4: four touch strips round the jog ring (left, right, top, bottom), each an endless drag: the engine reads how far a
# finger moved one and puts it back to the middle (0.5), so the wheel never runs out; left/right take an up-down drag, top/bottom a sideways one
for n in range(1, 5):
    for k, nm in (("l", "LEFT"), ("r", "RIGHT"), ("t", "TOP"), ("b", "BOTTOM")):
        add("d%d_jog%s" % (n, k), "JOG " + nm, "knob", 0.5, ctl="[Channel%d],jog_%s" % (n, k), deck=n)


def ff(v):
    return "%.6gf" % float(v) if "." in "%.6g" % float(v) or "e" in "%.6g" % float(v) else "%.6g.0f" % float(v)


def main():
    json.dump({"params": P}, open(os.path.join(ROOT, "params.json"), "w"), indent=1)
    L = ["/* GENERATED by tools/gen_params.py: reMixXXL's parameter keys. Do not edit. */", "#pragma once",
         "#define RX_NPARAMS %d" % len(P), "enum {"]
    for e in P:
        L.append("    RXP_%s," % e["key"].upper())
    L.append("};")
    L.append("typedef struct { const char *key; const char *ctl; int kind; int deck; float def; float lo, hi; int nopt; } rx_param_def_t;")
    L.append("enum { RXK_KNOB, RXK_TOGGLE, RXK_TRIGGER, RXK_ENUM, RXK_TEXT, RXK_INT };")
    L.append("static const rx_param_def_t RX_PARAMS[RX_NPARAMS] = {")
    kinds = {"knob": "RXK_KNOB", "toggle": "RXK_TOGGLE", "trigger": "RXK_TRIGGER", "enum": "RXK_ENUM", "text": "RXK_TEXT", "int": "RXK_INT"}
    for e in P:
        L.append('    {"%s", "%s", %s, %d, %s, %s, %s, %d},' % (e["key"], e.get("ctl", ""), kinds[e["kind"]], e.get("deck", 0),
                                                                   ff(e["default"]), ff(e["min"]), ff(e["max"]), len(e.get("options") or [])))
    L.append("};")
    # option lists the engine needs
    L.append("static const char *const RX_RATE_RANGES[] = {%s};" % ", ".join('"%s"' % o for o in RATE_RANGES))
    L.append("static const float RX_RATE_RANGE_VALUES[] = {0.06f, 0.10f, 0.16f, 0.90f};")
    L.append("static const char *const RX_EFFECTS[] = {%s};" % ", ".join('"%s"' % o for o in EFFECTS))
    L.append("#define RX_EFFECTS_N %d" % len(EFFECTS))
    L.append("static const double RX_LOOP_SIZES[] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32};")
    L.append("#define RX_MAPPINGS_MAX %d" % MAPPINGS_MAX)
    open(os.path.join(ROOT, "src", "remix_params.h"), "w").write("\n".join(L) + "\n")
    print("gen_params: %d parameters" % len(P))


if __name__ == "__main__":
    main()
