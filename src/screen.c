/* reMixXXL's screen: draws the live parts of a page into the overlay's ARGB page buffer (overlay.c calls screen_draw
 * every frame with the page found on the display and the engine's latest rx_view_t).
 *
 * Only the boxes of the page (boxes.h) are drawn; the rest of the buffer stays transparent so the skin shows through.
 * The buffer is cleared once when the page changes, then every frame repaints just that page's boxes (each box is
 * painted whole: a black background and its content), so nothing of an earlier frame is left behind.
 *
 * The look follows Mixxx (src/waveform/renderers, waveformoverviewrenderer.cpp):
 *   - the RGB waveform: per column the band peaks low/mid/high give the colour (low -> red, mid -> green, high ->
 *     blue, normalised so the strongest band is full), the overall peak gives the height, symmetric about the middle
 *     (WaveformRendererRGB::draw); a dim reference line along the middle (AxesColor)
 *   - the overview: the same per column over the whole track, normalised to the track's peak
 *   - the beat grid: thin lines on every beat, brighter on downbeats; hot cue marks with their number in the Mixxx
 *     hot cue colours; the loop shaded; the end-of-track preroll pattern (triangles) beyond the track's ends
 *   - the play position: a white line (PlayPosColor), 2 px, centred in the scrolling waveform
 *
 * Cost (one Cortex-A17 core, software): the overview strips and the deck's overview are rendered once per track
 * load into cached images and copied each frame; the scrolling waveform keeps a cached image that is shifted by the
 * integer pixel delta each frame and only the new columns are drawn (a full redraw on seek / zoom / track change).
 * Everything else is lines, small rectangles and text. All memory is taken on the first call (screen_draw allocates
 * nothing after that). */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remix.h"
#include "boxes.h"
#include "overlay_priv.h"
#include "font.h"

/* ------------------------------------------------------------------------------------------------ colours */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COL_WHITE RGB(255, 255, 255)
#define COL_ACCENT RGB(201, 106, 60)            /* the plugin's orange */
#define COL_AXES RGB(245, 245, 245)             /* Mixxx AxesColor default */
#define COL_PLAYPOS RGB(255, 255, 255)          /* Mixxx PlayPosColor (= axes) */
#define COL_PREROLL RGB(110, 110, 110)          /* the preroll triangles (SignalColor, dimmed) */
#define COL_LOOP RGB(32, 190, 68)               /* the loop shade: Mixxx green */
#define COL_BEAT RGB(255, 255, 255)
#define COL_DIM RGB(150, 150, 150)
/* Mixxx RGB waveform band colours (WaveformSignalColors: SignalRGBLow/Mid/High default Qt::red/green/blue) */
static const float RGB_LOW[3] = {1.0f, 0.0f, 0.0f}, RGB_MID[3] = {0.0f, 1.0f, 0.0f}, RGB_HIGH[3] = {0.0f, 0.0f, 1.0f};
/* Mixxx hot cue colour palette (PredefinedColorPalettes::kMixxxHotcueColorPalette) */
static const uint32_t HOTCUE_COL[8] = {0xC50A08, 0x32BE44, 0x42D4F4, 0xF8D200, 0x0044FF, 0xAF00CC, 0xFCA6D7, 0xF2F2FF};

#define TXT_PX 26                                /* the readouts (the family's minimum) */
#define TXT_BIG 34                               /* the deck numbers */
#define OV_LABEL_W (RX_OV_X - 8)                 /* the label tile left of each overview strip (remix_pages OV_LABEL_W) */
#define WAVE_MAX_COLS_PER_REDRAW 48000           /* 120 s at 400 columns a second: the zoom's ceiling */
#define ZOOM_MIN 1.0
#define ZOOM_MAX 120.0

/* ------------------------------------------------------------------------------------------------ state */
typedef struct { uint8_t *p; int w, h; } img_t;

typedef struct {                /* the overview images of one deck (rendered once per track) */
    img_t ov;                   /* RX_OV_W x RX_OV_H */
    img_t dk;                   /* RX_DK_OVER_W x RX_DK_OVER_H */
    char key[200];              /* title + artist + duration: a different key = a new track */
    int valid;
} deck_cache_t;

typedef struct {                /* the scrolling waveform's cache */
    img_t img;                  /* RX_DK_WAVE_W x RX_DK_WAVE_H: the waveform only (no beats, marks, playhead) */
    int valid, deck;
    long p0;                    /* the global pixel index of the image's left column (pixel = time * pps) */
    double pps, duration;
    char key[200];
} wave_cache_t;

typedef struct { int zone_drawn; } wheel_state_t;

static struct {
    int inited;
    int last_page;
    deck_cache_t deck[RX_DECKS];
    wave_cache_t wave;
    rx_wave_col_t *cols;        /* scratch: columns fetched for one redraw */
    rx_wave_col_t *ovcols;      /* scratch: RX_OVERVIEW_W columns */
    wheel_state_t wheel[3];     /* WHEELS left, right; WHEEL */
    font_target_t ft;
} S;

static int alloc_img(img_t *i, int w, int h) {
    i->w = w; i->h = h;
    i->p = calloc((size_t)w * h, 4);
    return i->p != NULL;
}

static int screen_init(void) {
    if (S.inited) return 1;
    for (int d = 0; d < RX_DECKS; d++) {
        if (!alloc_img(&S.deck[d].ov, RX_OV_W, RX_OV_H)) return 0;
        if (!alloc_img(&S.deck[d].dk, RX_DK_OVER_W, RX_DK_OVER_H)) return 0;
    }
    if (!alloc_img(&S.wave.img, RX_DK_WAVE_W, RX_DK_WAVE_H)) return 0;
    S.cols = calloc(WAVE_MAX_COLS_PER_REDRAW + 16, sizeof *S.cols);
    S.ovcols = calloc(RX_OVERVIEW_W, sizeof *S.ovcols);
    if (!S.cols || !S.ovcols) return 0;
    font_prepare(TXT_PX);
    font_prepare(TXT_BIG);
    S.last_page = -1;
    S.inited = 1;
    return 1;
}

