/* web.c: the companion web page. The plugin runs a tiny web server (HTTP + WebSocket, no libraries) on the Force; any
 * browser on the same network opens http://<force ip>:<port>/ and sees the four scrolling RGB waveforms with a fixed
 * playhead, beat grid, cues, loops, overview strips and levels, drawn by the browser from the same data the Force's
 * own screen uses. Read-only: it never changes anything in the engine.
 *
 *   GET /            the page (web_page.h, generated from web/index.html)
 *   GET /w/<deck>    a deck's full-resolution waveform, 4 bytes per column (low, mid, high, all), RX_WAVE_PX_PER_SEC per second
 *   GET /o/<deck>    a deck's overview, RX_OVERVIEW_W columns, same 4 bytes
 *   GET /ws          WebSocket: a JSON text frame about every 16 ms with every deck's state; the page draws between
 *                    frames from position + rate, so the playhead stays smooth with minimal latency
 *
 * One thread per connection (at most WEB_MAX), no shared state but the latest view copy. Nothing here runs on the audio
 * thread except web_publish, which only try-locks and copies. */
#define _GNU_SOURCE
#include "remix.h"
#include "web_page.h"
#include "map_page.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include "qr.h"
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WEB_MAX 8
#define WEB_PORT_FIRST 6731        /* VADER's range, known to be reachable on the Force */
#define WEB_PORT_TRIES 10
#define PUSH_MS 16

struct rx_web {
    int lsock, port;
    pthread_t accept_th;
    atomic_int stop, clients;
    pthread_mutex_t lock;
    rx_view_t view;
    atomic_int have;
    char url[96];
    unsigned char qr[RX_QR_N * RX_QR_BYTES];   /* the page address as a QR code, 8 modules a byte (the skin's grid) */
    int qr_ok;
    char ip[64];
    rx_engine_t *eng;                 /* the page can touch the decks (scratch / nudge / a few buttons) */
};

#define SCRATCH_TICKS_PER_REV 720.0      /* same wheel as touch.c */
#define SCRATCH_RPM 33.3333333
#define SCRATCH_ALPHA (1.0 / 8)
#define SCRATCH_BETA (SCRATCH_ALPHA / 32)
/* the buttons the page may press, per deck (any other name is ignored) */
static const char *const WEB_BUTTONS[] = {"play", "cue_default", "beatsync", "sync_master", "vinyl_mode", "keylock", "quantize", "slip_enabled", "rate_set_default"};
#define WEB_BUTTONS_N ((int)(sizeof WEB_BUTTONS / sizeof WEB_BUTTONS[0]))
static ctl_id web_ids[RX_DECKS][4];     /* vinyl_mode, sync_master, keylock, quantize: shown on the page */
static int web_ids_ok;
static void web_lookup_ids(void) {
    if (web_ids_ok) return;
    static const char *const nm[4] = {"vinyl_mode", "sync_master", "keylock", "quantize"};
    for (int d = 0; d < RX_DECKS; d++) {
        char g[24]; snprintf(g, sizeof g, "[Channel%d]", d + 1);
        for (int k = 0; k < 4; k++) web_ids[d][k] = ctl_lookup(g, nm[k]);
    }
    web_ids_ok = 1;
}
static double web_ctl(ctl_id id) { return id >= 0 ? ctl_get(id) : 0; }

