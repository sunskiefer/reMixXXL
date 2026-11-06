#!/usr/bin/env python3
"""The WHEEL page's turning ring, for the live screen (src/screen.c turns this one picture to any angle, like XYZ-STACK's wheel) and the fixed
light that lies over it (it does not turn).

    python3 tools/gen_ring.py [out_dir]       default: build/Screen/ring_<D>.png and ring_light_<D>.png

D = twice RX_WHEEL_0_R in src/boxes.h (the WHEEL page's jog wheel square, the same D tools/remix_skin.py gives the skin's ring). The ring is the
user's art/ui/jog/ring_flat_user.png (no baked light) padded to a square about its own centre, at the wheel's own size."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import remix_skin as K  # noqa: E402


def wheel_d():
    src = open(os.path.join(ROOT, "src", "boxes.h")).read()
    return 2 * int(re.search(r"#define RX_WHEEL_0_R (\d+)", src).group(1))


def main(out_dir):
    from PIL import Image
    D = wheel_d()
    os.makedirs(out_dir, exist_ok=True)
    big = K._jog_ring_square().resize((D * 2, D * 2), Image.LANCZOS)
    ring = big.resize((D, D), Image.LANCZOS)
    ring.save(os.path.join(out_dir, "ring_%d.png" % D))
    K.jog_light(D).save(os.path.join(out_dir, "ring_light_%d.png" % D))
    print("gen_ring: %dx%d -> %s" % (D, D, out_dir))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "Screen"))