void screen_set_panel_font(const char *ttf_path) {
    font_load(ttf_path);
}

/* ------------------------------------------------------------------------------------------------ primitives */
/* the page buffer of this frame (set at the top of screen_draw) */
static uint8_t *B;
static int BW, BH, BP;

static inline void put_opaque(uint8_t *d, uint32_t rgb) { d[0] = (uint8_t)rgb; d[1] = (uint8_t)(rgb >> 8); d[2] = (uint8_t)(rgb >> 16); d[3] = 255; }

/* "over" with a straight-alpha colour onto the premultiplied buffer */
static inline void blend_px(uint8_t *d, uint32_t rgb, int a) {
    int ia = 255 - a;
    d[0] = (uint8_t)(((rgb & 255) * a + d[0] * ia + 127) / 255);
    d[1] = (uint8_t)((((rgb >> 8) & 255) * a + d[1] * ia + 127) / 255);
    d[2] = (uint8_t)((((rgb >> 16) & 255) * a + d[2] * ia + 127) / 255);
    d[3] = (uint8_t)(a + (d[3] * ia + 127) / 255);
}

static void clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > BW) *w = BW - *x;
    if (*y + *h > BH) *h = BH - *y;
}

/* an opaque rectangle (rgb), or transparent when clear */
static void fill_rect(int x, int y, int w, int h, uint32_t rgb, int clear) {
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    uint8_t *row0 = B + (size_t)y * BP + x * 4;
    if (clear) { for (int j = 0; j < h; j++) memset(row0 + (size_t)j * BP, 0, (size_t)w * 4); return; }
    for (int i = 0; i < w; i++) put_opaque(row0 + i * 4, rgb);
    for (int j = 1; j < h; j++) memcpy(row0 + (size_t)j * BP, row0, (size_t)w * 4);
}

static void blend_rect(int x, int y, int w, int h, uint32_t rgb, int a) {
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0 || a <= 0) return;
    for (int j = 0; j < h; j++) {
        uint8_t *d = B + (size_t)(y + j) * BP + x * 4;
        for (int i = 0; i < w; i++, d += 4) blend_px(d, rgb, a);
    }
}

static void vline(int x, int y0, int y1, uint32_t rgb, int a) {
    if (x < 0 || x >= BW) return;
    if (y0 < 0) y0 = 0;
    if (y1 > BH) y1 = BH;
    uint8_t *d = B + (size_t)y0 * BP + x * 4;
    if (a >= 255) for (int y = y0; y < y1; y++, d += BP) put_opaque(d, rgb);
    else for (int y = y0; y < y1; y++, d += BP) blend_px(d, rgb, a);
}

/* an image (premultiplied ARGB, w x h) copied into the buffer at (x, y) (clipped) */
static void blit(const img_t *im, int x, int y) {
    int sx = 0, sy = 0, w = im->w, h = im->h;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > BW) w = BW - x;
    if (y + h > BH) h = BH - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) memcpy(B + (size_t)(y + j) * BP + x * 4, im->p + ((size_t)(sy + j) * im->w + sx) * 4, (size_t)w * 4);
}

/* a straight-alpha ARGB picture (B G R A bytes, w x h, pitch w*4) blended over the buffer, its centre at (cx, cy),
 * scaled to dw x dh (nearest neighbour: the platter frames are made for the biggest wheel and sampled down for the
 * smaller ones; 1:1 when the sizes agree) */
static void blend_pic(const uint8_t *pic, int w, int h, int cx, int cy, int dw, int dh) {
    int x0 = cx - dw / 2, y0 = cy - dh / 2;
    for (int j = 0; j < dh; j++) {
        int py = y0 + j;
        if (py < 0 || py >= BH) continue;
        int sy = dh == h ? j : (int)((long)j * h / dh);
        const uint8_t *srow = pic + (size_t)sy * w * 4;
        for (int i = 0; i < dw; i++) {
            int px = x0 + i;
            if (px < 0 || px >= BW) continue;
            const uint8_t *s = srow + (dw == w ? i : (int)((long)i * w / dw)) * 4;
            if (!s[3]) continue;
            uint8_t *d = B + (size_t)py * BP + px * 4;
            if (s[3] == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
            else blend_px(d, RGB(s[2], s[1], s[0]), s[3]);
        }
    }
}

/* a ring (annulus r0..r1) tinted; r0 = 0 gives a disc. Soft 1 px edges. */
static void ring(float cx, float cy, float r0, float r1, uint32_t rgb, int a) {
    int x0 = (int)floorf(cx - r1 - 1), x1 = (int)ceilf(cx + r1 + 1), y0 = (int)floorf(cy - r1 - 1), y1 = (int)ceilf(cy + r1 + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= BW) x1 = BW - 1;
    if (y1 >= BH) y1 = BH - 1;
    float r0i = r0 > 0.5f ? r0 : 0;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        uint8_t *row = B + (size_t)y * BP;
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, d = sqrtf(dx * dx + dy * dy);
            float cov = r1 + 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            if (r0i > 0) { float ci = d - r0i + 0.5f; if (ci <= 0) continue; if (ci < cov) cov = ci; }
            int aa = (int)(a * cov + 0.5f);
            if (aa > 0) blend_px(row + x * 4, rgb, aa);
        }
    }
}