/* ------------------------------------------------------------------------------------------------ SHA-1 and base64 (for the WebSocket handshake) */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; } sha1_t;
static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
static void sha1_block(sha1_t *c, const uint8_t *p) {
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    sha1_t c = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
    for (size_t i = 0; i < len; i++) { c.buf[c.n++] = data[i]; if (c.n == 64) { sha1_block(&c, c.buf); c.n = 0; } }
    c.len = (uint64_t)len * 8;
    c.buf[c.n++] = 0x80;
    if (c.n > 56) { while (c.n < 64) c.buf[c.n++] = 0; sha1_block(&c, c.buf); c.n = 0; }
    while (c.n < 56) c.buf[c.n++] = 0;
    for (int i = 0; i < 8; i++) c.buf[56 + i] = (uint8_t)(c.len >> (56 - 8 * i));
    sha1_block(&c, c.buf);
    for (int i = 0; i < 5; i++) { out[i * 4] = c.h[i] >> 24; out[i * 4 + 1] = c.h[i] >> 16; out[i * 4 + 2] = c.h[i] >> 8; out[i * 4 + 3] = c.h[i]; }
}
static void b64(const uint8_t *in, int n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}
/* exposed for the self-test (tools/web_selftest.c) */
void rx_web_accept_key(const char *key, char *out29) {
    char buf[128]; snprintf(buf, sizeof buf, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t d[20]; sha1((const uint8_t *)buf, strlen(buf), d); b64(d, 20, out29);
}

/* ------------------------------------------------------------------------------------------------ sockets */
static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}
static void http_reply(int fd, const char *status, const char *type, const void *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", status, type, len);
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void json_str(char *out, size_t cap, const char *s) {
    size_t o = 0;
    for (; *s && o + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* one snapshot of every deck as JSON; returns the length */
static int build_json(const rx_view_t *v, char *out, int cap) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int o = snprintf(out, (size_t)cap, "{\"t\":%.3f,\"xf\":%.3f,\"cpu\":%.1f,\"vm\":[%.3f,%.3f],\"d\":[",
                     ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6, v->xfader, v->cpu_pct, v->master_vu_l, v->master_vu_r);
    for (int i = 0; i < RX_DECKS; i++) {
        const rx_deck_view_t *d = &v->deck[i];
        char ti[200], ar[200]; json_str(ti, sizeof ti, d->title); json_str(ar, sizeof ar, d->artist);
        o += snprintf(out + o, (size_t)(cap - o),
                      "%s{\"ld\":%d,\"an\":%d,\"ap\":%d,\"dur\":%.3f,\"bpm\":%.2f,\"b0\":%.4f,\"pos\":%.4f,\"rate\":%.5f,\"pl\":%d,"
                      "\"lp\":%d,\"li\":%.4f,\"lo\":%.4f,\"cue\":%.4f,\"key\":%.0f,\"vu\":[%.3f,%.3f],\"vol\":%.3f,\"ti\":\"%s\",\"ar\":\"%s\",\"vin\":%d,\"sm\":%d,\"kl\":%d,\"qn\":%d,\"hc\":[",
                      i ? "," : "", d->loaded, d->analysing, d->analyse_pct, d->duration, d->bpm, d->beat0, d->pos, d->rate, d->playing,
                      d->loop_on, d->loop_in, d->loop_out, d->cue, d->key, d->vu_l, d->vu_r, v->ch_vol[i], ti, ar,
                      web_ctl(web_ids[i][0]) > 0.5, web_ctl(web_ids[i][1]) > 0.5, web_ctl(web_ids[i][2]) > 0.5, web_ctl(web_ids[i][3]) > 0.5);
        for (int h = 0; h < RX_HOTCUES; h++)
            o += snprintf(out + o, (size_t)(cap - o), "%s[%.4f,%d]", h ? "," : "", d->hotcue[h], d->hotcue_colour[h]);
        o += snprintf(out + o, (size_t)(cap - o), "],\"nc\":%d}", rx_wave_total_cols(i));
        if (o >= cap - 400) break;
    }
    o += snprintf(out + o, (size_t)(cap - o), "]}");
    return o;
}

static int ws_send_text(int fd, const char *p, int n) {
    uint8_t h[4]; int hl = 0;
    h[hl++] = 0x81;
    if (n < 126) h[hl++] = (uint8_t)n; else { h[hl++] = 126; h[hl++] = (uint8_t)(n >> 8); h[hl++] = (uint8_t)n; }
    if (send_all(fd, h, (size_t)hl) < 0) return -1;
    return send_all(fd, p, (size_t)n);
}

typedef struct { rx_web_t *w; int fd; } conn_t;

static void serve_wave(int fd, int deck, int overview) {
    if (overview) {
        rx_wave_col_t *c = malloc(sizeof *c * RX_OVERVIEW_W);
        if (!c) { http_reply(fd, "500 Error", "text/plain", "memory", 6); return; }
        if (!rx_overview(deck, c)) memset(c, 0, sizeof *c * RX_OVERVIEW_W);
        http_reply(fd, "200 OK", "application/octet-stream", c, sizeof *c * RX_OVERVIEW_W);
        free(c); return;
    }
    int n = rx_wave_total_cols(deck);
    if (n <= 0) { http_reply(fd, "200 OK", "application/octet-stream", "", 0); return; }
    rx_wave_col_t *c = malloc(sizeof *c * (size_t)n);
    if (!c) { http_reply(fd, "500 Error", "text/plain", "memory", 6); return; }
    int got = rx_wave_columns(deck, c, 0, n);
    http_reply(fd, "200 OK", "application/octet-stream", c, sizeof *c * (size_t)got);
    free(c);
}

/* ------------------------------------------------------------------------------------------------ what the page sends
 * short text frames:  D <deck>        a finger/mouse went down on a waveform
 *                     M <deck> <rev>  it moved: revolutions of the platter (1 rev = 1.8 s of music at 33 rpm); + = forward in time
 *                     U <deck>        it let go
 *                     G <deck> <sec>  go to a spot on the track (the page's ZOOM view): playing = jump and play on, stopped = cue from there
 *                     Q <deck>        a pitch drag starts (remembers the pitch fader)
 *                     P <deck> <d>    the pitch moves by d (a fraction of the tempo: 0.01 = 1 %); the drag sets the pitch fader
 *                     B <deck> <name> <0|1>   a button (WEB_BUTTONS only); CUE sends 1 on press and 0 on release, like a real one
 * vinyl mode: touching holds the track and moving scratches it; CDJ mode: moving nudges the tempo (jog), as on the wheel pages */
typedef struct { int scr[RX_DECKS]; double pr[RX_DECKS]; } web_conn_t;

static int deck_of(const char *s) { return (s[0] >= '1' && s[0] <= '4') ? s[0] - '1' : -1; }
static void web_cmd(rx_web_t *w, web_conn_t *cn, char *m) {
    if (!w->eng || !m[0] || m[1] != ' ') return;
    int d = deck_of(m + 2); if (d < 0) return;
    rx_deck_t *dk = rx_deck(w->eng, d);
    if (!dk) return;
    switch (m[0]) {
    case 'D':
        if (!cn->scr[d] && web_ctl(web_ids[d][0]) > 0.5) { deck_scratch_enable(dk, 1, SCRATCH_RPM, SCRATCH_ALPHA, SCRATCH_BETA); cn->scr[d] = 1; }
        break;
    case 'M': {
        if (m[3] != ' ') return;
        double rev = atof(m + 4);
        if (rev != rev || rev > 2.0 || rev < -2.0) return;
        if (cn->scr[d]) deck_scratch_tick(dk, rev * SCRATCH_TICKS_PER_REV);
        else deck_jog(dk, rev * 2.0 * M_PI * 2.0);               /* as touch.c: a turn of the wheel, jog scale */
        break;
    }
    case 'Q': {
        char g[24]; snprintf(g, sizeof g, "[Channel%d]", d + 1);
        ctl_id r = ctl_lookup(g, "rate");
        cn->pr[d] = r >= 0 ? ctl_get(r) : 0;
        break;
    }
    case 'P': {
        if (m[3] != ' ') return;
        double dl = atof(m + 4);
        if (dl != dl || dl > 0.3 || dl < -0.3) return;
        char g[24]; snprintf(g, sizeof g, "[Channel%d]", d + 1);
        ctl_id r = ctl_lookup(g, "rate"), rr = ctl_lookup(g, "rateRange");
        if (r < 0 || rr < 0) return;
        double range = ctl_get(rr); if (range < 0.01) range = 0.1;
        { ctl_id rd = ctl_lookup(g, "rate_dir"); double dir = rd >= 0 ? ctl_get(rd) : -1; if (dir == 0) dir = -1; cn->pr[d] += dl / (range * dir); }                              /* ratio = 1 + range * rate, so a change of dl in the tempo is dl / range of the fader */
        if (cn->pr[d] > 1) cn->pr[d] = 1; if (cn->pr[d] < -1) cn->pr[d] = -1;
        ctl_post(r, cn->pr[d]);
        break;
    }
    case 'G': {
        if (m[3] != ' ') return;
        double sec = atof(m + 4);
        if (sec != sec || sec < 0 || sec > 90000) return;
        char g[24]; snprintf(g, sizeof g, "[Channel%d]", d + 1);
        ctl_id c = ctl_lookup(g, "web_seek");
        if (c >= 0) ctl_post(c, sec);
        break;
    }
    case 'U':
        if (cn->scr[d]) { deck_scratch_enable(dk, 0, 0, 0, 0); cn->scr[d] = 0; }
        break;
    case 'B': {
        if (m[3] != ' ') return;
        char *name = m + 4, *sp = strchr(name, ' '); if (!sp) return;
        *sp = 0; int val = sp[1] == '1';
        for (int k = 0; k < WEB_BUTTONS_N; k++) if (strcmp(name, WEB_BUTTONS[k]) == 0) {
            char g[24]; snprintf(g, sizeof g, "[Channel%d]", d + 1);
            ctl_id c = ctl_lookup(g, name);
            if (c >= 0) ctl_post(c, val);
            break;
        }
        break;
    }
    }
}

/* the frames in buf[0..*bn): handles every complete one; -1 when the page closed */
static int ws_input(rx_web_t *w, web_conn_t *cn, uint8_t *buf, int *bn) {
    while (*bn >= 2) {
        int op = buf[0] & 15, masked = buf[1] >> 7, hl = 2;
        size_t len = buf[1] & 127;
        if (len == 126) { if (*bn < 4) break; len = (size_t)buf[2] << 8 | buf[3]; hl = 4; }
        else if (len == 127) return -1;
        if (masked) hl += 4;
        if (len > 120) return -1;
        if (*bn < hl + (int)len) { if (*bn >= (int)sizeof(uint8_t) * 500) *bn = 0; break; }
        if (op == 8) return -1;
        if (op == 1) {
            char msg[128];
            for (size_t i = 0; i < len; i++) msg[i] = (char)(buf[hl + i] ^ (masked ? buf[hl - 4 + (i & 3)] : 0));
            msg[len] = 0;
            web_cmd(w, cn, msg);
        }
        memmove(buf, buf + hl + len, (size_t)(*bn - hl - (int)len));
        *bn -= hl + (int)len;
    }
    return 0;
}

static void serve_ws(rx_web_t *w, int fd, const char *key) {
    char acc[40]; rx_web_accept_key(key, acc);
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (send_all(fd, h, (size_t)n) < 0) return;
    struct timeval tv = {0, 300000}; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    web_lookup_ids();
    rx_view_t *v = malloc(sizeof *v); char *js = malloc(16384);
    if (!v || !js) { free(v); free(js); return; }
    web_conn_t cn; memset(&cn, 0, sizeof cn);
    uint8_t buf[512]; int bn = 0;
    double last_push = -1000;
    for (;;) {
        if (atomic_load(&w->stop)) break;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (now - last_push >= PUSH_MS) {
            if (atomic_load(&w->have)) {
                pthread_mutex_lock(&w->lock); memcpy(v, &w->view, sizeof *v); pthread_mutex_unlock(&w->lock);
                int len = build_json(v, js, 16384);
                if (ws_send_text(fd, js, len) < 0) break;
            }
            last_push = now;
        }
        int wait = (int)(PUSH_MS - (now - last_push)); if (wait < 1) wait = 1; if (wait > PUSH_MS) wait = PUSH_MS;
        struct pollfd p = {fd, POLLIN, 0};
        int pr = poll(&p, 1, wait);                 /* wakes the moment the page sends something: no wait for the next push */
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0) {
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            ssize_t r = recv(fd, buf + bn, sizeof buf - (size_t)bn, 0);
            if (r <= 0 && errno != EINTR && errno != EAGAIN) break;
            if (r > 0) bn += (int)r;
            if (ws_input(w, &cn, buf, &bn) < 0) break;
        }
    }
    for (int d = 0; d < RX_DECKS; d++) if (cn.scr[d]) deck_scratch_enable(rx_deck(w->eng, d), 0, 0, 0, 0);   /* the page went away mid-touch */
    free(v); free(js);
}


/* ------------------------------------------------------------------------------------------------ the map builder's API
 *   GET  /map             the builder page (web/mapbuilder.html)
 *   GET  /api/controls    every control the plugin has: [{"g":"[Channel1]","n":"play","k":"trigger","lo":0,"hi":1}, ...]
 *   GET  /api/midi        the last note/CC/bend the plugin received: {"seq":n,"st":176,"d1":22,"d2":127}
 *   GET  /api/maps        the maps in the Mappings folder and what is loaded now (with the controls this plugin does not have)
 *   POST /api/savemap?name=X[&load=1]   body = the .midi.xml; written to Mappings (needs the header X-Rx: 1, so another web page cannot call it)
 *   POST /api/loadmap?name=X            loads a map from Mappings (same header)
 *   POST /api/nomap                     unloads the map */
static int url_arg(const char *query, const char *key, char *out, size_t cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *q = query; q && *q; ) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1; size_t o = 0;
            while (*v && *v != '&' && o + 1 < cap) {
                if (*v == '%' && v[1] && v[2]) { char h[3] = {v[1], v[2], 0}; out[o++] = (char)strtol(h, NULL, 16); v += 3; }
                else if (*v == '+') { out[o++] = ' '; v++; }
                else out[o++] = *v++;
            }
            out[o] = 0;
            return 1;
        }
        q = strchr(q, '&'); if (q) q++;
    }
    return 0;
}

