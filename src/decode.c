/* reMixXXL -- decode.c: a whole audio file to interleaved int16 stereo at RX_SR (44100 Hz).
 *
 * Formats (by extension, decode_is_audio_file): wav (RIFF/RIFX/RF64/W64, dr_wav), flac (dr_flac), mp3 (minimp3),
 * aif/aiff (listed as audio so the browser shows them, but decode returns -ENOTSUP: dr_wav's AIFF reader does
 * not do every variant and we do not want a half-working deck -- see the comment at decode_file).
 *
 * Memory (the device has little): the file is streamed, DEC_CHUNK frames at a time, through the resampler straight
 * into the int16 output buffer, which grows by realloc in DEC_GROW-frame steps (or is sized once when the decoder
 * knows the frame count).  Nothing is decoded to float as a whole.  minimp3's mp3dec_ex mmaps the compressed file
 * (read-only pages, dropped by the kernel when memory is short), it does not decode it up front.
 *
 * Resampling (any input rate -> 44100): a polyphase windowed-sinc FIR, RS_TAPS taps, RS_PHASES phases with linear
 * interpolation between neighbouring phases (so arbitrary ratios like 48000/44100 or 22050/44100 work with the one
 * table).  The cutoff is 0.45 x min(in, out) rate with a Kaiser window (beta 6), so downsampling from 48/96 kHz is
 * properly band-limited; it is not merely a 4-point cubic.  The table is built once (static, lazily) and shared.
 *
 * Tags: wav INFO INAM/IART (dr_wav metadata), flac Vorbis comments TITLE/ARTIST (dr_flac metadata callback),
 * mp3 ID3v2.3/2.4 TIT2/TPE1 (a small parser here, ISO-8859-1/UTF-8/UTF-16 BMP) with ID3v1 as a fallback.  When no
 * title tag is found the file name stem is the title and the artist stays "".
 *
 * Progress: progress(user, 0..100) is called about every DEC_CHUNK frames (percent of the known frame count, or of
 * the compressed bytes for an mp3 without a VBR tag). */

#include "remix.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"
#include "minimp3_ex.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEC_CHUNK 4096                       /* input frames per decoder call */
#define DEC_GROW (1 << 20)                   /* output growth step, frames (4 MB of int16 stereo) */

/* ------------------------------------------------------------------------------------------------ growing output */
typedef struct {
    int16_t *lr;
    int64_t frames, cap, max;
    int full;                                /* hit max_frames: stop decoding */
} dec_out_t;

static int out_reserve(dec_out_t *o, int64_t need) {
    if (need <= o->cap) return 0;
    int64_t cap = o->cap ? o->cap : DEC_GROW;
    while (cap < need) cap += DEC_GROW;
    if (cap > o->max) cap = o->max;
    if (cap < need) return -1;
    int16_t *p = realloc(o->lr, (size_t)cap * 2 * sizeof(int16_t));
    if (!p) return -1;
    o->lr = p; o->cap = cap;
    return 0;
}