/* an arc of the ring r0..r1 from angle a0 to a1 (turns, 0 = up, clockwise) */
static void arc(float cx, float cy, float r0, float r1, float a0, float a1, uint32_t rgb, int a) {
    if (a1 <= a0) return;
    int x0 = (int)floorf(cx - r1 - 1), x1 = (int)ceilf(cx + r1 + 1), y0 = (int)floorf(cy - r1 - 1), y1 = (int)ceilf(cy + r1 + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= BW) x1 = BW - 1;
    if (y1 >= BH) y1 = BH - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        uint8_t *row = B + (size_t)y * BP;
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, d = sqrtf(dx * dx + dy * dy);
            if (d < r0 - 0.5f || d > r1 + 0.5f) continue;
            float ang = atan2f(dx, -dy) / 6.2831853f;        /* 0 up, clockwise */
            if (ang < 0) ang += 1;
            if (ang < a0 || ang > a1) continue;
            float cov = r1 + 0.5f - d;
            float ci = d - r0 + 0.5f;
            if (ci < cov) cov = ci;
            if (cov > 1) cov = 1;
            if (cov <= 0) continue;
            blend_px(row + x * 4, rgb, (int)(a * cov + 0.5f));
        }
    }
}

/* a disc made transparent (what the spinning label is painted over each frame) */
static void clear_disc(int cx, int cy, int r) {
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= BH) continue;
        int dy = y - cy, hw = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - hw, x1 = cx + hw + 1;
        if (x0 < 0) x0 = 0;
        if (x1 > BW) x1 = BW;
        if (x1 > x0) memset(B + (size_t)y * BP + x0 * 4, 0, (size_t)(x1 - x0) * 4);
    }
}

/* a 2 px wide line from the centre outwards (the plain disc's marker) */
static void radial(float cx, float cy, float r0, float r1, float turns, uint32_t rgb) {
    float s = sinf(turns * 6.2831853f), c = -cosf(turns * 6.2831853f);
    for (float r = r0; r <= r1; r += 0.5f) {
        int x = (int)(cx + s * r), y = (int)(cy + c * r);
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++)
            if (x + dx >= 0 && x + dx < BW && y + dy >= 0 && y + dy < BH) put_opaque(B + (size_t)(y + dy) * BP + (x + dx) * 4, rgb);
    }
}

static void text(int x, int y, int px, uint32_t rgb, int a, const char *s, const rx_box_t *clip) {
    font_clip_t c;
    if (clip) { c.x = clip->x; c.y = clip->y; c.w = clip->w; c.h = clip->h; }
    font_draw(&S.ft, x, y, px, rgb, a, s, clip ? &c : NULL);
}
static void text_right(int xr, int y, int px, uint32_t rgb, int a, const char *s, const rx_box_t *clip) {
    text(xr - font_width(px, s), y, px, rgb, a, s, clip);
}

static void fmt_time(char *buf, size_t n, double t) {
    int neg = t < 0;
    if (neg) t = -t;
    int m = (int)(t / 60), s = (int)t % 60, d = (int)((t - floor(t)) * 10);
    snprintf(buf, n, "%s%02d:%02d.%d", neg ? "-" : "", m, s, d);
}

/* ------------------------------------------------------------------------------------------------ the waveform data */
/* one column's Mixxx RGB colour and its height fraction */
static inline uint32_t band_colour(int low, int mid, int high) {
    float r = low * RGB_LOW[0] + mid * RGB_MID[0] + high * RGB_HIGH[0];
    float g = low * RGB_LOW[1] + mid * RGB_MID[1] + high * RGB_HIGH[1];
    float b = low * RGB_LOW[2] + mid * RGB_MID[2] + high * RGB_HIGH[2];
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx <= 0) return 0;
    return RGB((int)(r / mx * 255 + 0.5f), (int)(g / mx * 255 + 0.5f), (int)(b / mx * 255 + 0.5f));
}

/* the overview image of a deck: RX_OVERVIEW_W columns (rx_overview) squeezed to w x h, normalised to the track's
 * peak as Mixxx's overview renderer does; black background */
static void render_overview(img_t *im, int deck) {
    memset(im->p, 0, (size_t)im->w * im->h * 4);
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) im->p[i * 4 + 3] = 255;
    if (!rx_overview(deck, S.ovcols)) return;
    int peak = 1;
    for (int i = 0; i < RX_OVERVIEW_W; i++) if (S.ovcols[i].all > peak) peak = S.ovcols[i].all;
    int half = im->h / 2;
    for (int x = 0; x < im->w; x++) {
        int c0 = (int)((long)x * RX_OVERVIEW_W / im->w), c1 = (int)((long)(x + 1) * RX_OVERVIEW_W / im->w);
        if (c1 <= c0) c1 = c0 + 1;
        int lo = 0, mi = 0, hi = 0, al = 0;
        for (int c = c0; c < c1 && c < RX_OVERVIEW_W; c++) {
            if (S.ovcols[c].low > lo) lo = S.ovcols[c].low;
            if (S.ovcols[c].mid > mi) mi = S.ovcols[c].mid;
            if (S.ovcols[c].high > hi) hi = S.ovcols[c].high;
            if (S.ovcols[c].all > al) al = S.ovcols[c].all;
        }
        uint32_t col = band_colour(lo, mi, hi);
        if (!col) continue;
        int hh = (int)((long)al * (half - 1) / peak);
        for (int y = half - hh; y <= half + hh && y < im->h; y++) if (y >= 0) put_opaque(im->p + ((size_t)y * im->w + x) * 4, col);
    }
}

static void deck_key(const rx_deck_view_t *d, char *key, size_t n) {
    snprintf(key, n, "%d|%.1f|%.80s|%.80s", d->loaded, d->duration, d->title, d->artist);
}