static void api_controls(int fd) {
    int n = ctl_count();
    size_t cap = (size_t)n * 190 + 64, o = 0;
    char *b = malloc(cap);
    if (!b) { http_reply(fd, "500 Error", "text/plain", "memory", 6); return; }
    static const char *const KIND[] = {"float", "toggle", "trigger", "int", "text"};
    o += (size_t)snprintf(b + o, cap - o, "[");
    int first = 1;
    for (int i = 0; i < n; i++) {
        const char *g = ctl_group(i), *nm = ctl_name(i);
        if (!g || !nm || !g[0] || ctl_kind(i) == CK_TEXT) continue;
        if (!strcmp(g, "[reMixXXL]") && (!strncmp(nm, "qr", 2) || !strncmp(nm, "maps_", 5) || !strncmp(nm, "browse_row", 10) || !strcmp(nm, "midi_learn") || !strcmp(nm, "midi_map") || !strcmp(nm, "mt_led") || !strncmp(nm, "overview_tap", 12))) continue;
        if (!strncmp(nm, "jog_", 4) || !strncmp(nm, "bpm_", 4) || !strcmp(nm, "cues_dirty") || !strcmp(nm, "jog_disp")) continue;
        char eg[160], en[160];
        json_str(eg, sizeof eg, g); json_str(en, sizeof en, nm);
        int k = ctl_kind(i); if (k < 0 || k > 4) k = 0;
        o += (size_t)snprintf(b + o, cap - o, "%s{\"g\":\"%s\",\"n\":\"%s\",\"k\":\"%s\",\"lo\":%g,\"hi\":%g}", first ? "" : ",", eg, en, KIND[k], ctl_min(i), ctl_max(i));
        first = 0;
        if (o + 240 > cap) break;
    }
    o += (size_t)snprintf(b + o, cap - o, "]");
    http_reply(fd, "200 OK", "application/json", b, o);
    free(b);
}

