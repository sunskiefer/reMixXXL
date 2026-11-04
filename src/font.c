/* reMixXXL text (see font.h): stb_truetype glyphs cached per size, blended into the ARGB page buffer.
 *
 * The cache: for each prepared size, the 95 printable ASCII glyphs as 8-bit coverage bitmaps in one block of memory,
 * taken once at font_prepare (the screen prepares its sizes on its first frame, so nothing is allocated per frame).
 * Kerning is applied from the font's kern table (Titillium has one) so "11" and "AV" sit as in the skin.
 *
 * Drawing blends "over": the target is premultiplied ARGB (what the overlay plane shows; on a fully transparent
 * or fully opaque pixel premultiplied and straight alpha agree, and everything the screen draws is one of those
 * except glyph edges, which sit on the boxes' opaque black). */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "stb/stb_truetype.h"
#include "font.h"

#define FONT_SIZES 4
#define FONT_FIRST 32
#define FONT_LAST 126
#define FONT_N (FONT_LAST - FONT_FIRST + 1)

typedef struct { int x0, y0, w, h, adv; uint8_t *bits; } glyph_t;   /* x0,y0: offset from the pen (y from the em top) */
typedef struct { int px, line_h, ascent; glyph_t g[FONT_N]; uint8_t *pool; } size_cache_t;

static stbtt_fontinfo g_font;
static uint8_t *g_font_data;
static int g_loaded;
static size_cache_t g_sizes[FONT_SIZES];
static int g_nsizes;

/* ------------------------------------------------------------------------------------------------ the fallback font */
/* a 5 x 7 bitmap font for the printable ASCII set (rows top to bottom, 5 bits each, MSB left) used only when no
 * TrueType file is there; scaled by an integer so it is never under 26 px tall */
static const uint8_t FB[FONT_N][7] = {
    {0,0,0,0,0,0,0},{4,4,4,4,4,0,4},{10,10,10,0,0,0,0},{10,10,31,10,31,10,10},{4,15,20,14,5,30,4},{24,25,2,4,8,19,3},
    {12,18,20,8,21,18,13},{12,4,8,0,0,0,0},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},{0,4,21,14,21,4,0},{0,4,4,31,4,4,0},
    {0,0,0,0,12,4,8},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},{0,1,2,4,8,16,0},{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},{0,12,12,0,12,12,0},{0,12,12,0,12,4,8},{2,4,8,16,8,4,2},{0,0,31,0,31,0,0},
    {8,4,2,1,2,4,8},{14,17,1,2,4,0,4},{14,17,1,13,21,21,14},{14,17,17,17,31,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {28,18,17,17,17,18,28},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},
    {17,17,25,21,19,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
    {17,17,10,4,10,17,17},{17,17,17,10,4,4,4},{31,1,2,4,8,16,31},{14,8,8,8,8,8,14},{0,16,8,4,2,1,0},{14,2,2,2,2,2,14},
    {4,10,17,0,0,0,0},{0,0,0,0,0,0,31},{8,4,2,0,0,0,0},{0,0,14,1,15,17,15},{16,16,22,25,17,17,30},{0,0,14,16,16,17,14},
    {1,1,13,19,17,17,15},{0,0,14,17,31,16,14},{6,9,8,28,8,8,8},{0,15,17,17,15,1,14},{16,16,22,25,17,17,17},{4,0,12,4,4,4,14},
    {2,0,6,2,2,18,12},{16,16,18,20,24,20,18},{12,4,4,4,4,4,14},{0,0,26,21,21,17,17},{0,0,22,25,17,17,17},{0,0,14,17,17,17,14},
    {0,0,30,17,30,16,16},{0,0,13,19,15,1,1},{0,0,22,25,16,16,16},{0,0,14,16,14,1,30},{8,8,28,8,8,9,6},{0,0,17,17,17,19,13},
    {0,0,17,17,17,10,4},{0,0,17,17,21,21,10},{0,0,17,10,4,10,17},{0,0,17,17,15,1,14},{0,0,31,2,4,8,31},{2,4,4,8,4,4,2},
    {4,4,4,4,4,4,4},{8,4,4,2,4,4,8},{0,0,8,21,2,0,0},
};

/* ------------------------------------------------------------------------------------------------ loading */
int font_load(const char *ttf_path) {
    if (!ttf_path || !ttf_path[0]) return 0;
    FILE *f = fopen(ttf_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return 0; }
    uint8_t *data = malloc((size_t)n);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return 0; }
    fclose(f);
    if (!stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0))) { free(data); return 0; }
    free(g_font_data);
    g_font_data = data;
    g_loaded = 1;
    /* sizes prepared before (with the fallback) are rebuilt with the real font */
    for (int i = 0; i < g_nsizes; i++) { int px = g_sizes[i].px; free(g_sizes[i].pool); memset(&g_sizes[i], 0, sizeof g_sizes[i]); g_sizes[i].px = px; }
    int n0 = g_nsizes;
    g_nsizes = 0;
    for (int i = 0; i < n0; i++) font_prepare(g_sizes[i].px);
    return 1;
}

int font_loaded(void) { return g_loaded; }

static size_cache_t *find_size(int px) {
    for (int i = 0; i < g_nsizes; i++) if (g_sizes[i].px == px) return &g_sizes[i];
    return NULL;
}

