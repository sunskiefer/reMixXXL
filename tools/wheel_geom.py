# the new WHEEL page parts, boxes on the user's picture (b frame); skin = b + OFF
OFF = (43, 22)
PARTS = {  # id: [(half, key, kind, (x,y,w,h))]
    10: [("L", "loop_in", "trigger", (136, 31, 69, 84)), ("R", "loop_out", "trigger", (205, 31, 76, 84))],
    11: [(None, "reloop", "trigger", (296, 31, 78, 66))],
    9: [("L", "bl4", "trigger", (136, 116, 69, 57)), ("R", "bl8", "trigger", (205, 116, 76, 57))],
    14: [("L", "prev", "trigger", (160, 173, 76, 77)), ("R", "next", "trigger", (236, 173, 76, 77))],
    15: [("L", "back", "trigger", (160, 252, 76, 78)), ("R", "fwd", "trigger", (236, 252, 76, 78))],
    3: [(None, "cue", "trigger", (197, 339, 131, 118))],
    4: [(None, "play", "toggle", (197, 458, 131, 138))],
    12: [("L", "bjback", "trigger", (4, 418, 90, 53)), ("R", "bjfwd", "trigger", (94, 418, 86, 53))],
    13: [("L", "slip", "toggle", (4, 472, 88, 101)), ("R", "rev", "toggle", (92, 472, 88, 101))],
    17: [("L", "vinyl", "toggle", (1027, 28, 76, 63))],
    18: [("L", "sync", "trigger", (1023, 126, 78, 66)), ("R", "master", "toggle", (1101, 126, 79, 66))],
    19: [(None, "keysync", "trigger", (1037, 206, 129, 48))],
    8: [(None, "rate_range", "popup", (1037, 254, 129, 80))],
    7: [(None, "keylock", "toggle", (1037, 335, 129, 80))],
    6: [(None, "rate_reset", "trigger", (887, 427, 38, 57))],
    16: [(None, "jogfeel", "rknob", (1072, 441, 114, 108))],
}
FADER = (966, 298, 66, 306)       # unchanged
LAMP_C = (1101.5, 392)            # the red lamp centre on the picture
KNOB = (55.5, 60, 23)             # jog adjust knob body in its box (cx, cy, r)

# 0.1.20: the right-hand column (these parts, the tempo fader 5, the static art right of RIGHT_X0 and the JOG ADJUST knob) is moved this many
# pixels to the right on the page, so no space is left at the right edge (the picture + OFF ends 50 px short of it)
RIGHT_SHIFT = 43
RIGHT_PARTS = (5, 6, 7, 8, 16, 17, 18, 19)