static void api_maps(rx_web_t *w, int fd) {
    rx_midi_t *m = engine_midi(w->eng);
    char *b = malloc(16384);
    if (!b) { http_reply(fd, "500 Error", "text/plain", "memory", 6); return; }
    size_t o = 0, cap = 16384;
    o += (size_t)snprintf(b + o, cap - o, "{\"maps\":[");
    int total = midi_mapping_total(m);
    for (int i = 0; i < total && o + 400 < cap - 2200; i++) {
        char nm[256], e[300];
        if (midi_mapping_name_at(m, i, nm, sizeof nm)) continue;
        json_str(e, sizeof e, nm);
        o += (size_t)snprintf(b + o, cap - o, "%s\"%s\"", i ? "," : "", e);
    }
    char file[RX_PATH], name[128], unk[2048]; int nin = 0, ns = 0, nu = 0;
    int have = midi_report(m, file, sizeof file, name, sizeof name, &nin, &ns, &nu, unk, sizeof unk, 0);
    const char *fb = strrchr(file, '/'); fb = fb ? fb + 1 : file;
    char ef[300], en[300], eu[4200];
    json_str(ef, sizeof ef, fb); json_str(en, sizeof en, name); json_str(eu, sizeof eu, unk);
    if (have > 0) o += (size_t)snprintf(b + o, cap - o, "],\"loaded\":{\"file\":\"%s\",\"name\":\"%s\",\"controls\":%d,\"script\":%d,\"unsupported\":%d,\"unsupported_list\":\"%s\"}}", ef, en, nin, ns, nu, eu);
    else o += (size_t)snprintf(b + o, cap - o, "],\"loaded\":null}");
    http_reply(fd, "200 OK", "application/json", b, o);
    free(b);
}