static deck_cache_t *deck_images(int deck, const rx_deck_view_t *d) {
    deck_cache_t *c = &S.deck[deck];
    char key[200];
    deck_key(d, key, sizeof key);
    if (!c->valid || strcmp(key, c->key)) {
        render_overview(&c->ov, deck);
        render_overview(&c->dk, deck);
        strcpy(c->key, key);
        c->valid = 1;
    }
    return c;
}

/* ------------------------------------------------------------------------------------------------ the scrolling waveform */
/* draw the columns x0..x1 (image pixels) of the wave cache: pixel p = S.wave.p0 + x is time p / pps */
static void wave_draw_cols(int x0, int x1, int deck) {
    img_t *im = &S.wave.img;
    double pps = S.wave.pps, cps = RX_WAVE_PX_PER_SEC / pps;   /* columns per pixel */
    int half = im->h / 2;
    long total = rx_wave_total_cols(deck);
    /* clear the columns (black, opaque) and the centre line */
    for (int x = x0; x < x1; x++) {
        for (int y = 0; y < im->h; y++) { uint8_t *d = im->p + ((size_t)y * im->w + x) * 4; d[0] = d[1] = d[2] = 0; d[3] = 255; }
        put_opaque(im->p + ((size_t)half * im->w + x) * 4, RGB(60, 60, 60));
    }
    /* the track's columns for this pixel range, fetched in one go */
    long pa = S.wave.p0 + x0, pb = S.wave.p0 + x1;
    long ca = (long)floor(pa * cps), cb = (long)ceil(pb * cps) + 1;
    if (ca < 0) ca = 0;
    if (cb > total) cb = total;
    int n = (int)(cb - ca);
    if (n > WAVE_MAX_COLS_PER_REDRAW) n = WAVE_MAX_COLS_PER_REDRAW;
    int got = n > 0 ? rx_wave_columns(deck, S.cols, (int)ca, n) : 0;
    for (int x = x0; x < x1; x++) {
        long p = S.wave.p0 + x;
        double t = p / pps;
        if (t < 0 || (S.wave.duration > 0 && t >= S.wave.duration)) {
            /* the preroll pattern (Mixxx WaveformRendererPreroll): triangles pointing at the track's start / end */
            int ph = (int)(((p % 40) + 40) % 40);
            if (ph < 24) {
                float f = t < 0 ? ph / 24.0f : 1.0f - ph / 24.0f;
                int hh = (int)(f * (im->h / 5));
                for (int y = half - hh; y <= half + hh; y++) put_opaque(im->p + ((size_t)y * im->w + x) * 4, COL_PREROLL);
            }
            continue;
        }
        long c0 = (long)floor(p * cps) - ca, c1 = (long)floor((p + 1) * cps) - ca;
        if (c1 <= c0) c1 = c0 + 1;
        int lo = 0, mi = 0, hi = 0, al = 0;
        for (long c = c0; c < c1; c++) {
            if (c < 0 || c >= got) continue;
            const rx_wave_col_t *w = &S.cols[c];
            if (w->low > lo) lo = w->low;
            if (w->mid > mi) mi = w->mid;
            if (w->high > hi) hi = w->high;
            if (w->all > al) al = w->all;
        }
        uint32_t col = band_colour(lo, mi, hi);
        if (!col) continue;
        int hh = al * (half - 2) / 255;
        for (int y = half - hh; y <= half + hh; y++) put_opaque(im->p + ((size_t)y * im->w + x) * 4, col);
    }
}

/* bring the cache to the play position: shift by the integer pixel delta and draw what came in; a full redraw on
 * a seek, a zoom change or a new track */
static void wave_update(int deck, const rx_deck_view_t *d, double zoom) {
    wave_cache_t *w = &S.wave;
    img_t *im = &w->img;
    double pps = im->w / zoom;
    char key[200];
    deck_key(d, key, sizeof key);
    long p0 = lround(d->pos * pps) - im->w / 2;
    int full = !w->valid || w->deck != deck || fabs(w->pps - pps) > 1e-9 || strcmp(key, w->key) || w->duration != d->duration;
    if (full) {
        w->deck = deck; w->pps = pps; w->duration = d->duration; w->p0 = p0; w->valid = 1;
        strcpy(w->key, key);
        wave_draw_cols(0, im->w, deck);
        return;
    }
    long delta = p0 - w->p0;
    if (delta == 0) return;
    if (delta >= im->w || delta <= -im->w) { w->p0 = p0; wave_draw_cols(0, im->w, deck); return; }
    int dd = (int)delta;
    size_t rowb = (size_t)im->w * 4;
    if (dd > 0) {   /* the picture moves left: keep [dd, w) at [0, w - dd), draw the right */
        for (int y = 0; y < im->h; y++) memmove(im->p + (size_t)y * rowb, im->p + (size_t)y * rowb + (size_t)dd * 4, rowb - (size_t)dd * 4);
        w->p0 = p0;
        wave_draw_cols(im->w - dd, im->w, deck);
    } else {
        dd = -dd;
        for (int y = 0; y < im->h; y++) memmove(im->p + (size_t)y * rowb + (size_t)dd * 4, im->p + (size_t)y * rowb, rowb - (size_t)dd * 4);
        w->p0 = p0;
        wave_draw_cols(0, dd, deck);
    }
}

