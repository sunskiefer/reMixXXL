/* reMixXXL: the screen renderer offline (native x86: it only tests screen.c + font.c, nothing of the device).
 *
 * Fills a fake rx_view_t with a synthetic track on every deck (the rx_wave_columns / rx_overview stubs below make
 * a waveform with a kick on every beat, a bass line and hats), draws each page into a 1280 x 628 ARGB buffer and
 * writes build/screen_test/<page>.png (the transparent part shown over a dark grey, so the boxes stand out), plus
 * a few frames of the DECK and WHEELS pages in motion. Then it times screen_draw per page (steady state: the caches
 * warm, the waveform scrolling) and prints ms a frame.
 *
 *   test/run_b.sh   builds and runs it */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#include "remix.h"
#include "boxes.h"
#include "overlay_priv.h"

/* ------------------------------------------------------------------------------------------------ the stubs */
#define TRACK_SEC 240.0
#define TRACK_BPM 126.0
static rx_wave_col_t *g_cols[RX_DECKS];
static int g_ncols[RX_DECKS];
static rx_wave_col_t g_ov[RX_DECKS][RX_OVERVIEW_W];
static int g_loaded[RX_DECKS];

static void make_track(int deck, double sec, double bpm, unsigned seed) {
    int n = (int)(sec * RX_WAVE_PX_PER_SEC);
    g_cols[deck] = calloc((size_t)n, sizeof(rx_wave_col_t));
    g_ncols[deck] = n;
    double per = 60.0 / bpm;
    srand(seed);
    for (int i = 0; i < n; i++) {
        double t = (double)i / RX_WAVE_PX_PER_SEC;
        double beat = fmod(t, per) / per;                       /* 0..1 within the beat */
        int bar = (int)(t / (per * 4));
        double section = (bar / 8) % 4 == 2 ? 0.35 : 1.0;      /* a breakdown every so often */
        double kick = exp(-beat * 9) * section;
        double bass = (0.45 + 0.25 * sin(t * 0.7 + seed)) * section * (fmod(t, per * 2) < per * 1.5 ? 1 : 0.3);
        double hat = (fmod(t + per / 2, per) / per < 0.12 ? 0.8 : 0.15) * (section > 0.5 ? 1 : 0.4);
        double noise = (rand() % 100) / 100.0 * 0.15;
        double lo = fmin(1, kick * 0.9 + bass * 0.5), mi = fmin(1, 0.25 + bass * 0.4 + kick * 0.3 + noise), hi = fmin(1, hat * 0.7 + noise);
        double all = fmin(1, 0.35 * lo + 0.35 * mi + 0.3 * hi + kick * 0.4);
        if (t < 0.4 || t > sec - 2) { lo = mi = hi = all = 0; }   /* silence at the ends */
        g_cols[deck][i].low = (uint8_t)(lo * 255);
        g_cols[deck][i].mid = (uint8_t)(mi * 255);
        g_cols[deck][i].high = (uint8_t)(hi * 255);
        g_cols[deck][i].all = (uint8_t)(all * 255);
    }
    for (int x = 0; x < RX_OVERVIEW_W; x++) {
        int c0 = (int)((long)x * n / RX_OVERVIEW_W), c1 = (int)((long)(x + 1) * n / RX_OVERVIEW_W);
        rx_wave_col_t m = {0, 0, 0, 0};
        for (int c = c0; c < c1; c++) {
            if (g_cols[deck][c].low > m.low) m.low = g_cols[deck][c].low;
            if (g_cols[deck][c].mid > m.mid) m.mid = g_cols[deck][c].mid;
            if (g_cols[deck][c].high > m.high) m.high = g_cols[deck][c].high;
            if (g_cols[deck][c].all > m.all) m.all = g_cols[deck][c].all;
        }
        g_ov[deck][x] = m;
    }
    g_loaded[deck] = 1;
}