static void api_post(rx_web_t *w, int fd, const char *path, const char *query, const char *hdr, char *body, size_t blen) {
    if (!strcasestr(hdr, "\nX-Rx: 1")) { http_reply(fd, "403 Forbidden", "text/plain", "forbidden", 9); return; }
    rx_midi_t *m = engine_midi(w->eng);
    if (!m) { http_reply(fd, "503 Unavailable", "text/plain", "no MIDI engine", 14); return; }
    char name[160];
    url_arg(query, "name", name, sizeof name);
    if (!strcmp(path, "/api/savemap")) {
        char saved[RX_PATH + 130];
        int r = midi_save_map(m, name, body, blen, saved, sizeof saved);
        if (r) { char msg[64]; int l = snprintf(msg, sizeof msg, "{\"ok\":false,\"error\":%d}", r); http_reply(fd, "400 Bad Request", "application/json", msg, (size_t)l); return; }
        engine_map_rescan(w->eng);
        char tmp[8]; if (url_arg(query, "load", tmp, sizeof tmp) && tmp[0] == '1') engine_map_pick(w->eng, saved);
        char es[RX_PATH + 160], out[RX_PATH + 200]; json_str(es, sizeof es, saved);
        int l = snprintf(out, sizeof out, "{\"ok\":true,\"saved\":\"%s\"}", es);
        http_reply(fd, "200 OK", "application/json", out, (size_t)l);
    } else if (!strcmp(path, "/api/loadmap")) {
        char p[RX_PATH + 40], clean[160]; int k = 0;
        for (const char *c = name; *c && k < 150; c++) if (*c != '/' && *c != '\\') clean[k++] = *c;   /* a name in Mappings, no path */
        clean[k] = 0;
        if (!clean[0] || strstr(clean, "..")) { http_reply(fd, "400 Bad Request", "text/plain", "name", 4); return; }
        char dir[RX_PATH]; int has = 0;
        char nm[300];
        for (int i = 0, t = midi_mapping_total(m); i < t && !has; i++) {
            if (midi_mapping_name_at(m, i, nm, sizeof nm) == 0 && (!strcasecmp(nm, clean))) { if (!midi_mapping_path_at(m, i, dir, sizeof dir)) { snprintf(p, sizeof p, "%s", dir); has = 1; } }
        }
        if (!has) { http_reply(fd, "404 Not Found", "text/plain", "no such map", 11); return; }
        engine_map_pick(w->eng, p);
        http_reply(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
    } else if (!strcmp(path, "/api/nomap")) {
        engine_map_none(w->eng);
        http_reply(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
    } else http_reply(fd, "404 Not Found", "text/plain", "not found", 9);
}

static void *conn_main(void *arg) {
    conn_t *c = arg; rx_web_t *w = c->w; int fd = c->fd; free(c);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char req[4096]; int n = 0;
    char *hend = NULL;
    while (n < (int)sizeof req - 1) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - (size_t)n, 0);
        if (r <= 0) break;
        n += (int)r; req[n] = 0;
        if ((hend = strstr(req, "\r\n\r\n"))) break;
    }
    req[n] = 0;
    if (strncmp(req, "POST ", 5) == 0 && hend) {
        char *path = req + 5, *sp = strchr(path, ' ');
        if (sp) *sp = 0;
        char *query = strchr(path, '?'); if (query) *query++ = 0; else query = path + strlen(path);
        long clen = 0;
        const char *cl = strcasestr(sp ? sp + 1 : req, "Content-Length:");
        if (cl) clen = atol(cl + 15);
        if (clen < 0 || clen > 262144) { http_reply(fd, "413 Too Large", "text/plain", "too large", 9); goto done; }
        char *body = malloc((size_t)clen + 1);
        if (!body) { http_reply(fd, "500 Error", "text/plain", "memory", 6); goto done; }
        long have = n - (int)(hend + 4 - req);
        if (have > clen) have = clen;
        if (have > 0) memcpy(body, hend + 4, (size_t)have);
        while (have < clen) {
            ssize_t r = recv(fd, body + have, (size_t)(clen - have), 0);
            if (r <= 0) break;
            have += r;
        }
        body[have] = 0;
        if (have < clen) http_reply(fd, "400 Bad Request", "text/plain", "short body", 10);
        else api_post(w, fd, path, query, sp ? sp + 1 : req, body, (size_t)have);
        free(body);
        goto done;
    }
    if (strncmp(req, "GET ", 4) == 0) {
        char *path = req + 4, *sp = strchr(path, ' ');
        if (sp) *sp = 0;
        char *q = strchr(path, '?'); if (q) *q = 0;
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) http_reply(fd, "200 OK", "text/html; charset=utf-8", RX_WEB_PAGE, strlen(RX_WEB_PAGE));
        else if (strcmp(path, "/map") == 0 || strcmp(path, "/map/") == 0) http_reply(fd, "200 OK", "text/html; charset=utf-8", RX_MAP_PAGE, strlen(RX_MAP_PAGE));
        else if (strcmp(path, "/api/controls") == 0) api_controls(fd);
        else if (strcmp(path, "/api/maps") == 0) api_maps(w, fd);
        else if (strcmp(path, "/api/midi") == 0) {
            int st = 0, d1 = 0, d2 = 0, seq = midi_last_msg(engine_midi(w->eng), &st, &d1, &d2);
            char out[96]; int l = snprintf(out, sizeof out, "{\"seq\":%d,\"st\":%d,\"d1\":%d,\"d2\":%d}", seq, st, d1, d2);
            http_reply(fd, "200 OK", "application/json", out, (size_t)l);
        }
        else if ((path[1] == 'w' || path[1] == 'o') && path[2] == '/' && path[3] >= '0' && path[3] <= '3' && path[4] == 0) serve_wave(fd, path[3] - '0', path[1] == 'o');
        else if (strcmp(path, "/ws") == 0) {
            const char *k = strcasestr(sp ? sp + 1 : req, "Sec-WebSocket-Key:");
            char key[64] = {0};
            if (k) { k += 18; while (*k == ' ') k++; for (int i = 0; i < 60 && k[i] && k[i] != '\r' && k[i] != '\n'; i++) key[i] = k[i]; }
            if (key[0]) serve_ws(w, fd, key); else http_reply(fd, "400 Bad Request", "text/plain", "websocket only", 14);
        } else http_reply(fd, "404 Not Found", "text/plain", "not found", 9);
    }
