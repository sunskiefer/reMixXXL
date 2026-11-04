/* reMixXXL text: TrueType glyphs (stb_truetype) rendered once per size into a cache, then blended into the screen's
 * ARGB page buffer. Sizes used by the screen: 26 and 34 px (the plugin family's rule: never smaller than 26 px).
 *
 * "px" is the em size, as PIL's ImageFont.truetype(size) and the skin builder count it (stbtt's EmToPixels mapping),
 * so a 26 px label here is the same height as a 26 px label in the skin.
 *
 * Without a font file (screen_set_panel_font never called, or the file missing) a built-in 5 x 7 bitmap font scaled
 * x4 (28 px tall) stands in, so the readouts stay readable and never small.
 *
 * All memory is taken when a size is first prepared (font_prepare); drawing allocates nothing. */
#pragma once
#include <stdint.h>

int font_load(const char *ttf_path);                 /* 1: loaded; 0: not (the bitmap fallback is used) */
int font_loaded(void);
int font_prepare(int px);                            /* render the ASCII glyphs of one size into the cache; 1 ok */

/* the pixel buffer text is drawn into: ARGB8888, little endian (bytes B G R A), premultiplied alpha */
typedef struct { uint8_t *p; int w, h, pitch; } font_target_t;

/* draw s with its em box's top-left at (x, y) (so y + px is roughly the baseline + descent); returns the width.
 * rgb: 0xRRGGBB; alpha 0..255 (the text's own opacity; glyph coverage multiplies it). Clipped to clip (or the
 * target when clip is NULL). */
typedef struct { int x, y, w, h; } font_clip_t;
int font_draw(font_target_t *t, int x, int y, int px, uint32_t rgb, int alpha, const char *s, const font_clip_t *clip);
int font_width(int px, const char *s);
int font_height(int px);                             /* the line height to reserve for that size */
