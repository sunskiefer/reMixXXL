#!/usr/bin/env python3
"""The spinning centre label of the jog wheel, as ready-drawn frames for the live screen (src/screen.c draws the frame
that matches the platter's angle: a filmstrip, so the Force never rotates a picture at run time).

    python3 tools/gen_platter_frames.py [out.png]       default: build/Screen/platter_frames.png

The label disc is cut from the user's CDJ photo (art/platter_nobrand.png, centre and radius from
tools/remix_pages.py: PLATTER_CIRCLE and LABEL_R) with a circular alpha mask, then turned in FRAMES steps (a vertical
strip of square RGBA frames). The user will replace the label art later; this just takes whatever is in the photo."""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import remix_pages as P  # noqa: E402

FRAMES = 72            # 5 degrees a frame
PAD = 2


def main(out):
    cx, cy, _ = P.PLATTER_CIRCLE
    r = P.LABEL_R
    src = Image.open(os.path.join(ROOT, "art", "platter_nobrand.png")).convert("RGBA")
    d = 2 * r + 2 * PAD
    disc = src.crop((cx - r - PAD, cy - r - PAD, cx + r + PAD, cy + r + PAD))
    mask = Image.new("L", (d, d), 0)
    ImageDraw.Draw(mask).ellipse((PAD, PAD, d - PAD - 1, d - PAD - 1), fill=255)
    disc.putalpha(mask)
    strip = Image.new("RGBA", (d, d * FRAMES), (0, 0, 0, 0))
    for i in range(FRAMES):
        fr = disc.rotate(-360.0 * i / FRAMES, resample=Image.BICUBIC, center=(d / 2, d / 2))
        fr.putalpha(mask)
        strip.alpha_composite(fr, (0, i * d))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    strip.save(out)
    print("gen_platter_frames: %d frames of %dx%d -> %s" % (FRAMES, d, d, out))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "Screen", "platter_frames.png"))