done:
    close(fd);
    atomic_fetch_sub(&w->clients, 1);
    return NULL;
}

static void web_refresh(rx_web_t *w);
static void *accept_main(void *arg) {
    rx_web_t *w = arg;
    int rt = 0;
    while (!atomic_load(&w->stop)) {
        if (rt-- <= 0) { rt = 8; web_refresh(w); }      /* every ~2 s (the poll below waits 250 ms) */
        struct pollfd p = {w->lsock, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        int fd = accept(w->lsock, NULL, NULL);
        if (fd < 0) continue;
        if (atomic_load(&w->clients) >= WEB_MAX) { close(fd); continue; }
        conn_t *c = malloc(sizeof *c);
        if (!c) { close(fd); continue; }
        c->w = w; c->fd = fd;
        atomic_fetch_add(&w->clients, 1);
        pthread_t th;
        if (pthread_create(&th, NULL, conn_main, c) != 0) { atomic_fetch_sub(&w->clients, 1); close(fd); free(c); continue; }
        pthread_detach(th);
    }
    return NULL;
}

/* the Force's address on the network, for the on-screen line: the first non-loopback IPv4 */
static void find_ip(char *out, size_t n) {
    snprintf(out, n, "<force ip>");
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
        break;
    }
    freeifaddrs(ifa);
}