int rx_wave_columns(int deck, rx_wave_col_t *out, int first_col, int n) {
    if (!g_loaded[deck] || first_col < 0) return 0;
    int avail = g_ncols[deck] - first_col;
    if (avail <= 0) return 0;
    if (n > avail) n = avail;
    memcpy(out, g_cols[deck] + first_col, (size_t)n * sizeof *out);
    return n;
}
int rx_wave_total_cols(int deck) { return g_loaded[deck] ? g_ncols[deck] : 0; }
int rx_overview(int deck, rx_wave_col_t *out) {
    if (!g_loaded[deck]) return 0;
    memcpy(out, g_ov[deck], sizeof g_ov[deck]);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ the view */
static void fill_view(rx_view_t *v, double t) {
    memset(v, 0, sizeof *v);
    v->zoom = 8;
    v->deck_select = 1;
    v->wheel_deck[0] = 0;
    v->wheel_deck[1] = 1;
    v->cpu_pct = 23.4;
    v->master_vu_l = 0.7f;
    v->master_vu_r = 0.75f;
    const char *titles[4] = {"Blue Monday", "Strings Of Life", "Pacific State", "Energy Flash"};
    const char *artists[4] = {"New Order", "Rhythim Is Rhythim", "808 State", "Joey Beltram"};
    for (int k = 0; k < RX_DECKS; k++) {
        rx_deck_view_t *d = &v->deck[k];
        d->loaded = k != 3;
        d->analysing = k == 3;
        d->analyse_pct = 62;
        d->duration = TRACK_SEC;
        d->bpm = TRACK_BPM + k;
        d->beat0 = 0.42;
        d->rate = k == 1 ? 1.02 : 1.0;
        d->playing = k < 2;
        d->pos = 61.3 + k * 37 + (d->playing ? t * d->rate : 0);
        d->loop_on = k == 1;
        d->loop_in = k == 1 ? d->pos - 1.2 : -1;
        d->loop_out = k == 1 ? d->pos + 2.6 : -1;
        d->cue = 32.0;
        for (int i = 0; i < RX_HOTCUES; i++) { d->hotcue[i] = i < 5 ? 20 + i * 48.3 + k * 3 : -1; d->hotcue_colour[i] = i; }
        d->hotcue[1] = d->pos + 1.1;   /* one in view */
        d->vu_l = 0.3f + 0.15f * k;
        d->vu_r = 0.35f + 0.15f * k;
        snprintf(d->title, sizeof d->title, "%s", titles[k]);
        snprintf(d->artist, sizeof d->artist, "%s", artists[k]);
        d->platter_angle = (float)fmod(d->pos / (60.0 / 33.333), 1.0);
        d->touch_zone = k == 0 ? 2 : k == 1 ? 1 : 0;
    }
}

/* ------------------------------------------------------------------------------------------------ output */
static void write_png(const char *path, const uint8_t *argb, int w, int h) {
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        const uint8_t *s = argb + (size_t)i * 4;
        int a = s[3];
        /* premultiplied over a dark grey (the skin stands in) */
        rgb[i * 3 + 0] = (uint8_t)(s[2] + 70 * (255 - a) / 255);
        rgb[i * 3 + 1] = (uint8_t)(s[1] + 70 * (255 - a) / 255);
        rgb[i * 3 + 2] = (uint8_t)(s[0] + 74 * (255 - a) / 255);
    }
    stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    printf("wrote %s\n", path);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "build/screen_test";
    const char *font = argc > 2 ? argv[2] : "third_party/fonts/TitilliumWeb-SemiBold.ttf";
    mkdir(out, 0755);
    screen_set_panel_font(font);
    for (int k = 0; k < 3; k++) make_track(k, TRACK_SEC, TRACK_BPM + k, 7u + k);
    /* a platter strip for the test: 36 frames of a disc with a stripe, 170 px */
    {
        int fw = 2 * RX_WHEEL_0_LABEL_R, fh = fw, fn = 36;
        uint8_t *strip = calloc((size_t)fw * fh * 4 * fn, 1);
        for (int i = 0; i < fn; i++) {
            float a = (float)i / fn * 6.2831853f;
            for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) {
                float dx = x + 0.5f - fw / 2.0f, dy = y + 0.5f - fh / 2.0f, d = sqrtf(dx * dx + dy * dy);
                if (d > fw / 2.0f - 1) continue;
                float u = dx * cosf(a) + dy * sinf(a), w = -dx * sinf(a) + dy * cosf(a);
                uint8_t *p = strip + (((size_t)i * fh + y) * fw + x) * 4;
                int stripe = fabsf(w) < 6 && u > 0;
                p[0] = stripe ? 60 : 30; p[1] = stripe ? 106 : 30; p[2] = stripe ? 201 : 34; p[3] = 255;
                if (d < 7) { p[0] = p[1] = p[2] = 210; }
            }
        }
        /* overlay.c's store isn't linked here: the test's own copy of the hook */
        extern void test_set_platter(const uint8_t *s, int w, int h, int n);
        test_set_platter(strip, fw, fh, fn);
    }
    int W = RX_SKIN_W, H = RX_SKIN_H, P = W * 4;
    uint8_t *buf = calloc((size_t)H, P);
    rx_view_t v;
    const char *names[RX_PAGES] = {"overview", "deck", "browse", "mixer", "fx", "wheels", "wheel"};
    char path[512];
    for (int page = 0; page < RX_PAGES; page++) {
        fill_view(&v, 0);
        screen_draw(page, &v, buf, W, H, P, 0);
        if (page == RX_PAGE_DECK || page == RX_PAGE_WHEELS) {   /* a few frames on: the waveform scrolls, the label turns */
            for (int f = 1; f <= 12; f++) { fill_view(&v, f / 30.0); screen_draw(page, &v, buf, W, H, P, f / 30.0); }
        }
        snprintf(path, sizeof path, "%s/%s.png", out, names[page]);
        write_png(path, buf, W, H);
    }
    /* the DECK page zoomed out, and the WHEEL page with the bitmap font (no TTF) is not tested here */
    fill_view(&v, 0);
    v.zoom = 60;
    screen_draw(RX_PAGE_DECK, &v, buf, W, H, P, 0);
    snprintf(path, sizeof path, "%s/deck_zoom60.png", out);
    write_png(path, buf, W, H);
    v.zoom = 2;
    v.deck[1].pos = 0.3;   /* the preroll pattern before the start */
    screen_draw(RX_PAGE_DECK, &v, buf, W, H, P, 0);
    snprintf(path, sizeof path, "%s/deck_zoom2_start.png", out);
    write_png(path, buf, W, H);
    /* timing: steady state, playing, 300 frames a page */
    printf("\nms a frame (x86, steady state, 300 frames each):\n");
    for (int page = 0; page < RX_PAGES; page++) {
        fill_view(&v, 0);
        screen_draw(page, &v, buf, W, H, P, 0);
        double t0 = now_ms();
        for (int f = 1; f <= 300; f++) { fill_view(&v, f / 30.0); screen_draw(page, &v, buf, W, H, P, f / 30.0); }
        double ms = (now_ms() - t0) / 300;
        printf("  %-9s %6.2f ms\n", names[page], ms);
    }
    /* a seek every frame on DECK (the worst case: a full waveform redraw each time) */
    {
        double t0 = now_ms();
        for (int f = 1; f <= 100; f++) { fill_view(&v, f * 3.7); screen_draw(RX_PAGE_DECK, &v, buf, W, H, P, 0); }
        printf("  deck, a seek every frame (full redraw): %6.2f ms\n", (now_ms() - t0) / 100);
    }
    return 0;
}

/* the platter store for the test (overlay.c, which owns the real one, is not built on x86) */
static const uint8_t *t_pl; static int t_w, t_h, t_n;
void test_set_platter(const uint8_t *s, int w, int h, int n) { t_pl = s; t_w = w; t_h = h; t_n = n; }
const uint8_t *overlay_platter_frames(int *w, int *h, int *n) { if (!t_pl) return NULL; *w = t_w; *h = t_h; *n = t_n; return t_pl; }