/* append n stereo frames (clips to max_frames; sets full) */
static int out_push(dec_out_t *o, const int16_t *lr, int64_t n) {
    if (o->full) return 0;
    if (o->frames + n > o->max) { n = o->max - o->frames; o->full = 1; }
    if (n <= 0) return 0;
    if (out_reserve(o, o->frames + n) != 0) return -ENOMEM;
    memcpy(o->lr + o->frames * 2, lr, (size_t)n * 2 * sizeof(int16_t));
    o->frames += n;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ resampler */
#define RS_TAPS 32
#define RS_PHASES 256
#define RS_HALF (RS_TAPS / 2)

static float g_rs_table[RS_PHASES + 1][RS_TAPS];   /* [phase][tap], phase p = fractional offset p/RS_PHASES */
static int g_rs_table_built;
static double g_rs_table_cutoff;                   /* the normalised cutoff the table was built for */

static double bessel_i0(double x) {              /* modified Bessel I0 for the Kaiser window */
    double sum = 1, term = 1, k = 1;
    for (; k < 50; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; if (term < 1e-12 * sum) break; }
    return sum;
}

/* cutoff: fraction of the input sample rate (0.5 = Nyquist of the input) */
static void rs_build_table(double cutoff) {
    const double beta = 6.0;
    const double i0b = bessel_i0(beta);
    for (int p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES;
        double sum = 0;
        for (int t = 0; t < RS_TAPS; t++) {
            /* tap t sits at input offset (t - RS_HALF + 1) relative to the base sample; the output point is at +frac */
            double x = (double)(t - RS_HALF + 1) - frac;
            double sinc = (x == 0.0) ? 1.0 : sin(2 * M_PI * cutoff * x) / (2 * M_PI * cutoff * x);
            double w = x / RS_HALF; w = 1 - w * w; if (w < 0) w = 0;
            double kaiser = bessel_i0(beta * sqrt(w)) / i0b;
            double v = 2 * cutoff * sinc * kaiser;
            g_rs_table[p][t] = (float)v; sum += v;
        }
        for (int t = 0; t < RS_TAPS; t++) g_rs_table[p][t] = (float)(g_rs_table[p][t] / sum);   /* unity DC gain */
    }
    g_rs_table_built = 1;
    g_rs_table_cutoff = cutoff;
}

typedef struct {
    int in_rate, out_rate;
    double step;                             /* input frames per output frame */
    double pos;                              /* position of the next output frame in input-frame units, relative
                                                to hist[0] */
    float hist[(RS_TAPS + DEC_CHUNK) * 2];   /* RS_TAPS frames of history followed by the current chunk */
    int hist_frames;                         /* valid frames in hist */
    int16_t out[DEC_CHUNK * 4];              /* enough for a 4:1 upsample of a chunk */
} rs_t;

static void rs_init(rs_t *r, int in_rate, int out_rate) {
    memset(r, 0, sizeof *r);
    r->in_rate = in_rate; r->out_rate = out_rate;
    r->step = (double)in_rate / out_rate;
    r->pos = RS_HALF;                        /* the first output sample lines up with the first input sample */
    r->hist_frames = RS_TAPS;                /* leading zeros: the filter's pre-history */
    double lo = in_rate < out_rate ? in_rate : out_rate;
    double cutoff = 0.45 * lo / in_rate;
    if (!g_rs_table_built || g_rs_table_cutoff != cutoff) rs_build_table(cutoff);
}

static inline int16_t clip16(float v) {
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)lrintf(v);
}

/* feed n frames (interleaved float stereo, int16 range), get output frames into r->out; returns count.
 * n <= DEC_CHUNK.  pass n = 0 at the end to flush (the last RS_HALF frames of history). */
static int rs_process(rs_t *r, const float *lr, int n, int flush) {
    if (n > 0) memcpy(r->hist + r->hist_frames * 2, lr, (size_t)n * 2 * sizeof(float));
    int avail = r->hist_frames + n;
    if (flush) {                             /* pad with silence so the tail gets through the filter */
        memset(r->hist + avail * 2, 0, (size_t)RS_HALF * 2 * sizeof(float));
        avail += RS_HALF;
    }
    int produced = 0;
    /* an output frame at position pos needs input frames [floor(pos)-RS_HALF+1 .. floor(pos)+RS_HALF] */
    while (floor(r->pos) + RS_HALF < avail && produced < DEC_CHUNK * 4) {
        int base = (int)floor(r->pos);
        double frac = r->pos - base;
        double pf = frac * RS_PHASES;
        int p0 = (int)pf; float pw = (float)(pf - p0);
        const float *t0 = g_rs_table[p0], *t1 = g_rs_table[p0 + 1];
        const float *in = r->hist + (base - RS_HALF + 1) * 2;
        float sl = 0, sr = 0;
        for (int t = 0; t < RS_TAPS; t++) {
            float c = t0[t] + (t1[t] - t0[t]) * pw;
            sl += c * in[t * 2];
            sr += c * in[t * 2 + 1];
        }
        r->out[produced * 2] = clip16(sl);
        r->out[produced * 2 + 1] = clip16(sr);
        produced++;
        r->pos += r->step;
    }
    /* keep the last RS_TAPS frames as history, slide the position with them */
    int keep = RS_TAPS;
    int drop = avail - keep;
    if (drop > 0) {
        memmove(r->hist, r->hist + drop * 2, (size_t)keep * 2 * sizeof(float));
        r->hist_frames = keep;
        r->pos -= drop;
    } else {
        r->hist_frames = avail;
    }
    return produced;
}