/* the address and its QR code, again when the network address changed (the accept thread, about every 2 s) */
static void web_refresh(rx_web_t *w) {
    char ip[64]; find_ip(ip, sizeof ip);
    if (!strcmp(ip, w->ip)) return;
    char url[96]; snprintf(url, sizeof url, "http://%s:%d/", ip, w->port);
    unsigned char mods[41 * 41], qb[RX_QR_N * RX_QR_BYTES]; int n = 0, ok = 0;
    memset(qb, 0, sizeof qb);
    if (ip[0] != '<' && qr_encode(url, RX_QR_VER, mods, &n) == 0 && n == RX_QR_N) {
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) if (mods[y * n + x]) qb[y * RX_QR_BYTES + x / 8] |= (unsigned char)(0x80 >> (x & 7));
        ok = 1;
    }
    pthread_mutex_lock(&w->lock);
    snprintf(w->ip, sizeof w->ip, "%s", ip);
    snprintf(w->url, sizeof w->url, "http://%s:%d", ip, w->port);
    memcpy(w->qr, qb, sizeof qb); w->qr_ok = ok;
    pthread_mutex_unlock(&w->lock);
    rx_log("web: companion page at %s%s", url, ok ? " (QR ready)" : " (no network yet)");
}

/* the QR bytes for the skin (RX_QR_N rows of RX_QR_BYTES); 0 when there is no address yet or the lock is busy (audio thread: never waits) */
int web_qr(rx_web_t *w, unsigned char *out) {
    if (!w || pthread_mutex_trylock(&w->lock) != 0) return 0;
    int ok = w->qr_ok;
    if (ok) memcpy(out, w->qr, sizeof w->qr); else memset(out, 0, sizeof w->qr);
    pthread_mutex_unlock(&w->lock);
    return ok ? 1 : -1;      /* -1: no network: the caller blanks the code */
}

