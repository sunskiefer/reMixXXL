/* reMixXXL: what overlay.c and screen.c share beyond remix.h (the public contract). Nothing else includes this. */
#pragma once
#include <stdint.h>
#include "remix.h"

/* the platter's centre-label frames (overlay_set_platter_frames): a strip of ARGB8888 pictures w x h, n of them, one
 * per step of a turn; NULL when none were given (screen.c then draws a plain disc). Read by the overlay thread only. */
const uint8_t *overlay_platter_frames(int *w, int *h, int *n);

/* the part of a page the screen ever draws into (skin pixels), the union of that page's boxes: the overlay sets an
 * XRGB plane (no transparency) to just this rectangle so the rest of the skin stays visible. w = 0: nothing drawn. */
void screen_used_rect(int page, rx_box_t *r);

/* the page buffer's pixel layout: ARGB8888 little endian (bytes B, G, R, A), premultiplied alpha; alpha 0 = the skin
 * shows through. Everything screen.c draws is either fully opaque (the boxes: black background) or a tint it drew
 * itself, so the plane and the test PNGs agree. */
#define RX_ARGB_BPP 4