/* ------------------------------------------------------------------------------------------------ the sink */
/* Everything a decoder produces goes through here: int16 interleaved, 1 or 2 channels, in_rate; out is 44100 stereo. */
typedef struct {
    dec_out_t out;
    rs_t *rs;                                /* NULL when in_rate == RX_SR (straight copy) */
    int channels;
    float fbuf[DEC_CHUNK * 2];
    int16_t sbuf[DEC_CHUNK * 2];
    void (*progress)(void *, int); void *user;
    int last_pct;
} sink_t;

static int sink_init(sink_t *s, int channels, int in_rate, int64_t known_frames, int max_frames,
                     void (*progress)(void *, int), void *user) {
    memset(s, 0, sizeof *s);
    if (channels < 1 || in_rate < 8000 || in_rate > 384000) return -EINVAL;
    s->channels = channels;
    s->out.max = max_frames;
    s->progress = progress; s->user = user; s->last_pct = -1;
    if (in_rate != RX_SR) {
        s->rs = malloc(sizeof *s->rs);
        if (!s->rs) return -ENOMEM;
        rs_init(s->rs, in_rate, RX_SR);
    }
    if (known_frames > 0) {
        int64_t est = (int64_t)((double)known_frames * RX_SR / in_rate) + RS_TAPS;
        if (est > max_frames) est = max_frames;
        if (out_reserve(&s->out, est) != 0) return -ENOMEM;
    }
    return 0;
}

static void sink_progress(sink_t *s, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (s->progress && pct != s->last_pct) { s->last_pct = pct; s->progress(s->user, pct); }
}

/* n frames of int16, s->channels interleaved (n <= DEC_CHUNK) */
static int sink_push(sink_t *s, const int16_t *in, int n) {
    if (n <= 0 || s->out.full) return 0;
    /* to stereo: mono duplicated, >2 channels: the first two (L, R) */
    const int16_t *st = in;
    if (s->channels != 2) {
        for (int i = 0; i < n; i++) {
            int16_t l = in[i * s->channels], r = s->channels > 1 ? in[i * s->channels + 1] : l;
            s->sbuf[i * 2] = l; s->sbuf[i * 2 + 1] = r;
        }
        st = s->sbuf;
    }
    if (!s->rs) return out_push(&s->out, st, n);
    for (int i = 0; i < n * 2; i++) s->fbuf[i] = (float)st[i];
    int got = rs_process(s->rs, s->fbuf, n, 0);
    return out_push(&s->out, s->rs->out, got);
}

static int sink_finish(sink_t *s) {
    if (s->rs && !s->out.full) {
        int got = rs_process(s->rs, NULL, 0, 1);
        int rc = out_push(&s->out, s->rs->out, got);
        if (rc) return rc;
    }
    return 0;
}

static void sink_free(sink_t *s) { free(s->rs); s->rs = NULL; }

/* ------------------------------------------------------------------------------------------------ names and tags */
static const char *file_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    const char *slash = strrchr(name, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static void title_from_stem(const char *path, char *title, int n) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= n) len = n - 1;
    memcpy(title, base, (size_t)len); title[len] = 0;
}

static void copy_tag(char *dst, int n, const char *src, int len) {
    if (len >= n) len = n - 1;
    if (len < 0) len = 0;
    memcpy(dst, src, (size_t)len); dst[len] = 0;
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == 0)) dst[--len] = 0;
}