/* the size's cache: one pass to measure every glyph, one block of memory, one pass to render */
int font_prepare(int px) {
    if (px < 26) px = 26;   /* the family's rule */
    if (find_size(px)) return 1;
    if (g_nsizes >= FONT_SIZES) return 0;
    size_cache_t *s = &g_sizes[g_nsizes];
    memset(s, 0, sizeof *s);
    s->px = px;
    if (g_loaded) {
        float sc = stbtt_ScaleForPixelHeight(&g_font, (float)px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
        s->ascent = (int)(asc * sc + 0.5f);
        s->line_h = (int)((asc - desc) * sc + 0.5f);
        size_t total = 0;
        for (int c = 0; c < FONT_N; c++) {
            int x0, y0, x1, y1, adv, lsb;
            stbtt_GetCodepointBitmapBox(&g_font, c + FONT_FIRST, sc, sc, &x0, &y0, &x1, &y1);
            stbtt_GetCodepointHMetrics(&g_font, c + FONT_FIRST, &adv, &lsb);
            s->g[c].x0 = x0;
            s->g[c].y0 = s->ascent + y0;            /* from the em top */
            s->g[c].w = x1 - x0;
            s->g[c].h = y1 - y0;
            s->g[c].adv = (int)(adv * sc + 0.5f);
            total += (size_t)(s->g[c].w * s->g[c].h);
        }
        s->pool = calloc(total ? total : 1, 1);
        if (!s->pool) return 0;
        uint8_t *p = s->pool;
        for (int c = 0; c < FONT_N; c++) {
            glyph_t *g = &s->g[c];
            g->bits = p;
            if (g->w > 0 && g->h > 0) stbtt_MakeCodepointBitmap(&g_font, p, g->w, g->h, g->w, sc, sc, c + FONT_FIRST);
            p += (size_t)(g->w * g->h);
        }
    } else {
        /* the bitmap fallback: 5 x 7 cells scaled so the glyph is at least 26 px tall... (7 * 4 = 28) */
        int k = (px + 6) / 7;
        if (k < 4) k = 4;
        s->ascent = 7 * k;
        s->line_h = 9 * k;
        size_t total = (size_t)FONT_N * (size_t)(5 * k) * (size_t)(7 * k);
        s->pool = calloc(total, 1);
        if (!s->pool) return 0;
        uint8_t *p = s->pool;
        for (int c = 0; c < FONT_N; c++) {
            glyph_t *g = &s->g[c];
            g->x0 = 0; g->y0 = 0; g->w = 5 * k; g->h = 7 * k; g->adv = 6 * k; g->bits = p;
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (FB[c][r] & (16 >> col))
                        for (int yy = 0; yy < k; yy++) memset(p + (size_t)(r * k + yy) * (5 * k) + col * k, 255, (size_t)k);
            p += (size_t)(5 * k) * (size_t)(7 * k);
        }
    }
    g_nsizes++;
    return 1;
}

int font_height(int px) {
    size_cache_t *s = find_size(px);
    if (!s && font_prepare(px)) s = find_size(px);
    return s ? s->line_h : px;
}

static int kern(const size_cache_t *s, int a, int b) {
    if (!g_loaded) return 0;
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)s->px);
    return (int)(stbtt_GetCodepointKernAdvance(&g_font, a, b) * sc + (sc > 0 ? 0.5f : -0.5f));
}

int font_width(int px, const char *str) {
    size_cache_t *s = find_size(px);
    if (!s && font_prepare(px)) s = find_size(px);
    if (!s || !str) return 0;
    int w = 0, prev = 0;
    for (const unsigned char *u = (const unsigned char *)str; *u; u++) {
        int c = *u < FONT_FIRST || *u > FONT_LAST ? '?' : *u;
        if (prev) w += kern(s, prev, c);
        w += s->g[c - FONT_FIRST].adv;
        prev = c;
    }
    return w;
}

/* ------------------------------------------------------------------------------------------------ drawing */
int font_draw(font_target_t *t, int x, int y, int px, uint32_t rgb, int alpha, const char *str, const font_clip_t *clip) {
    size_cache_t *s = find_size(px);
    if (!s && font_prepare(px)) s = find_size(px);
    if (!s || !str || !t || !t->p) return 0;
    int cx0 = 0, cy0 = 0, cx1 = t->w, cy1 = t->h;
    if (clip) {
        if (clip->x > cx0) cx0 = clip->x;
        if (clip->y > cy0) cy0 = clip->y;
        if (clip->x + clip->w < cx1) cx1 = clip->x + clip->w;
        if (clip->y + clip->h < cy1) cy1 = clip->y + clip->h;
    }
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = 0;
    int pen = x, prev = 0;
    for (const unsigned char *u = (const unsigned char *)str; *u; u++) {
        int c = *u < FONT_FIRST || *u > FONT_LAST ? '?' : *u;
        if (prev) pen += kern(s, prev, c);
        const glyph_t *gl = &s->g[c - FONT_FIRST];
        int gx = pen + gl->x0, gy = y + gl->y0;
        for (int yy = 0; yy < gl->h; yy++) {
            int py = gy + yy;
            if (py < cy0 || py >= cy1) continue;
            const uint8_t *src = gl->bits + (size_t)yy * gl->w;
            uint8_t *row = t->p + (size_t)py * t->pitch;
            for (int xx = 0; xx < gl->w; xx++) {
                int px_ = gx + xx;
                if (px_ < cx0 || px_ >= cx1) continue;
                int cov = src[xx];
                if (!cov) continue;
                int a = (cov * alpha + 127) / 255;        /* this pixel's alpha */
                if (!a) continue;
                uint8_t *d = row + px_ * 4;
                int ia = 255 - a;
                d[0] = (uint8_t)((b * a + d[0] * ia + 127) / 255);
                d[1] = (uint8_t)((g * a + d[1] * ia + 127) / 255);
                d[2] = (uint8_t)((r * a + d[2] * ia + 127) / 255);
                d[3] = (uint8_t)(a + (d[3] * ia + 127) / 255);
            }
        }
        pen += gl->adv;
        prev = c;
    }
    return pen - x;
}