/* the marks over a strip showing time t_left..t_right across x..x+w: beat grid (optional), loop, hot cues, playhead */
static void draw_marks(const rx_box_t *bx, const rx_deck_view_t *d, double t_left, double t_right, int beats, int big_marks) {
    double pps = bx->w / (t_right - t_left);
    /* the loop shade */
    if (d->loop_in >= 0 && d->loop_out > d->loop_in) {
        int xa = (int)((d->loop_in - t_left) * pps), xb = (int)((d->loop_out - t_left) * pps);
        if (xa < 0) xa = 0;
        if (xb > bx->w) xb = bx->w;
        if (xb > xa) blend_rect(bx->x + xa, bx->y, xb - xa, bx->h, COL_LOOP, d->loop_on ? 70 : 32);
        if (d->loop_in >= t_left && d->loop_in <= t_right) vline(bx->x + (int)((d->loop_in - t_left) * pps), bx->y, bx->y + bx->h, COL_LOOP, 255);
        if (d->loop_out >= t_left && d->loop_out <= t_right) vline(bx->x + (int)((d->loop_out - t_left) * pps), bx->y, bx->y + bx->h, COL_LOOP, 255);
    }
    /* the beat grid: every beat, brighter on downbeats (every fourth from the first beat) */
    if (beats && d->bpm > 0) {
        double per = 60.0 / d->bpm;
        long k0 = (long)ceil((t_left - d->beat0) / per), k1 = (long)floor((t_right - d->beat0) / per);
        if (k1 - k0 < 4000)
            for (long k = k0; k <= k1; k++) {
                int x = bx->x + (int)((d->beat0 + k * per - t_left) * pps);
                int down = ((k % 4) + 4) % 4 == 0;
                vline(x, bx->y, bx->y + bx->h, COL_BEAT, down ? 170 : 70);
            }
    }
    /* the main cue (orange) and the hot cues: a line and, when big, a numbered tag at the top */
    if (d->cue >= t_left && d->cue <= t_right) vline(bx->x + (int)((d->cue - t_left) * pps), bx->y, bx->y + bx->h, COL_ACCENT, 220);
    for (int i = 0; i < RX_HOTCUES; i++) {
        if (d->hotcue[i] < 0 || d->hotcue[i] < t_left || d->hotcue[i] > t_right) continue;
        int x = bx->x + (int)((d->hotcue[i] - t_left) * pps);
        uint32_t col = HOTCUE_COL[d->hotcue_colour[i] & 7];
        vline(x, bx->y, bx->y + bx->h, col, 255);
        vline(x + 1, bx->y, bx->y + bx->h, col, 160);
        if (big_marks) {
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            int tw = font_width(TXT_PX, n) + 12, th = font_height(TXT_PX);
            fill_rect(x, bx->y, tw, th, col, 0);
            uint32_t ink = (col == 0xF2F2FF || col == 0xF8D200 || col == 0x42D4F4 || col == 0xFCA6D7) ? RGB(0, 0, 0) : COL_WHITE;
            text(x + 6, bx->y, TXT_PX, ink, 255, n, bx);
        } else {
            /* a small triangle in the cue colour at the top */
            for (int j = 0; j < 7; j++) for (int dx = -(6 - j); dx <= 6 - j; dx++) {
                int px = x + dx, py = bx->y + j;
                if (px >= bx->x && px < bx->x + bx->w) put_opaque(B + (size_t)py * BP + px * 4, col);
            }
        }
    }
}

static void playhead(const rx_box_t *bx, double frac) {
    int x = bx->x + (int)(frac * bx->w);
    if (x < bx->x) x = bx->x;
    if (x > bx->x + bx->w - 2) x = bx->x + bx->w - 2;
    vline(x, bx->y, bx->y + bx->h, COL_PLAYPOS, 255);
    vline(x + 1, bx->y, bx->y + bx->h, COL_PLAYPOS, 255);
}

/* ------------------------------------------------------------------------------------------------ the pages */

static void page_deck(const rx_view_t *v) {
    int k = v->deck_select < 0 ? 0 : v->deck_select >= RX_DECKS ? RX_DECKS - 1 : v->deck_select;
    const rx_deck_view_t *d = &v->deck[k];
    rx_box_t wb = {RX_DK_WAVE_X, RX_DK_WAVE_Y, RX_DK_WAVE_W, RX_DK_WAVE_H};
    rx_box_t ob = {RX_DK_OVER_X, RX_DK_OVER_Y, RX_DK_OVER_W, RX_DK_OVER_H};
    double zoom = v->zoom;
    if (!(zoom >= ZOOM_MIN)) zoom = 8;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    char s[96];
    if (!d->loaded) {
        fill_rect(wb.x, wb.y, wb.w, wb.h, 0, 0);
        fill_rect(ob.x, ob.y, ob.w, ob.h, 0, 0);
        S.wave.valid = 0;
        if (d->analysing) snprintf(s, sizeof s, "DECK %d  ANALYSING %d%%", k + 1, d->analyse_pct); else snprintf(s, sizeof s, "DECK %d  NO TRACK", k + 1);
        text(wb.x + 16, wb.y + (wb.h - font_height(TXT_PX)) / 2, TXT_PX, COL_DIM, 255, s, &wb);
        return;
    }
    /* the scrolling waveform: the cache shifted to the position, then the marks and the playhead over it */
    wave_update(k, d, zoom);
    blit(&S.wave.img, wb.x, wb.y);
    double t_left = S.wave.p0 / S.wave.pps, t_right = t_left + zoom;
    draw_marks(&wb, d, t_left, t_right, 1, 1);
    playhead(&wb, 0.5);
    /* the readouts in the box's top corners: elapsed / remaining left; BPM, rate, CPU right */
    char a[32], b[32];
    fmt_time(a, sizeof a, d->pos);
    fmt_time(b, sizeof b, -(d->duration - d->pos));
    snprintf(s, sizeof s, "%s  %s", a, b);
    int th = font_height(TXT_PX);
    int tw = font_width(TXT_PX, s);
    blend_rect(wb.x + 4, wb.y + 4, tw + 12, th, RGB(0, 0, 0), 150);
    text(wb.x + 10, wb.y + 4, TXT_PX, COL_WHITE, 255, s, &wb);
    double rate_pct = (fabs(d->rate) - 1) * 100;
    if (d->bpm > 0) snprintf(s, sizeof s, "%.1f BPM  %+.1f%%  CPU %.0f%%", d->bpm * fabs(d->rate), rate_pct, v->cpu_pct);
    else snprintf(s, sizeof s, "-- BPM  %+.1f%%  CPU %.0f%%", rate_pct, v->cpu_pct);
    tw = font_width(TXT_PX, s);
    blend_rect(wb.x + wb.w - tw - 16, wb.y + 4, tw + 12, th, RGB(0, 0, 0), 150);
    text_right(wb.x + wb.w - 10, wb.y + 4, TXT_PX, COL_WHITE, 255, s, &wb);
    if (d->rate < 0) text(wb.x + 10, wb.y + 4 + th, TXT_PX, COL_ACCENT, 255, "REVERSE", &wb);
    /* the overview strip under it */
    deck_cache_t *c = deck_images(k, d);
    blit(&c->dk, ob.x, ob.y);
    draw_marks(&ob, d, 0, d->duration > 0 ? d->duration : 1, 0, 0);
    playhead(&ob, d->duration > 0 ? d->pos / d->duration : 0);
}