int decode_is_audio_file(const char *name) {
    const char *e = file_ext(name);
    return !strcasecmp(e, "wav") || !strcasecmp(e, "flac") || !strcasecmp(e, "mp3") ||
           !strcasecmp(e, "aif") || !strcasecmp(e, "aiff");
}

/* ID3v2 text frame to UTF-8-ish ASCII (encoding 0 ISO-8859-1 -> UTF-8, 1/2 UTF-16 -> BMP chars as UTF-8, 3 UTF-8) */
static void id3_text(const uint8_t *d, int len, char *out, int n) {
    if (len < 1) { out[0] = 0; return; }
    int enc = d[0]; d++; len--;
    int o = 0;
    if (enc == 0) {
        for (int i = 0; i < len && o < n - 3; i++) {
            uint8_t c = d[i];
            if (c == 0) break;
            if (c < 0x80) out[o++] = (char)c;
            else { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        }
    } else if (enc == 3) {
        for (int i = 0; i < len && o < n - 1; i++) { if (!d[i]) break; out[o++] = (char)d[i]; }
    } else {                                 /* UTF-16 with BOM (1) or BE (2) */
        int be = (enc == 2);
        int i = 0;
        if (enc == 1 && len >= 2) { if (d[0] == 0xFF && d[1] == 0xFE) be = 0; else if (d[0] == 0xFE && d[1] == 0xFF) be = 1; i = 2; }
        for (; i + 1 < len && o < n - 4; i += 2) {
            unsigned c = be ? (d[i] << 8 | d[i + 1]) : (d[i + 1] << 8 | d[i]);
            if (c == 0) break;
            if (c >= 0xD800 && c <= 0xDFFF) continue;        /* surrogates: skip (rare in titles) */
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
            else { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    out[o] = 0;
}

static uint32_t id3_syncsafe(const uint8_t *p) { return ((p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

/* reads the ID3v2 header at the file start (TIT2/TPE1) and the ID3v1 trailer; fills what it finds */
static void mp3_tags(const char *path, char *title, int tn, char *artist, int an) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t h[10];
    if (fread(h, 1, 10, f) == 10 && memcmp(h, "ID3", 3) == 0 && h[3] <= 4) {
        int ver = h[3];
        uint32_t size = id3_syncsafe(h + 6);
        if (size > (1u << 22)) size = 1u << 22;              /* 4 MB of tags is plenty (cover art lives there) */
        uint8_t *tag = malloc(size);
        if (tag && fread(tag, 1, size, f) == size) {
            uint32_t p = 0;
            if (h[5] & 0x40) {                                /* extended header */
                if (size >= 4) p = ver == 4 ? id3_syncsafe(tag) : (uint32_t)(tag[0] << 24 | tag[1] << 16 | tag[2] << 8 | tag[3]) + 4;
            }
            while (p + 10 <= size) {
                if (tag[p] == 0) break;                       /* padding */
                uint32_t fsz = ver == 4 ? id3_syncsafe(tag + p + 4)
                                        : (uint32_t)(tag[p + 4] << 24 | tag[p + 5] << 16 | tag[p + 6] << 8 | tag[p + 7]);
                if (ver == 2) {                               /* v2.2: 3-byte ids and sizes */
                    fsz = (uint32_t)(tag[p + 3] << 16 | tag[p + 4] << 8 | tag[p + 5]);
                    if (p + 6 + fsz > size) break;
                    if (!memcmp(tag + p, "TT2", 3)) id3_text(tag + p + 6, (int)fsz, title, tn);
                    else if (!memcmp(tag + p, "TP1", 3)) id3_text(tag + p + 6, (int)fsz, artist, an);
                    p += 6 + fsz;
                    continue;
                }
                if (p + 10 + fsz > size) break;
                if (!memcmp(tag + p, "TIT2", 4)) id3_text(tag + p + 10, (int)fsz, title, tn);
                else if (!memcmp(tag + p, "TPE1", 4)) id3_text(tag + p + 10, (int)fsz, artist, an);
                p += 10 + fsz;
            }
        }
        free(tag);
    }
    if (!title[0] || !artist[0]) {                            /* ID3v1 trailer */
        uint8_t t[128];
        if (fseek(f, -128, SEEK_END) == 0 && fread(t, 1, 128, f) == 128 && memcmp(t, "TAG", 3) == 0) {
            if (!title[0]) copy_tag(title, tn, (const char *)t + 3, 30);
            if (!artist[0]) copy_tag(artist, an, (const char *)t + 33, 30);
        }
    }
    fclose(f);
}

typedef struct { char *title; int tn; char *artist; int an; } flac_tag_ctx_t;

static void flac_meta_cb(void *user, drflac_metadata *m) {
    flac_tag_ctx_t *c = user;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    const char *s; drflac_uint32 len;
    while ((s = drflac_next_vorbis_comment(&it, &len)) != NULL) {
        if (len > 6 && !strncasecmp(s, "TITLE=", 6)) copy_tag(c->title, c->tn, s + 6, (int)len - 6);
        else if (len > 7 && !strncasecmp(s, "ARTIST=", 7)) copy_tag(c->artist, c->an, s + 7, (int)len - 7);
    }
}

/* ------------------------------------------------------------------------------------------------ decoders */
static int decode_wav(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user) {
    drwav w;
    if (!drwav_init_file_with_metadata(&w, path, 0, NULL)) return -EINVAL;
    if (w.container == drwav_container_aiff) { drwav_uninit(&w); return -ENOTSUP; }
    for (drwav_uint32 i = 0; i < w.metadataCount; i++) {
        drwav_metadata *m = &w.pMetadata[i];
        if (m->type == drwav_metadata_type_list_info_title) copy_tag(out->title, sizeof out->title, m->data.infoText.pString, (int)m->data.infoText.stringLength);
        else if (m->type == drwav_metadata_type_list_info_artist) copy_tag(out->artist, sizeof out->artist, m->data.infoText.pString, (int)m->data.infoText.stringLength);
    }
    sink_t s;
    int rc = sink_init(&s, w.channels, (int)w.sampleRate, (int64_t)w.totalPCMFrameCount, max_frames, progress, user);
    if (rc) { drwav_uninit(&w); return rc; }
    int16_t *buf = malloc((size_t)DEC_CHUNK * w.channels * sizeof(int16_t));
    if (!buf) { sink_free(&s); drwav_uninit(&w); return -ENOMEM; }
    int64_t done = 0;
    while (!s.out.full) {
        drwav_uint64 n = drwav_read_pcm_frames_s16(&w, DEC_CHUNK, buf);
        if (n == 0) break;
        rc = sink_push(&s, buf, (int)n);
        if (rc) break;
        done += (int64_t)n;
        if (w.totalPCMFrameCount) sink_progress(&s, (int)(done * 100 / (int64_t)w.totalPCMFrameCount));
    }
    free(buf);
    drwav_uninit(&w);
    if (!rc) rc = sink_finish(&s);
    sink_free(&s);
    out->lr = s.out.lr; out->frames = s.out.frames;
    return rc;
}

static int decode_flac(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user) {
    flac_tag_ctx_t tc = { out->title, sizeof out->title, out->artist, sizeof out->artist };
    drflac *f = drflac_open_file_with_metadata(path, flac_meta_cb, &tc, NULL);
    if (!f) return -EINVAL;
    sink_t s;
    int rc = sink_init(&s, f->channels, (int)f->sampleRate, (int64_t)f->totalPCMFrameCount, max_frames, progress, user);
    if (rc) { drflac_close(f); return rc; }
    int16_t *buf = malloc((size_t)DEC_CHUNK * f->channels * sizeof(int16_t));
    if (!buf) { sink_free(&s); drflac_close(f); return -ENOMEM; }
    int64_t done = 0;
    while (!s.out.full) {
        drflac_uint64 n = drflac_read_pcm_frames_s16(f, DEC_CHUNK, buf);
        if (n == 0) break;
        rc = sink_push(&s, buf, (int)n);
        if (rc) break;
        done += (int64_t)n;
        if (f->totalPCMFrameCount) sink_progress(&s, (int)(done * 100 / (int64_t)f->totalPCMFrameCount));
    }
    free(buf);
    drflac_close(f);
    if (!rc) rc = sink_finish(&s);
    sink_free(&s);
    out->lr = s.out.lr; out->frames = s.out.frames;
    return rc;
}

static int decode_mp3(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user) {
    mp3_tags(path, out->title, sizeof out->title, out->artist, sizeof out->artist);
    mp3dec_ex_t *dec = malloc(sizeof *dec);    /* ~ 7 KB + the frame buffer: not on the stack of a small thread */
    if (!dec) return -ENOMEM;
    /* MP3D_DO_NOT_SCAN: no full header walk up front (a 15-minute file is ~35k frames); we stream anyway. With a
     * Xing/Info tag the encoder delay and padding are still trimmed. */
    int r = mp3dec_ex_open(dec, path, MP3D_DO_NOT_SCAN);
    if (r) { free(dec); return r == MP3D_E_IOERROR ? -EIO : -EINVAL; }
    if (dec->info.hz == 0 || dec->info.channels == 0) { mp3dec_ex_close(dec); free(dec); return -EINVAL; }
    int ch = dec->info.channels;
    int64_t known = dec->detected_samples ? (int64_t)(dec->detected_samples / (uint64_t)ch) : 0;
    sink_t s;
    int rc = sink_init(&s, ch, dec->info.hz, known, max_frames, progress, user);
    if (rc) { mp3dec_ex_close(dec); free(dec); return rc; }
    int16_t *buf = malloc((size_t)DEC_CHUNK * ch * sizeof(int16_t));
    if (!buf) { sink_free(&s); mp3dec_ex_close(dec); free(dec); return -ENOMEM; }
    int64_t done = 0;
    while (!s.out.full) {
        size_t n = mp3dec_ex_read(dec, buf, (size_t)DEC_CHUNK * ch);
        if (n == 0) {
            if (dec->last_error && dec->last_error != MP3D_E_DECODE && done == 0) rc = -EINVAL;
            break;
        }
        int frames = (int)(n / (size_t)ch);
        rc = sink_push(&s, buf, frames);
        if (rc) break;
        done += frames;
        if (known) sink_progress(&s, (int)(done * 100 / known));
        else if (dec->file.size) sink_progress(&s, (int)((dec->offset * 100) / dec->file.size));
    }
    free(buf);
    mp3dec_ex_close(dec);
    free(dec);
    if (!rc) rc = sink_finish(&s);
    sink_free(&s);
    out->lr = s.out.lr; out->frames = s.out.frames;
    return rc;
}

/* ------------------------------------------------------------------------------------------------ API */
int decode_file(const char *path, rx_audio_t *out, int max_frames, void (*progress)(void *, int), void *user) {
    if (!path || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    if (max_frames <= 0) max_frames = RX_MAX_TRACK_SEC * RX_SR;
    const char *e = file_ext(path);
    int rc;
    if (!strcasecmp(e, "wav")) rc = decode_wav(path, out, max_frames, progress, user);
    else if (!strcasecmp(e, "flac")) rc = decode_flac(path, out, max_frames, progress, user);
    else if (!strcasecmp(e, "mp3")) rc = decode_mp3(path, out, max_frames, progress, user);
    else if (!strcasecmp(e, "aif") || !strcasecmp(e, "aiff")) rc = -ENOTSUP;   /* see the header comment */
    else rc = -ENOTSUP;
    if (rc < 0) { decode_free(out); return rc; }
    if (out->frames <= 0) { decode_free(out); return -EINVAL; }
    if (!out->title[0]) title_from_stem(path, out->title, sizeof out->title);
    if (progress) progress(user, 100);
    return (int)out->frames;
}

void decode_free(rx_audio_t *a) {
    if (!a) return;
    free(a->lr);
    a->lr = NULL;
    a->frames = 0;
}