rx_web_t *web_start(rx_engine_t *eng) {
    signal(SIGPIPE, SIG_IGN);
    rx_web_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    pthread_mutex_init(&w->lock, NULL);
    w->eng = eng;
    w->lsock = -1;
    for (int t = 0; t < WEB_PORT_TRIES && w->lsock < 0; t++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) break;
        int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)(WEB_PORT_FIRST + t));
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0) { w->lsock = s; w->port = WEB_PORT_FIRST + t; }
        else { rx_log("web: port %d not available (%s)", WEB_PORT_FIRST + t, strerror(errno)); close(s); }
    }
    if (w->lsock < 0) { rx_log("web: no port free, companion page off"); free(w); return NULL; }
    char ip[64]; find_ip(ip, sizeof ip);
    snprintf(w->url, sizeof w->url, "http://%s:%d", ip, w->port);
    rx_log("web: companion page at %s", w->url);
    if (pthread_create(&w->accept_th, NULL, accept_main, w) != 0) { close(w->lsock); free(w); return NULL; }
    return w;
}

void web_stop(rx_web_t *w) {
    if (!w) return;
    atomic_store(&w->stop, 1);
    pthread_join(w->accept_th, NULL);
    close(w->lsock);
    for (int i = 0; i < 40 && atomic_load(&w->clients) > 0; i++) { struct timespec sl = {0, 50000000}; nanosleep(&sl, NULL); }
    if (atomic_load(&w->clients) == 0) free(w);     /* a connection thread still running keeps the struct (a leak beats a crash) */
}

void web_publish(rx_web_t *w, const rx_view_t *v) {
    if (!w) return;
    if (pthread_mutex_trylock(&w->lock) != 0) return;      /* audio thread: never wait */
    memcpy(&w->view, v, sizeof *v);
    pthread_mutex_unlock(&w->lock);
    atomic_store(&w->have, 1);
}

const char *web_url(rx_web_t *w) { return w ? w->url : ""; }