/* a segmented VU bar: two columns (L, R) in box x,y,w,h; levels 0..1 linear, shown on a dB scale -60..0 */
static void vu_bar(const int *bx, float l, float r) {
    const int seg = 8, gap = 2, n = bx[3] / (seg + gap);
    int colw = (bx[2] - 2) / 2;
    fill_rect(bx[0], bx[1], bx[2], bx[3], 0, 0);
    for (int side = 0; side < 2; side++) {
        float v = side ? r : l;
        float db = v > 1e-4f ? 20 * log10f(v) : -100;
        float frac = (db + 60) / 60;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int lit = (int)(frac * n + 0.5f);
        int x = bx[0] + side * (colw + 2);
        for (int i = 0; i < n; i++) {
            float f = (float)i / n;
            uint32_t col = f < 0.7f ? RGB(40, 200, 70) : f < 0.9f ? RGB(240, 180, 30) : RGB(230, 40, 40);
            int on = i < lit;
            uint32_t c = on ? col : RGB(((col >> 16) & 255) / 5, ((col >> 8) & 255) / 5, (col & 255) / 5);
            int y = bx[1] + bx[3] - (i + 1) * (seg + gap) + gap;
            fill_rect(x, y, colw, seg, c, 0);
        }
    }
}

static void page_mixer(const rx_view_t *v) {
    for (int k = 0; k < RX_DECKS; k++) vu_bar(RX_MX_VU[k], v->deck[k].vu_l, v->deck[k].vu_r);
    vu_bar(RX_MX_MASTER_VU, v->master_vu_l, v->master_vu_r);
}


/* ------------------------------------------------------------------------------------------------ the WHEEL page's turning ring
 * The ring picture (tools/gen_ring.py: ring_<D>.png, straight RGBA, and the fixed light ring_light_<D>.png) is turned to ANY angle each frame, as
 * XYZ-STACK turns its wheel: every pixel of the ring's annulus is looked up in the picture turned back by the angle (bilinear), the light is laid
 * over it (it does not turn), and the result replaces the buffer's pixels there. The skin's own ring (a film strip, same size and place) lies under
 * it; its edges never differ, so only this one shows. */
typedef struct { int d; float rin; uint8_t *ring, *light; } ring_pic_t;
static ring_pic_t *g_ring_pic;

void screen_set_ring(const uint8_t *ring_rgba, const uint8_t *light_rgba, int d) {
    if (!ring_rgba || !light_rgba || d < 16) return;
    ring_pic_t *R = calloc(1, sizeof *R);
    if (!R) return;
    R->d = d;
    R->ring = malloc((size_t)d * d * 4);
    R->light = malloc((size_t)d * d * 4);
    if (!R->ring || !R->light) { free(R->ring); free(R->light); free(R); return; }
    memcpy(R->ring, ring_rgba, (size_t)d * d * 4);
    memcpy(R->light, light_rgba, (size_t)d * d * 4);
    /* the ring's inner edge: walking out from the middle along the centre row, the first pixel that is the ring */
    int c = d / 2, k = 0;
    while (k < c && R->ring[((size_t)c * d + c + k) * 4 + 3] > 10) k++;    /* the picture's own centre knob (the skin's centre disc covers it): skipped */
    while (k < c && R->ring[((size_t)c * d + c + k) * 4 + 3] <= 10) k++;   /* the gap, then the ring itself */
    R->rin = (float)k;
    __atomic_store_n(&g_ring_pic, R, __ATOMIC_RELEASE);   /* an older picture (a skin reload) is left in place: never freed while the overlay thread may read it */
}

static inline float rclamp_(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float g_ring_turn_drawn = -10.0f;
static int g_ring_force = 1;

static inline void ring_texel(const ring_pic_t *R, int x, int y, float *pr, float *pg, float *pb, float *pa) {
    if (x < 0 || y < 0 || x >= R->d || y >= R->d) { *pr = *pg = *pb = *pa = 0; return; }
    const uint8_t *t = R->ring + ((size_t)y * R->d + x) * 4;
    float a = t[3];
    *pr = t[0] * a / 255.0f; *pg = t[1] * a / 255.0f; *pb = t[2] * a / 255.0f; *pa = a;
}

static void draw_ring_turned(int cx, int cy, float turn) {
    const ring_pic_t *R = __atomic_load_n(&g_ring_pic, __ATOMIC_ACQUIRE);
    if (!R) return;
    turn -= floorf(turn);
    if (!g_ring_force && fabsf(turn - g_ring_turn_drawn) < 0.0004f) return;   /* under 0.15 degree: nothing to redraw */
    g_ring_turn_drawn = turn; g_ring_force = 0;
    int D = R->d;
    float h = (float)D * 0.5f, ang = turn * 6.2831853f, ca = cosf(ang), sa = sinf(ang);
    float rin2 = (R->rin - 2.0f) * (R->rin - 2.0f), rout2 = (h + 2.0f) * (h + 2.0f);
    for (int y = 0; y < D; y++) {
        int py = cy - D / 2 + y;
        if (py < 0 || py >= BH) continue;
        float dy = (float)y + 0.5f - h;
        for (int x = 0; x < D; x++) {
            int px = cx - D / 2 + x;
            if (px < 0 || px >= BW) continue;
            float dx = (float)x + 0.5f - h, rr = dx * dx + dy * dy;
            if (rr < rin2 || rr > rout2) continue;
            float sx = dx * ca + dy * sa + h - 0.5f, sy = -dx * sa + dy * ca + h - 0.5f;   /* back to the unturned picture */
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - (float)x0, fy = sy - (float)y0;
            float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
            ring_texel(R, x0, y0, &r00, &g00, &b00, &a00); ring_texel(R, x0 + 1, y0, &r10, &g10, &b10, &a10);
            ring_texel(R, x0, y0 + 1, &r01, &g01, &b01, &a01); ring_texel(R, x0 + 1, y0 + 1, &r11, &g11, &b11, &a11);
            float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
            float a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
            uint8_t *d = B + (size_t)py * BP + px * 4;
            if (a < 0.5f) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            float rr_ = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11, gg = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11, bb = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
            const uint8_t *l = R->light + ((size_t)y * D + x) * 4;   /* the light does not turn: this pixel's own place */
            float la = l[3] / 255.0f, ia = 1.0f - la, an = a / 255.0f;
            rr_ = rr_ * ia + l[0] * la * an; gg = gg * ia + l[1] * la * an; bb = bb * ia + l[2] * la * an;
            d[0] = (uint8_t)rclamp_(bb + 0.5f, 0, 255); d[1] = (uint8_t)rclamp_(gg + 0.5f, 0, 255); d[2] = (uint8_t)rclamp_(rr_ + 0.5f, 0, 255); d[3] = (uint8_t)rclamp_(a + 0.5f, 0, 255);
        }
    }
}

/* one wheel: the centre label spinning, the position arc, the touch-zone tint, the readouts above and below */
static void wheel(int slot, int cx, int cy, int r, int rtop, int label_r, const rx_deck_view_t *d, int deck_no, int title_in_stripe) {
    wheel_state_t *ws = &S.wheel[slot];
    if (slot == 2) draw_ring_turned(cx, cy, d->platter_angle);   /* the WHEEL page: the ring turned to the platter's exact angle (smooth, like XYZ-STACK's wheel) */
    /* the touch-zone tint: a faint ring over the top (zone 1) or the rim (zone 2); redrawn only when it changes,
     * the area under it cleared first (the label and arc are painted over it every frame) */
    if (slot != 2 && d->touch_zone != ws->zone_drawn) {   /* (no zone tint on the WHEEL page: the ring itself shows the finger) */
        fill_rect(cx - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, 0, 1);
        if (d->touch_zone == 1) ring(cx, cy, label_r + 12, rtop, COL_ACCENT, 48);
        else if (d->touch_zone == 2) ring(cx, cy, rtop, r, COL_ACCENT, 48);
        ws->zone_drawn = d->touch_zone;
    }
    /* the label disc: cleared and redrawn every frame (it turns) */
    int lr = label_r + 10;
    clear_disc(cx, cy, lr);   /* inside the zone tint (which starts at label_r + 12) */
    int fw, fh, fn;
    const uint8_t *frames = overlay_platter_frames(&fw, &fh, &fn);
    float ang = d->platter_angle - floorf(d->platter_angle);
    if (frames) {
        int i = (int)(ang * fn) % fn;
        int dw = 2 * label_r, dh = (int)((long)fh * dw / (fw > 0 ? fw : 1));
        blend_pic(frames + (size_t)i * fw * fh * 4, fw, fh, cx, cy, dw, dh);
    } else {
        ring(cx, cy, 0, label_r, RGB(28, 28, 30), 255);
        ring(cx, cy, label_r - 2, label_r, RGB(90, 90, 95), 255);
        radial(cx, cy, 8, label_r - 6, ang, d->playing ? COL_ACCENT : COL_DIM);
        ring(cx, cy, 0, 6, RGB(200, 200, 200), 255);
    }
    /* the position arc: the elapsed part of the track around the label */
    ring(cx, cy, label_r + 3, label_r + 7, RGB(40, 40, 40), 255);
    if (d->loaded && d->duration > 0) arc(cx, cy, label_r + 3, label_r + 7, 0, (float)(d->pos / d->duration), COL_ACCENT, 255);
    /* the readouts: above the platter the track's name (or in the stripe), below it BPM and time */
    int th = font_height(TXT_PX);
    char s[256], a[32], b[32];
    rx_box_t top = {cx - r, cy - r - th - 6, 2 * r, th + 4};
    rx_box_t bot = {cx - r, cy + r + 4, 2 * r, th + 4};
    if (bot.y + th > RX_SKIN_H) bot.y = RX_SKIN_H - th;    /* the big wheel reaches the page's bottom: the text sits over its rim */
    if (top.y < RX_STRIPE_H) top.y = RX_STRIPE_H;
    fill_rect(top.x, top.y, top.w, top.h, 0, 1);
    fill_rect(bot.x, bot.y, bot.w, bot.h, 0, 1);
    if (d->artist[0]) snprintf(s, sizeof s, "%d: %s - %s", deck_no + 1, d->artist, d->title);
    else snprintf(s, sizeof s, "%d: %s", deck_no + 1, d->loaded ? d->title : "NO TRACK");
    if (title_in_stripe) {
        rx_box_t st = {16, (RX_STRIPE_H - th) / 2, cx - r - 24, th};
        fill_rect(st.x, st.y, st.w, st.h, 0, 1);
        text(st.x, st.y, TXT_PX, COL_WHITE, 255, s, &st);
    } else text(top.x + 4, top.y, TXT_PX, COL_WHITE, 255, s, &top);
    if (d->loaded) {
        fmt_time(a, sizeof a, d->pos);
        fmt_time(b, sizeof b, -(d->duration - d->pos));
        if (d->bpm > 0) snprintf(s, sizeof s, "%.1f BPM", d->bpm * fabs(d->rate)); else snprintf(s, sizeof s, "-- BPM");
        text(bot.x + 4, bot.y, TXT_PX, d->playing ? COL_ACCENT : COL_WHITE, 255, s, &bot);
        snprintf(s, sizeof s, "%s  %s", a, b);
        text_right(bot.x + bot.w - 4, bot.y, TXT_PX, COL_WHITE, 255, s, &bot);
    }
}

static void page_wheels(const rx_view_t *v) {
    int l = v->wheel_deck[0] & 3, r = v->wheel_deck[1] & 3;
    wheel(0, RX_WHEELS_0_CX, RX_WHEELS_0_CY, RX_WHEELS_0_R, RX_WHEELS_0_RTOP, RX_WHEELS_0_LABEL_R, &v->deck[l], l, 0);
    wheel(1, RX_WHEELS_1_CX, RX_WHEELS_1_CY, RX_WHEELS_1_R, RX_WHEELS_1_RTOP, RX_WHEELS_1_LABEL_R, &v->deck[r], r, 0);
}

static void page_wheel(const rx_view_t *v) {
    int k = v->wheel_deck[0] & 3;
    wheel(2, RX_WHEEL_0_CX, RX_WHEEL_0_CY, RX_WHEEL_0_R, RX_WHEEL_0_RTOP, RX_WHEEL_0_LABEL_R, &v->deck[k], k, 1);
}

/* ------------------------------------------------------------------------------------------------ the entry points */
void screen_used_rect(int page, rx_box_t *r) {
    int th = 36;   /* a 26 px line's height, rounded up */
    switch (page) {
    case RX_PAGE_DECK: *r = (rx_box_t){RX_DK_WAVE_X, RX_DK_WAVE_Y, RX_DK_WAVE_W, RX_DK_OVER_Y + RX_DK_OVER_H - RX_DK_WAVE_Y}; break;
    case RX_PAGE_MIXER: {
        int x0 = RX_MX_VU[0][0], x1 = RX_MX_MASTER_VU[0] + RX_MX_MASTER_VU[2], y0 = RX_MX_VU[0][1], y1 = RX_MX_VU[0][1] + RX_MX_VU[0][3];
        *r = (rx_box_t){x0, y0, x1 - x0, y1 - y0};
        break;
    }
    case RX_PAGE_WHEELS: {
        int x0 = RX_WHEELS_0_CX - RX_WHEELS_0_R - 2, x1 = RX_WHEELS_1_CX + RX_WHEELS_1_R + 2;
        int y0 = RX_WHEELS_0_CY - RX_WHEELS_0_R - th - 6, y1 = RX_WHEELS_0_CY + RX_WHEELS_0_R + th + 8;
        if (y0 < 0) y0 = 0;
        if (y1 > RX_SKIN_H) y1 = RX_SKIN_H;
        *r = (rx_box_t){x0, y0, x1 - x0, y1 - y0};
        break;
    }
    case RX_PAGE_WHEEL: {
        int x1 = RX_WHEEL_0_CX + RX_WHEEL_0_R + 2, y1 = RX_WHEEL_0_CY + RX_WHEEL_0_R + th + 8;
        if (y1 > RX_SKIN_H) y1 = RX_SKIN_H;
        *r = (rx_box_t){16, 0, x1 - 16, y1};
        break;
    }
    default: *r = (rx_box_t){0, 0, 0, 0}; break;
    }
}

void screen_draw(int page, const rx_view_t *v, uint8_t *argb, int w, int h, int pitch, double now_s) {
    (void)now_s;
    if (!argb || !v) return;
    if (!screen_init()) return;
    B = argb; BW = w; BH = h; BP = pitch;
    S.ft.p = argb; S.ft.w = w; S.ft.h = h; S.ft.pitch = pitch;
    if (page != S.last_page) {
        /* a new page: the whole buffer transparent once, then only the page's boxes are painted */
        for (int y = 0; y < h; y++) memset(argb + (size_t)y * pitch, 0, (size_t)w * 4);
        S.last_page = page;
        for (int i = 0; i < 3; i++) S.wheel[i].zone_drawn = -1;
        g_ring_force = 1;
    }
    switch (page) {
    case RX_PAGE_DECK: page_deck(v); break;
    case RX_PAGE_MIXER: page_mixer(v); break;
    case RX_PAGE_WHEELS: page_wheels(v); break;
    case RX_PAGE_WHEEL: page_wheel(v); break;
    default: break;   /* FX, BROWSE: nothing live */
    }
}
