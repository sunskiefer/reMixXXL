/* reMixXXL -- the plugin's engine: what mpc-vst-plugins' wrapper drives (engine.h), tying the parts together.
 *
 *   create()      the plugin folder, the log, the control table, four decks, the mixer, the effects, the browse list,
 *                 the live screen (overlay.c), the touch reader (touch.c) and MIDI (midi.c); a worker thread for loads
 *   set_param()   a parameter from MPC (a screen tap, a Q-Link, project restore): mapped to its Mixxx control
 *   get_param()   a parameter's value or text for MPC, "dirty" (what the engine changed by itself), "state"
 *   render()      one block: queued control writes applied, the decks played, the mixer and effects, int16 out, the
 *                 view for the screen published
 *   midi()        raw MIDI from the track's input -> midi.c
 *
 * Parameters are listed in tools/gen_params.py (params.json / remix_params.h): each one names the control it drives,
 * so this file converts ranges and nothing else. Knobs are 0..1 for MPC; the table below says how each maps to its
 * control's range (rate 0..1 -> -1..1, gain 0..1 -> 0..4 with 0.5 = unity, and so on).
 *
 * Threads: MPC calls set_param/get_param from its screen thread and render/midi from the audio thread, never two at
 * once (the wrapper's lock), so the engine itself needs no locking between those; the worker thread (loads, scans,
 * sidecar files) and the parts' own threads (screen, touch, JS) talk to the engine through ctl_post() and a few
 * atomics only. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "wrapper/engine.h"
#include "remix.h"
#include "remix_params.h"
#include "deck.h"
#include "fx_priv.h"
#include "boxes.h"
#include "version.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb/stb_image.h"

#define BROWSE_MAX 4096
#define LOADQ 16

typedef struct { char name[128]; char path[RX_PATH]; double bpm; int is_dir; } br_entry_t;

struct rx_engine {
    char plugin_dir[RX_PATH], tracks_dir[RX_PATH];
    rx_deck_t *deck[RX_DECKS];
    rx_mixer_t *mixer;
    rx_fx_t *fx;
    rx_overlay_t *ovl;
    rx_touch_t *touch;
    rx_web_t *web;
    rx_midi_t *midi;
    int owns_screen;                        /* the first instance: it has the overlay, touch and MIDI mapping threads */
    /* the parameters' controls, by index */
    ctl_id pctl[RX_NPARAMS];
    float last_reported[RX_NPARAMS];        /* what MPC was last told / told us: "dirty" reports differences */
    /* our own controls */
    ctl_id c_deck_sel, c_wheel_l, c_wheel_r, c_zoom, c_readout, c_cpu_text, c_browse_sel, c_brow, c_browse_page, c_prev, c_next,
           c_folder, c_up, c_sort, c_scroll, c_xf_mode, c_path, c_ov_tap[RX_DECKS], c_midi_map, c_midi_learn, c_midi_text,
           c_maps_row[RX_BROWSE_ROWS], c_maps_sel, c_maps_cur, c_maps_page, c_maps_prev, c_maps_next, c_maps_load, c_maps_none, c_maps_status,
           c_wheel_l_text, c_wheel_r_text, c_touch_text, c_browse_row[RX_BROWSE_ROWS], c_load[RX_DECKS], c_track_step[RX_DECKS];
    /* browse */
    pthread_mutex_t br_lock;                /* the list is rebuilt by the worker; readers copy under this lock */
    br_entry_t *br;
    int br_n, br_sel, br_page, br_sort, br_dirs;       /* br_dirs: the rows show folders (FOLDER mode) */
    char br_dir[RX_PATH];
    int br_rows_dirty;
    /* CONTROLLERS page */
    int mp_page, mp_dirty, mp_report_seen;
    /* the worker */
    pthread_t worker;
    pthread_mutex_t wlock;
    pthread_cond_t wcond;
    int stop;
    struct { int kind; int deck; char path[RX_PATH]; } q[LOADQ];   /* kind 1 load, 2 scan */
    int qn;
    /* host */
    double host_bpm, host_ppq;
    int host_playing;
    /* audio scratch */
    float dbuf[RX_DECKS][RX_BLOCK * 2];
    float mbuf[RX_BLOCK * 2];
    float cbuf[RX_BLOCK * 2];               /* the cue bus (PFL decks) of this block */
    double cpu_acc; int cpu_n; double cpu_pct;
    rx_view_t view;
    int blocks;
    int dirty_tick, diag_tick, qr_tick, mt_hold;
    ctl_id c_mt_led;                                /* the MASTER TEMPO lamp: flashes with the master deck's beat */
    ctl_id c_qr[RX_QR_N * RX_QR_BYTES];             /* the page address's QR code, a byte a control (the skin's film strips) */
    unsigned char qr_sent[RX_QR_N * RX_QR_BYTES];
    char state_cache[1 << 16];
};

static rx_engine_t *g_instance;              /* the one that owns the screen/touch/MIDI */
rx_engine_t *rx_engine_instance(void) { return g_instance; }
rx_deck_t *rx_deck(rx_engine_t *e, int i) { return (i >= 0 && i < RX_DECKS) ? e->deck[i] : NULL; }

/* ------------------------------------------------------------------------------------------------ helpers */
static void ctl_from_spec(rx_engine_t *e, int i, char *group, char *name) {
    /* "[Channel%d],play" -> group "[Channel1]", name "play"; a spec with %d takes the parameter's deck */
    const rx_param_def_t *p = &RX_PARAMS[i];
    char spec[160];
    if (strstr(p->ctl, "%d")) snprintf(spec, sizeof spec, p->ctl, p->deck); else snprintf(spec, sizeof spec, "%s", p->ctl);
    const char *c = strchr(spec, ',');
    if (!c) { group[0] = name[0] = 0; return; }
    size_t gl = (size_t)(c - spec);
    memcpy(group, spec, gl); group[gl] = 0;
    snprintf(name, 96, "%s", c + 1);
}

/* 0..1 knob <-> a control's own range */
static double knob_to_ctl(int i, double v) {
    const rx_param_def_t *p = &RX_PARAMS[i];
    const char *k = p->key;
    size_t kl = strlen(k);
    if (p->kind != RXK_KNOB) return v;
    if (!strcmp(k, "xfader")) return v * 2 - 1;
    if (kl > 5 && !strcmp(k + kl - 5, "_rate")) return 1 - v * 2;                 /* d1_rate: -1..1; the knob/fader 0 = the top = rate +1 (slower with rate_dir -1) */
    if ((kl > 5 && !strcmp(k + kl - 5, "_gain") && k[0] == 'd') || !strcmp(k, "master_gain") ||
        (kl > 3 && (!strcmp(k + kl - 3, "_hi") || !strcmp(k + kl - 4, "_mid") || !strcmp(k + kl - 4, "_low"))))
        return v < 0.5 ? v * 2 : 1 + (v - 0.5) * 6;                              /* 0..4, unity at 0.5 (Mixxx EQ/gain) */
    return v;
}
static double ctl_to_knob(int i, double c) {
    const rx_param_def_t *p = &RX_PARAMS[i];
    const char *k = p->key;
    size_t kl = strlen(k);
    if (p->kind != RXK_KNOB) return c;
    if (!strcmp(k, "xfader")) return (c + 1) * 0.5;
    if (kl > 5 && !strcmp(k + kl - 5, "_rate")) return (1 - c) * 0.5;
    if ((kl > 5 && !strcmp(k + kl - 5, "_gain") && k[0] == 'd') || !strcmp(k, "master_gain") ||
        (kl > 3 && (!strcmp(k + kl - 3, "_hi") || !strcmp(k + kl - 4, "_mid") || !strcmp(k + kl - 4, "_low"))))
        return c < 1 ? c * 0.5 : 0.5 + (c - 1) / 6;
    return c;
}

/* a parameter index by key (a small hash) */
static int find_param(const char *key) {
    static int built; static short table[2048];
    if (!built) {
        memset(table, -1, sizeof table);
        for (int i = 0; i < RX_NPARAMS; i++) {
            unsigned h = 5381; for (const char *s = RX_PARAMS[i].key; *s; s++) h = h * 33 + (unsigned char)*s;
            unsigned k = h & 2047; while (table[k] >= 0) k = (k + 1) & 2047; table[k] = (short)i;
        }
        built = 1;
    }
    unsigned h = 5381; for (const char *s = key; *s; s++) h = h * 33 + (unsigned char)*s;
    for (unsigned k = h & 2047; table[k] >= 0; k = (k + 1) & 2047) if (!strcmp(RX_PARAMS[table[k]].key, key)) return table[k];
    return -1;
}

/* ------------------------------------------------------------------------------------------------ browse */
static int br_cmp_name(const void *a, const void *b) {
    const br_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
static int br_cmp_bpm(const void *a, const void *b) {
    const br_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    if (x->bpm != y->bpm) return x->bpm < y->bpm ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* the sidecar next to a track: "<track>.remixxxl" (text: bpm, beat0, cues) */
static void sidecar_path(const char *track, char *out, int n) { snprintf(out, (size_t)n, "%s.remixxxl", track); }
static double sidecar_bpm(const char *track) {
    char p[RX_PATH]; sidecar_path(track, p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[256]; double bpm = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "bpm ", 4)) { bpm = atof(line + 4); break; }
    fclose(f);
    return bpm;
}

/* the drives: with no folder (br_dir empty) the list shows every mounted storage (the internal drive, the SD card, USB sticks), so BACK
 * from the top of one drive reaches the others */
static int vol_collect(char (*v)[RX_PATH], int max) {
    static const char *pseudo[] = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "overlay", "debugfs", "tracefs", "securityfs",
                                   "configfs", "pstore", "mqueue", "hugetlbfs", "fusectl", "autofs", "bpf", "ramfs", "rootfs", "squashfs", NULL};
    int n = 0;
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f) && n < max) {
        char src[256], mp[512], fs[64];
        if (sscanf(line, "%255s %511s %63s", src, mp, fs) != 3) continue;
        char *w = mp;   /* /proc/mounts writes a space as \040 */
        for (char *r = mp; *r; r++) { if (r[0] == '\\' && r[1] == '0' && r[2] == '4' && r[3] == '0') { *w++ = ' '; r += 3; } else *w++ = *r; }
        *w = 0;
        int skip = 0; for (int i = 0; pseudo[i]; i++) if (!strcmp(fs, pseudo[i])) skip = 1;
        if (skip) continue;
        if (strncmp(mp, "/media/", 7) && strncmp(mp, "/mnt/", 5) && strncmp(mp, "/run/media/", 11) && strncmp(mp, "/sdcard", 7) && strncmp(mp, "/storage/", 9)) continue;
        int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(v[i], mp)) dup = 1;
        if (!dup) snprintf(v[n++], RX_PATH, "%s", mp);
    }
    if (f) fclose(f);
    struct stat st;
    if (n < max && !stat("/sdcard", &st) && S_ISDIR(st.st_mode)) { int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(v[i], "/sdcard")) dup = 1; if (!dup) snprintf(v[n++], RX_PATH, "/sdcard"); }
    return n;
}
static int vol_is_root(const char *path) {
    char (*v)[RX_PATH] = calloc(32, RX_PATH);
    int n = v ? vol_collect(v, 32) : 0, hit = 0;
    for (int i = 0; i < n; i++) if (!strcmp(v[i], path)) hit = 1;
    free(v);
    return hit;
}
/* one level up; from the top of a drive (or from "/") to the list of drives */
static void browse_up_dir(rx_engine_t *e) {
    size_t l = strlen(e->br_dir);
    while (l > 1 && e->br_dir[l - 1] == '/') e->br_dir[--l] = 0;
    if (!e->br_dir[0] || vol_is_root(e->br_dir)) { e->br_dir[0] = 0; return; }
    char *sl = strrchr(e->br_dir, '/');
    if (!sl || sl == e->br_dir) e->br_dir[0] = 0; else *sl = 0;
}

static void browse_scan(rx_engine_t *e) {   /* worker thread */
    br_entry_t *list = calloc(BROWSE_MAX, sizeof *list);
    int n = 0;
    DIR *d = e->br_dir[0] ? opendir(e->br_dir) : NULL;
    if (!e->br_dir[0]) {   /* the drives */
        char (*v)[RX_PATH] = calloc(32, RX_PATH);
        int nv = v ? vol_collect(v, 32) : 0;
        for (int i = 0; i < nv && n < BROWSE_MAX; i++) {
            br_entry_t *b = &list[n++];
            b->is_dir = 1;
            snprintf(b->path, sizeof b->path, "%s", v[i]);
            const char *bn = strrchr(v[i], '/');
            snprintf(b->name, sizeof b->name, "%s", bn && bn[1] ? bn + 1 : v[i]);
        }
        free(v);
    } else if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < BROWSE_MAX) {
            if (de->d_name[0] == '.') continue;
            char full[RX_PATH];
            snprintf(full, sizeof full, "%s/%s", e->br_dir, de->d_name);
            struct stat st;
            if (stat(full, &st)) continue;
            int is_dir = S_ISDIR(st.st_mode);
            if (!is_dir && !decode_is_audio_file(de->d_name)) continue;
            br_entry_t *b = &list[n++];
            b->is_dir = is_dir;
            snprintf(b->path, sizeof b->path, "%s", full);
            snprintf(b->name, sizeof b->name, "%s", de->d_name);
            if (!is_dir) { char *dot = strrchr(b->name, '.'); if (dot && dot != b->name) *dot = 0; b->bpm = sidecar_bpm(full); }
        }
        closedir(d);
    } else if (e->br_dir[0]) rx_log("browse: cannot open %s: %s", e->br_dir, strerror(errno));
    qsort(list, (size_t)n, sizeof *list, e->br_sort == 1 ? br_cmp_bpm : br_cmp_name);
    pthread_mutex_lock(&e->br_lock);
    free(e->br);
    e->br = list; e->br_n = n;
    if (e->br_page * RX_BROWSE_ROWS >= n) e->br_page = 0;
    e->br_rows_dirty = 1;
    pthread_mutex_unlock(&e->br_lock);
    rx_log("browse: %d entries in %s", n, e->br_dir);
}

int browse_set_folder(rx_engine_t *e, const char *path) { snprintf(e->br_dir, sizeof e->br_dir, "%s", path); browse_scan(e); return e->br_n; }
int browse_count(rx_engine_t *e) { return e->br_n; }
const char *browse_name(rx_engine_t *e, int i) { return (i >= 0 && i < e->br_n) ? e->br[i].name : ""; }
const char *browse_path(rx_engine_t *e, int i) { return (i >= 0 && i < e->br_n) ? e->br[i].path : ""; }
double browse_bpm(rx_engine_t *e, int i) { return (i >= 0 && i < e->br_n) ? e->br[i].bpm : 0; }

/* the six row texts and the path, from the audio thread (short lock; the worker only swaps the list) */
static void browse_rows_update(rx_engine_t *e) {
    if (pthread_mutex_trylock(&e->br_lock)) return;
    char t[160];
    int base = e->br_page * RX_BROWSE_ROWS;
    for (int r = 0; r < RX_BROWSE_ROWS; r++) {
        int i = base + r;
        if (i < e->br_n) {
            const br_entry_t *b = &e->br[i];
            if (b->is_dir) snprintf(t, sizeof t, "%s  [FOLDER]", b->name);
            else if (b->bpm > 0) snprintf(t, sizeof t, "%s   %.1f", b->name, b->bpm);
            else snprintf(t, sizeof t, "%s", b->name);
        } else t[0] = 0;
        ctl_set_text(e->c_browse_row[r], t);
    }
    int pages = (e->br_n + RX_BROWSE_ROWS - 1) / RX_BROWSE_ROWS;
    snprintf(t, sizeof t, "%s   %d / %d", e->br_dir[0] ? e->br_dir : "DRIVES", pages ? e->br_page + 1 : 0, pages);
    ctl_set_text(e->c_path, t);
    ctl_set(e->c_browse_page, e->br_page);
    e->br_rows_dirty = 0;
    pthread_mutex_unlock(&e->br_lock);
}

static void queue_job(rx_engine_t *e, int kind, int deck, const char *path);
/* CONTROLLERS page: six rows of Mixxx maps (file names), the loaded one marked with a star; the status line says what is loaded */
static void maps_rows_update(rx_engine_t *e) {
    int total = midi_mapping_total(e->midi);
    if (total < 0) return;                                   /* the list is being rebuilt: next block */
    char loaded[RX_PATH] = "", lname[128] = "", t[200];
    int nin = 0, nscript = 0, nunk = 0;
    int have = midi_report(e->midi, loaded, sizeof loaded, lname, sizeof lname, &nin, &nscript, &nunk, NULL, 0, 1);
    if (have < 0) return;
    const char *lb = strrchr(loaded, '/'); lb = lb ? lb + 1 : loaded;
    int base = e->mp_page * RX_BROWSE_ROWS;
    for (int r = 0; r < RX_BROWSE_ROWS; r++) {
        char nm[RX_PATH], path[RX_PATH];
        t[0] = 0;
        if (base + r < total && !midi_mapping_name_at(e->midi, base + r, nm, sizeof nm) && !midi_mapping_path_at(e->midi, base + r, path, sizeof path)) {
            const char *pb = strrchr(path, '/'); pb = pb ? pb + 1 : path;
            snprintf(t, sizeof t, "%s%.100s", have && !strcmp(pb, lb) ? "* " : "   ", nm);
        }
        ctl_set_text(e->c_maps_row[r], t);
    }
    int pages = (total + RX_BROWSE_ROWS - 1) / RX_BROWSE_ROWS;
    if (have) snprintf(t, sizeof t, "LOADED %.26s  %d ctl  %d n/a", lname, nin, nunk);
    else snprintf(t, sizeof t, "NO CONTROLLER LOADED  (%d found)", total);
    ctl_set_text(e->c_maps_status, t);
    ctl_set(e->c_maps_page, e->mp_page);
    (void)pages;
    e->mp_dirty = 0;
}

/* the CONTROLLERS page's buttons and cursor (the same wheel/tap logic as the browse list) */
static void maps_actions(rx_engine_t *e) {
    if (!e->midi) return;
    int total = midi_mapping_total(e->midi);
    if (total < 0) return;
    if (ctl_get(e->c_maps_prev) >= 0.5) { ctl_set(e->c_maps_prev, 0); if (e->mp_page > 0) { e->mp_page--; e->mp_dirty = 1; } }
    if (ctl_get(e->c_maps_next) >= 0.5) { ctl_set(e->c_maps_next, 0); if ((e->mp_page + 1) * RX_BROWSE_ROWS < total) { e->mp_page++; e->mp_dirty = 1; } }
    int sel = (int)ctl_get(e->c_maps_sel), v = (int)lround(ctl_get(e->c_maps_cur));
    int idx = e->mp_page * RX_BROWSE_ROWS + sel;
    if (v != sel + 1) { if (v <= 0) idx--; else if (v >= 7) idx++; else idx = e->mp_page * RX_BROWSE_ROWS + (v - 1); }
    if (idx >= total) idx = total - 1;
    if (idx < 0) idx = 0;
    int pg = idx / RX_BROWSE_ROWS, rw = idx % RX_BROWSE_ROWS;
    if (pg != e->mp_page) { e->mp_page = pg; e->mp_dirty = 1; }
    if (rw != sel) { ctl_set(e->c_maps_sel, rw); e->mp_dirty = 1; }
    if (v != rw + 1) ctl_set(e->c_maps_cur, rw + 1);
    int bp = (int)ctl_get(e->c_maps_page);
    if (bp != e->mp_page && bp >= 0 && bp * RX_BROWSE_ROWS < total) { e->mp_page = bp; e->mp_dirty = 1; }
    if (ctl_get(e->c_maps_load) >= 0.5) {
        ctl_set(e->c_maps_load, 0);
        char path[RX_PATH];
        if (total > 0 && !midi_mapping_path_at(e->midi, e->mp_page * RX_BROWSE_ROWS + (int)ctl_get(e->c_maps_sel), path, sizeof path)) queue_job(e, 5, 0, path);
    }
    if (ctl_get(e->c_maps_none) >= 0.5) { ctl_set(e->c_maps_none, 0); queue_job(e, 6, 0, NULL); }
    /* the status line also follows loads made elsewhere (the MIDI MAP Q-Link, the web page) */
    char f[RX_PATH]; int nin = 0, ns = 0, nu = 0;
    int have = midi_report(e->midi, f, sizeof f, NULL, 0, &nin, &ns, &nu, NULL, 0, 1);
    if (have >= 0) {
        int key = have ? nin * 1000 + nu + (int)strlen(f) * 100000 : -1;
        if (key != e->mp_report_seen) { e->mp_report_seen = key; e->mp_dirty = 1; }
    }
    if (e->mp_dirty) maps_rows_update(e);
}

/* the web page's access: a saved map is looked for again, a load goes through the worker */
void engine_map_rescan(rx_engine_t *e) { if (e) queue_job(e, 7, 0, NULL); }
void engine_map_pick(rx_engine_t *e, const char *path) { if (e) queue_job(e, 5, 0, path); }
void engine_map_none(rx_engine_t *e) { if (e) queue_job(e, 6, 0, NULL); }
rx_midi_t *engine_midi(rx_engine_t *e) { return e ? e->midi : NULL; }

/* ------------------------------------------------------------------------------------------------ the worker */
static void queue_job(rx_engine_t *e, int kind, int deck, const char *path) {
    pthread_mutex_lock(&e->wlock);
    if (e->qn < LOADQ) {
        e->q[e->qn].kind = kind; e->q[e->qn].deck = deck;
        snprintf(e->q[e->qn].path, RX_PATH, "%s", path ? path : "");
        e->qn++;
        pthread_cond_signal(&e->wcond);
    }
    pthread_mutex_unlock(&e->wlock);
}

/* the analysis cache next to a track: "<track>.rxa" (binary: magic, counts, bpm, beat0, overview, columns) */
static int cache_read(const char *track, rx_analysis_t *an) {
    char p[RX_PATH]; snprintf(p, sizeof p, "%s.rxa", track);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    char magic[8] = {0}; int32_t ncols = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "RXA1\0\0\0\0", 8) || fread(&ncols, 4, 1, f) != 1 || ncols <= 0 || ncols > RX_WAVE_MAX_COLS) { fclose(f); return 0; }
    memset(an, 0, sizeof *an);
    an->cols = malloc((size_t)ncols * sizeof(rx_wave_col_t));
    if (!an->cols || fread(&an->bpm, 8, 1, f) != 1 || fread(&an->beat0, 8, 1, f) != 1 || fread(&an->gain, 4, 1, f) != 1 ||
        fread(an->overview, sizeof an->overview, 1, f) != 1 || fread(an->cols, sizeof(rx_wave_col_t), (size_t)ncols, f) != (size_t)ncols) {
        free(an->cols); an->cols = NULL; fclose(f); return 0;
    }
    an->ncols = ncols;
    fclose(f);
    return 1;
}
static void cache_write(const char *track, const rx_analysis_t *an) {
    char p[RX_PATH], tmp[RX_PATH]; snprintf(p, sizeof p, "%s.rxa", track); snprintf(tmp, sizeof tmp, "%s.rxa.new", track);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    int32_t ncols = an->ncols;
    int ok = fwrite("RXA1\0\0\0\0", 1, 8, f) == 8 && fwrite(&ncols, 4, 1, f) == 1 && fwrite(&an->bpm, 8, 1, f) == 1 && fwrite(&an->beat0, 8, 1, f) == 1 &&
             fwrite(&an->gain, 4, 1, f) == 1 && fwrite(an->overview, sizeof an->overview, 1, f) == 1 &&
             fwrite(an->cols, sizeof(rx_wave_col_t), (size_t)ncols, f) == (size_t)ncols;
    fclose(f);
    if (ok) rename(tmp, p); else unlink(tmp);
}
static void sidecar_write(const char *track, rx_deck_t *d, double bpm, double beat0) {
    char p[RX_PATH]; sidecar_path(track, p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    char cues[2048]; deck_cues_export(d, cues, sizeof cues);
    fprintf(f, "remixxxl 1\nbpm %.4f\nbeat0 %.6f\n%s", bpm, beat0, cues);
    fclose(f);
}
static void sidecar_read(const char *track, rx_deck_t *d) {
    char p[RX_PATH]; sidecar_path(track, p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char *text = calloc(1, 8192); size_t n = fread(text, 1, 8191, f); text[n] = 0; fclose(f);
    deck_cues_import(d, text);
    free(text);
}

static void load_progress(void *user, int pct) { (void)user; (void)pct; }

static void do_load(rx_engine_t *e, int deck, const char *path) {
    rx_deck_t *d = e->deck[deck];
    rx_log("load: deck %d <- %s", deck + 1, path);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    rx_audio_t *au = calloc(1, sizeof *au);
    int fr = decode_file(path, au, RX_MAX_TRACK_SEC * RX_SR, load_progress, e);
    if (fr <= 0) { rx_log("load: decode failed (%d)", fr); free(au); ctl_set_text(e->c_readout, "LOAD FAILED"); return; }
    rx_analysis_t *an = calloc(1, sizeof *an);
    int cached = cache_read(path, an);
    if (!cached) {
        if (analyze_audio(au->lr, au->frames, an, load_progress, e)) { rx_log("load: analysis failed"); decode_free(au); free(au); free(an); return; }
        cache_write(path, an);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    rx_log("load: %lld frames, bpm %.2f, beat0 %.3f, %s, %.1f s", (long long)au->frames, an->bpm, an->beat0, cached ? "cached analysis" : "analysed",
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    if (deck_set_track(d, path, au, an)) { rx_log("load: deck refused the track"); decode_free(au); free(au); analyze_free(an); free(an); return; }
    sidecar_read(path, d);
    char t[160]; snprintf(t, sizeof t, "DECK %d: %s", deck + 1, au->title[0] ? au->title : path);
    ctl_set_text(e->c_readout, t);
}

static void *worker_main(void *p) {
    rx_engine_t *e = p;
    for (;;) {
        pthread_mutex_lock(&e->wlock);
        while (!e->qn && !e->stop) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            if (pthread_cond_timedwait(&e->wcond, &e->wlock, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&e->wlock); for (int i = 0; i < RX_DECKS; i++) deck_reap(e->deck[i]); pthread_mutex_lock(&e->wlock); }
        }
        if (e->stop) { pthread_mutex_unlock(&e->wlock); return NULL; }
        int kind = e->q[0].kind, deck = e->q[0].deck;
        char path[RX_PATH]; snprintf(path, sizeof path, "%s", e->q[0].path);
        memmove(e->q, e->q + 1, (size_t)(e->qn - 1) * sizeof e->q[0]);
        e->qn--;
        pthread_mutex_unlock(&e->wlock);
        if (kind == 1) do_load(e, deck, path);
        else if (kind == 2) browse_scan(e);
        else if (kind == 4) { browse_up_dir(e); browse_scan(e); }   /* BACK */
        else if (kind == 5) { if (e->midi) midi_pick_mapping(e->midi, path); e->mp_dirty = 1; }   /* CONTROLLERS: LOAD */
        else if (kind == 6) { if (e->midi) midi_pick_none(e->midi); e->mp_dirty = 1; }             /* CONTROLLERS: NONE */
        else if (kind == 7) { if (e->midi) midi_rescan(e->midi); e->mp_dirty = 1; }                /* a map was saved: look again */
        else if (kind == 8) { if (e->midi) { midi_autoload(e->midi); } e->mp_dirty = 1; }
        else if (kind == 3) {   /* save the sidecar of a deck's track */
            rx_deck_t *d = e->deck[deck];
            const char *tp = deck_track_path(d);
            if (tp && tp[0]) {
                rx_deck_view_t v; deck_view(d, &v);
                sidecar_write(tp, d, v.bpm / (v.rate != 0 ? fabs(v.rate) : 1), v.beat0);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ the screen's platter frames */
static void load_platter_frames(rx_engine_t *e) {
    char p[RX_PATH]; snprintf(p, sizeof p, "%s/Screen/platter_frames.png", e->plugin_dir);
    int w, h, comp;
    unsigned char *px = stbi_load(p, &w, &h, &comp, 4);
    if (!px) { rx_log("screen: no %s (%s): plain discs", p, stbi_failure_reason()); return; }
    int frames = h / w;   /* a vertical strip of square frames */
    if (frames < 1) { stbi_image_free(px); return; }
    /* RGBA -> ARGB (B,G,R,A in memory) for the plane */
    for (int i = 0; i < w * h; i++) { unsigned char r = px[i * 4], b = px[i * 4 + 2]; px[i * 4] = b; px[i * 4 + 2] = r; }
    overlay_set_platter_frames(e->ovl, px, w, w, frames);
    rx_log("screen: platter frames %dx%d x %d", w, w, frames);
    stbi_image_free(px);
}

/* the WHEEL page's ring, turned to any angle by the screen (tools/gen_ring.py) */
static void load_ring(rx_engine_t *e) {
    static const struct { int which, d; const char *what; } rings[2] = {{0, 574, "the WHEEL page's big wheel"}, {1, 420, "the WHEELS page's two small wheels"}};
    for (int i = 0; i < 2; i++) {
        char p[RX_PATH], q[RX_PATH];
        snprintf(p, sizeof p, "%s/Screen/ring_%d.png", e->plugin_dir, rings[i].d);
        snprintf(q, sizeof q, "%s/Screen/ring_light_%d.png", e->plugin_dir, rings[i].d);
        int w, h, comp, w2, h2;
        unsigned char *a = stbi_load(p, &w, &h, &comp, 4), *b = stbi_load(q, &w2, &h2, &comp, 4);
        if (a && b && w == h && w2 == w && h2 == h) { screen_set_ring(rings[i].which, a, b, w); rx_log("screen: ring %dx%d (%s turn freely)", w, h, rings[i].what); }
        else rx_log("screen: no usable %s / %s: %s keep the skin's ring", p, q, rings[i].what);
        if (a) stbi_image_free(a);
        if (b) stbi_image_free(b);
    }
}

/* ------------------------------------------------------------------------------------------------ create / destroy */
static void *create(const char *data_dir) {
    rx_engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (data_dir && data_dir[0]) {
        snprintf(e->tracks_dir, sizeof e->tracks_dir, "%s", data_dir);
        snprintf(e->plugin_dir, sizeof e->plugin_dir, "%s", data_dir);
        char *s = strrchr(e->plugin_dir, '/');
        if (s && s != e->plugin_dir) *s = 0; else snprintf(e->plugin_dir, sizeof e->plugin_dir, ".");
    } else { snprintf(e->tracks_dir, sizeof e->tracks_dir, "."); snprintf(e->plugin_dir, sizeof e->plugin_dir, "."); }
    int first = __sync_bool_compare_and_swap(&g_instance, NULL, e);
    e->owns_screen = first;
    if (first) rx_log_open(e->plugin_dir);
    rx_log("reMixXXL %s: create (plugin dir %s, tracks %s, %s)", RX_VERSION_STR, e->plugin_dir, e->tracks_dir, first ? "first instance" : "another instance");
    ctl_init();
    pthread_mutex_init(&e->br_lock, NULL);
    pthread_mutex_init(&e->wlock, NULL);
    pthread_cond_init(&e->wcond, NULL);
    for (int i = 0; i < RX_DECKS; i++) { e->deck[i] = deck_create(i); deck_register_controls(e->deck[i]); }
    e->mixer = mixer_create(); mixer_register_controls(e->mixer);
    e->fx = fx_create(); fx_register_controls(e->fx);
    /* our own controls */
    e->c_deck_sel = ctl_register("[reMixXXL]", "deck_select", CK_INT, 0, 3, 0);
    e->c_wheel_l = ctl_register("[reMixXXL]", "wheel_deck_l", CK_INT, 0, 3, 0);
    e->c_wheel_r = ctl_register("[reMixXXL]", "wheel_deck_r", CK_INT, 0, 3, 1);
    e->c_zoom = ctl_register("[reMixXXL]", "zoom", CK_INT, 0, 4, 2);
    e->c_readout = ctl_register("[reMixXXL]", "readout", CK_TEXT, 0, 0, 0);
    e->c_cpu_text = ctl_register("[reMixXXL]", "cpu_text", CK_TEXT, 0, 0, 0);
    for (int r = 0; r < RX_BROWSE_ROWS; r++) { char n[32]; snprintf(n, sizeof n, "browse_row%d", r + 1); e->c_browse_row[r] = ctl_register("[reMixXXL]", n, CK_TEXT, 0, 0, 0); }
    e->c_browse_sel = ctl_register("[reMixXXL]", "browse_sel", CK_INT, 0, RX_BROWSE_ROWS - 1, 0);
    e->c_brow = ctl_register("[reMixXXL]", "browse_row", CK_INT, 0, 7, 1);
    e->c_browse_page = ctl_register("[reMixXXL]", "browse_page", CK_INT, 0, 999, 0);
    e->c_prev = ctl_register("[reMixXXL]", "browse_page_prev", CK_TRIGGER, 0, 1, 0);
    e->c_next = ctl_register("[reMixXXL]", "browse_page_next", CK_TRIGGER, 0, 1, 0);
    e->c_folder = ctl_register("[reMixXXL]", "browse_folder", CK_TRIGGER, 0, 1, 0);
    e->c_up = ctl_register("[reMixXXL]", "browse_up", CK_TRIGGER, 0, 1, 0);
    e->c_xf_mode = ctl_register("[reMixXXL]", "xf_mode", CK_INT, 0, 5, 0);
    e->c_sort = ctl_register("[reMixXXL]", "browse_sort", CK_INT, 0, 2, 0);
    e->c_scroll = ctl_register("[reMixXXL]", "browse_scroll", CK_FLOAT, 0, 1, 0);
    e->c_path = ctl_register("[reMixXXL]", "browse_path", CK_TEXT, 0, 0, 0);
    for (int i = 0; i < RX_DECKS; i++) {
        char n[40];
        snprintf(n, sizeof n, "overview_tap%d", i + 1); e->c_ov_tap[i] = ctl_register("[reMixXXL]", n, CK_TRIGGER, 0, 1, 0);
        snprintf(n, sizeof n, "[Channel%d]", i + 1);
        e->c_load[i] = ctl_register(n, "LoadSelectedTrack", CK_TRIGGER, 0, 1, 0);
        e->c_track_step[i] = ctl_register(n, "track_step", CK_INT, -1, 1, 0);
    }
    for (int i = 0; i < RX_QR_N * RX_QR_BYTES; i++) { char n[24]; snprintf(n, sizeof n, "qr%03d", i); e->c_qr[i] = ctl_register("[reMixXXL]", n, CK_FLOAT, 0, 1, 0); }
    e->c_mt_led = ctl_register("[reMixXXL]", "mt_led", CK_TOGGLE, 0, 1, 0);
    e->c_midi_map = ctl_register("[reMixXXL]", "midi_map", CK_INT, 0, RX_MAPPINGS_MAX - 1, 0);
    e->c_midi_learn = ctl_register("[reMixXXL]", "midi_learn", CK_TOGGLE, 0, 1, 0);
    e->c_midi_text = ctl_register("[reMixXXL]", "midi_text", CK_TEXT, 0, 0, 0);
    e->c_wheel_l_text = ctl_register("[reMixXXL]", "wheel_l_text", CK_TEXT, 0, 0, 0);
    e->c_wheel_r_text = ctl_register("[reMixXXL]", "wheel_r_text", CK_TEXT, 0, 0, 0);
    e->c_touch_text = ctl_register("[reMixXXL]", "touch_text", CK_TEXT, 0, 0, 0);
    compat_init();
    /* the parameters' controls */
    int missing = 0;
    for (int i = 0; i < RX_NPARAMS; i++) {
        char g[96], n[96];
        ctl_from_spec(e, i, g, n);
        e->pctl[i] = g[0] ? ctl_lookup(g, n) : CTL_NONE;
        if (e->pctl[i] == CTL_NONE && g[0]) { missing++; if (missing <= 40) rx_log("param %s: no control %s,%s (registering)", RX_PARAMS[i].key, g, n);
            e->pctl[i] = ctl_register(g, n, RX_PARAMS[i].kind == RXK_TRIGGER ? CK_TRIGGER : RX_PARAMS[i].kind == RXK_TOGGLE ? CK_TOGGLE : RX_PARAMS[i].kind == RXK_TEXT ? CK_TEXT : RX_PARAMS[i].kind == RXK_KNOB ? CK_FLOAT : CK_INT,
                                      RX_PARAMS[i].lo, RX_PARAMS[i].hi, knob_to_ctl(i, RX_PARAMS[i].def)); }
        e->last_reported[i] = RX_PARAMS[i].def;
        if (e->pctl[i] != CTL_NONE && RX_PARAMS[i].kind != RXK_TEXT && RX_PARAMS[i].kind != RXK_TRIGGER) ctl_set(e->pctl[i], knob_to_ctl(i, RX_PARAMS[i].def));
    }
    if (missing) rx_log("%d parameters had no control registered by their part (registered here)", missing);
    for (int i = 0; i < RX_BROWSE_ROWS; i++) { char n[32]; snprintf(n, sizeof n, "maps_row%d", i + 1); e->c_maps_row[i] = ctl_lookup("[reMixXXL]", n); }
    e->c_maps_sel = ctl_lookup("[reMixXXL]", "maps_sel"); e->c_maps_cur = ctl_lookup("[reMixXXL]", "maps_row");
    e->c_maps_page = ctl_lookup("[reMixXXL]", "maps_page"); e->c_maps_prev = ctl_lookup("[reMixXXL]", "maps_page_prev");
    e->c_maps_next = ctl_lookup("[reMixXXL]", "maps_page_next"); e->c_maps_load = ctl_lookup("[reMixXXL]", "maps_load");
    e->c_maps_none = ctl_lookup("[reMixXXL]", "maps_none"); e->c_maps_status = ctl_lookup("[reMixXXL]", "maps_status");
    e->mp_dirty = 1;
    snprintf(e->br_dir, sizeof e->br_dir, "%s", e->tracks_dir);
    mkdir(e->tracks_dir, 0777);
    ctl_set_text(e->c_readout, "reMixXXL " RX_VERSION_STR);
    pthread_create(&e->worker, NULL, worker_main, e);
    queue_job(e, 2, 0, NULL);
    if (first) {
        int ok = 0;
        e->ovl = overlay_start(e->plugin_dir, &ok);
        rx_log("screen: overlay %s", ok ? "started" : "not available (see remixxxl_overlay.log)");
        if (e->ovl) { load_platter_frames(e); load_ring(e); }
        e->touch = touch_start(e->plugin_dir, e->ovl, e);
        rx_log("touch: %s", touch_available(e->touch) ? "reading the touchscreen directly" : "no direct touch (see the log above)");
        e->web = web_start(e);
        e->midi = midi_start(e->plugin_dir, e);
        queue_job(e, 8, 0, NULL);        /* the controller picked last time loads again (worker) */
        char t[128]; snprintf(t, sizeof t, "MIDI: %d mappings, learn off", midi_mapping_count(e->midi));
        ctl_set_text(e->c_midi_text, t);
        ctl_set_text(e->c_touch_text, touch_available(e->touch) ? "TOUCH: DIRECT" : "TOUCH: OFF");
    }
    return e;
}

static void destroy(void *p) {
    rx_engine_t *e = p;
    pthread_mutex_lock(&e->wlock); e->stop = 1; pthread_cond_signal(&e->wcond); pthread_mutex_unlock(&e->wlock);
    pthread_join(e->worker, NULL);
    if (e->owns_screen) {
        if (e->web) web_stop(e->web);
        if (e->midi) midi_stop(e->midi);
        if (e->touch) touch_stop(e->touch);
        if (e->ovl) overlay_stop(e->ovl);
    }
    for (int i = 0; i < RX_DECKS; i++) deck_destroy(e->deck[i]);
    mixer_destroy(e->mixer);
    fx_destroy(e->fx);
    free(e->br);
    if (e->owns_screen) { g_instance = NULL; ctl_shutdown(); }
    rx_log("destroy");
    free(e);
}

/* ------------------------------------------------------------------------------------------------ MIDI */
static void midi(void *p, const uint8_t *msg, int len) {
    rx_engine_t *e = p;
    if (e->midi) midi_in(e->midi, msg, len);
    else if (g_instance && g_instance->midi) midi_in(g_instance->midi, msg, len);
}

/* ------------------------------------------------------------------------------------------------ parameters */
static void set_param(void *p, const char *key, const char *val) {
    rx_engine_t *e = p;
    if (!strcmp(key, "host_time")) {
        int pl = 0; double ppq = 0, tempo = 0;
        if (sscanf(val, "%d %lf %lf", &pl, &ppq, &tempo) == 3) {
            e->host_playing = pl; e->host_ppq = ppq;
            if (tempo > 0) e->host_bpm = tempo;
            for (int i = 0; i < RX_DECKS; i++) deck_host_tempo(e->deck[i], e->host_bpm, pl);
        }
        return;
    }
    if (!strcmp(key, "state")) {   /* restore: "deck N <path>\n" lines, then each deck's cues */
        const char *s = val;
        char line[RX_PATH + 64];
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t n = nl ? (size_t)(nl - s) : strlen(s);
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, s, n); line[n] = 0;
            s += n + (nl ? 1 : 0);
            int dk; char path[RX_PATH];
            if (sscanf(line, "deck %d %1023[^\n]", &dk, path) == 2 && dk >= 1 && dk <= RX_DECKS) queue_job(e, 1, dk - 1, path);
            else if (!strncmp(line, "folder ", 7)) { snprintf(e->br_dir, sizeof e->br_dir, "%s", line + 7); queue_job(e, 2, 0, NULL); }
            else if (!strncmp(line, "mapping ", 8) && e->midi) midi_load_mapping(e->midi, line + 8);
        }
        return;
    }
    int i = find_param(key);
    if (i < 0) return;
    ctl_id c = e->pctl[i];
    if (c == CTL_NONE) return;
    double v = strtod(val, NULL);
    switch (RX_PARAMS[i].kind) {
    case RXK_TEXT: return;
    case RXK_TRIGGER: if (v >= 0.5) ctl_set(c, 1); break;
    case RXK_TOGGLE: ctl_set(c, v >= 0.5 ? 1 : 0); break;
    case RXK_ENUM: case RXK_INT: { int k = (int)lround(v); if (k < 0) k = 0; if (k > RX_PARAMS[i].hi) k = (int)RX_PARAMS[i].hi; ctl_set(c, k); break; }
    default: if (v < 0) v = 0; if (v > 1) v = 1; ctl_set(c, knob_to_ctl(i, v)); break;
    }
    e->last_reported[i] = (float)v;
}

static int put(char *buf, int len, const char *s) { int n = snprintf(buf, (size_t)len, "%s", s); return n + 1; }

static int get_param(void *p, const char *key, char *buf, int len) {
    rx_engine_t *e = p;
    if (len <= 0) { if (!strcmp(key, "state")) return (int)sizeof e->state_cache; return 0; }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < RX_DECKS; i++) {
            const char *tp = deck_track_path(e->deck[i]);
            if (tp && tp[0]) n += snprintf(buf + n, (size_t)(len - n > 0 ? len - n : 0), "deck %d %s\n", i + 1, tp);
        }
        n += snprintf(buf + n, (size_t)(len - n > 0 ? len - n : 0), "folder %s\n", e->br_dir);
        if (e->midi && midi_mapping_name(e->midi) && midi_mapping_name(e->midi)[0]) n += snprintf(buf + n, (size_t)(len - n > 0 ? len - n : 0), "mapping %s\n", midi_mapping_name(e->midi));
        return n + 1;
    }
    if (!strcmp(key, "dirty")) {   /* the parameters whose controls moved by themselves (MIDI, the engine) since MPC last heard */
        int n = 0;
        for (int i = 0; i < RX_NPARAMS && n < len - 8; i++) {
            if (e->pctl[i] == CTL_NONE || RX_PARAMS[i].kind == RXK_TEXT || RX_PARAMS[i].kind == RXK_TRIGGER) continue;
            double v = ctl_get(e->pctl[i]);
            float pv = (float)(RX_PARAMS[i].kind == RXK_KNOB ? ctl_to_knob(i, v) : v);
            if (fabsf(pv - e->last_reported[i]) > 1e-4f) { e->last_reported[i] = pv; n += snprintf(buf + n, (size_t)(len - n), "%d ", i); }
        }
        return n > 0 ? n + 1 : put(buf, len, " ");
    }
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display")) {   /* a value's text */
        char base[64]; snprintf(base, sizeof base, "%.*s", (int)(kl - 8), key);
        int i = find_param(base);
        if (i < 0 || e->pctl[i] == CTL_NONE) return 0;
        double v = ctl_get(e->pctl[i]);
        if (kl > 13 && !strcmp(base + strlen(base) - 5, "_rate")) {   /* the real speed change: + faster, - slower (the + is at the bottom of the fader) */
            char g[24]; snprintf(g, sizeof g, "[Channel%c]", base[1]);
            ctl_id rr = ctl_lookup(g, "rateRange"), rd = ctl_lookup(g, "rate_dir");
            double sp = v * (rr != CTL_NONE ? ctl_get(rr) : 0.1) * (rd != CTL_NONE ? ctl_get(rd) : -1);
            return snprintf(buf, (size_t)len, "%+.2f%%", sp * 100) + 1;
        }
        if (!strncmp(base, "fx", 2) && strstr(base, "_p")) {   /* an effect parameter: the effect's own value text */
            int u = base[2] - '1', s = base[4] - '1';
            char g[64]; snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d_Effect%d]", u + 1, s + 1);
            int fxi = (int)ctl_get(ctl_lookup(g, "effect_idx"));
            if (fxi <= 0) return put(buf, len, "-");
            return snprintf(buf, (size_t)len, "%.0f%%", v * 100) + 1;
        }
        return 0;
    }
    if (kl > 5 && !strcmp(key + kl - 5, "_name")) {   /* dynamic names: the effect parameters */
        char base[64]; snprintf(base, sizeof base, "%.*s", (int)(kl - 5), key);
        if (!strncmp(base, "fx", 2) && base[3] == '_' && base[5] == '_' && base[6] == 'p') {
            int u = base[2] - '1', s = base[4] - '1', pn = base[7] - '1';
            char g[64]; snprintf(g, sizeof g, "[EffectRack1_EffectUnit%d_Effect%d]", u + 1, s + 1);
            int fxi = (int)ctl_get(ctl_lookup(g, "effect_idx"));
            const char *nm = fx_param_name(fxi, pn);
            return put(buf, len, nm ? nm : "-");
        }
        if (!strcmp(base, "midi_map") && e->midi) {
            int k = (int)ctl_get(e->c_midi_map);
            return put(buf, len, k <= 0 ? "MIDI MAP: NONE" : midi_mapping_file(e->midi, k - 1) ? midi_mapping_file(e->midi, k - 1) : "MIDI MAP");
        }
        return 0;
    }
    int i = find_param(key);
    if (i < 0 || e->pctl[i] == CTL_NONE) return 0;
    if (RX_PARAMS[i].kind == RXK_TEXT) { ctl_text(e->pctl[i], buf, len); return (int)strlen(buf) + 1; }
    double v = ctl_get(e->pctl[i]);
    if (RX_PARAMS[i].kind == RXK_TRIGGER) return put(buf, len, "0");
    if (RX_PARAMS[i].kind == RXK_KNOB) v = ctl_to_knob(i, v);
    return snprintf(buf, (size_t)len, "%.5g", v) + 1;
}

/* ------------------------------------------------------------------------------------------------ render */
static void engine_actions(rx_engine_t *e) {
    /* our own triggers, once per block, after ctl_drain */
    for (int i = 0; i < RX_DECKS; i++) {
        if (ctl_get(e->c_load[i]) >= 0.5) {
            ctl_set(e->c_load[i], 0);
            int sel = (int)ctl_get(e->c_browse_sel) + e->br_page * RX_BROWSE_ROWS;
            if (!pthread_mutex_trylock(&e->br_lock)) {
                if (sel >= 0 && sel < e->br_n) {
                    if (e->br[sel].is_dir) { snprintf(e->br_dir, sizeof e->br_dir, "%s", e->br[sel].path); e->br_page = 0; queue_job(e, 2, 0, NULL); }
                    else queue_job(e, 1, i, e->br[sel].path);
                }
                pthread_mutex_unlock(&e->br_lock);
            }
        }
        if (ctl_get(e->c_ov_tap[i]) >= 0.5) { ctl_set(e->c_ov_tap[i], 0); ctl_set(e->c_deck_sel, i); }
        int step = (int)ctl_get(e->c_track_step[i]);
        if (step) {   /* the deck's TRACK |< >| : the neighbour of its track in the list */
            ctl_set(e->c_track_step[i], 0);
            const char *tp = deck_track_path(e->deck[i]);
            if (tp && !pthread_mutex_trylock(&e->br_lock)) {
                int at = -1;
                for (int k = 0; k < e->br_n; k++) if (!strcmp(e->br[k].path, tp)) { at = k; break; }
                int to = at < 0 ? 0 : at + step;
                while (to >= 0 && to < e->br_n && e->br[to].is_dir) to += step;
                if (to >= 0 && to < e->br_n) queue_job(e, 1, i, e->br[to].path);
                pthread_mutex_unlock(&e->br_lock);
            }
        }
    }
    if (ctl_get(e->c_prev) >= 0.5) { ctl_set(e->c_prev, 0); if (e->br_page > 0) { e->br_page--; e->br_rows_dirty = 1; } }
    if (ctl_get(e->c_next) >= 0.5) { ctl_set(e->c_next, 0); if ((e->br_page + 1) * RX_BROWSE_ROWS < e->br_n) { e->br_page++; e->br_rows_dirty = 1; } }
    if (ctl_get(e->c_up) >= 0.5) { ctl_set(e->c_up, 0); e->br_page = 0; queue_job(e, 4, 0, NULL); }
    if (ctl_get(e->c_folder) >= 0.5) {   /* FOLDER: open the selected row if it is a folder, else go up */
        ctl_set(e->c_folder, 0);
        int sel = (int)ctl_get(e->c_browse_sel) + e->br_page * RX_BROWSE_ROWS;
        int done = 0;
        if (!pthread_mutex_trylock(&e->br_lock)) {
            if (sel >= 0 && sel < e->br_n && e->br[sel].is_dir) { snprintf(e->br_dir, sizeof e->br_dir, "%s", e->br[sel].path); done = 1; }
            pthread_mutex_unlock(&e->br_lock);
        }
        e->br_page = 0; queue_job(e, done ? 2 : 4, 0, NULL);
    }
    {   /* XFDR MODE: SLOW SMOOTH FAST CUT REV SLOW REV FAST -> the mixer's Mixxx crossfader controls (curve, constant power, reverse) */
        static int last_xf = -1;
        int m = (int)ctl_get(e->c_xf_mode);
        if (m != last_xf) {
            last_xf = m;
            static const double curve[6] = {1.0, 1.0, 6.0, 100.0, 1.0, 6.0};
            static const int constpwr[6] = {0, 1, 0, 0, 0, 0}, rev[6] = {0, 0, 0, 0, 1, 1};
            if (m >= 0 && m < 6) {
                ctl_set(ctl_lookup("[Mixer Profile]", "xFaderCurve"), curve[m]);
                ctl_set(ctl_lookup("[Mixer Profile]", "xFaderMode"), constpwr[m]);
                ctl_set(ctl_lookup("[Mixer Profile]", "xFaderReverse"), rev[m]);
            }
        }
    }
    int sort = (int)ctl_get(e->c_sort);
    if (sort != e->br_sort) { e->br_sort = sort; queue_job(e, 2, 0, NULL); }
    double sc = ctl_get(e->c_scroll);
    { static double last_sc = -1; if (fabs(sc - last_sc) > 1e-6) { last_sc = sc; int pages = (e->br_n + RX_BROWSE_ROWS - 1) / RX_BROWSE_ROWS; if (pages > 0) { int pg = (int)(sc * (pages - 1) + 0.5); if (pg != e->br_page) { e->br_page = pg; e->br_rows_dirty = 1; } } } }
    {   /* the list cursor: br_row 1..6 = a row of this page (a tap); 0 / 7 = the data wheel or a Q-Link ran off the top / bottom of the
         * page: move one entry (turning the page when it crosses) and put the value back, so the wheel never stops at a page edge */
        int sel = (int)ctl_get(e->c_browse_sel), v = (int)lround(ctl_get(e->c_brow)), n = e->br_n;
        int idx = e->br_page * RX_BROWSE_ROWS + sel;
        if (v != sel + 1) {
            if (v <= 0) idx--; else if (v >= 7) idx++; else idx = e->br_page * RX_BROWSE_ROWS + (v - 1);
        }
        if (idx >= n) idx = n - 1;
        if (idx < 0) idx = 0;
        int pg = idx / RX_BROWSE_ROWS, rw = idx % RX_BROWSE_ROWS;
        if (pg != e->br_page) { e->br_page = pg; e->br_rows_dirty = 1; }
        if (rw != sel) { ctl_set(e->c_browse_sel, rw); e->br_rows_dirty = 1; }
        if (v != rw + 1) ctl_set(e->c_brow, rw + 1);
    }
    int bp = (int)ctl_get(e->c_browse_page);
    if (bp != e->br_page && bp * RX_BROWSE_ROWS < e->br_n && bp >= 0) { e->br_page = bp; e->br_rows_dirty = 1; }
    if (e->br_rows_dirty) browse_rows_update(e);
    maps_actions(e);
    /* MIDI mapping select and learn */
    if (e->midi) {
        static int last_map = -1;
        int m = (int)ctl_get(e->c_midi_map);
        if (m != last_map) { last_map = m; if (m > 0 && midi_mapping_file(e->midi, m - 1)) midi_load_mapping(e->midi, midi_mapping_file(e->midi, m - 1)); }
    }
    if (e->touch) touch_set_wheel_decks(e->touch, (int)ctl_get(e->c_wheel_l), (int)ctl_get(e->c_wheel_r));
}

static void publish_view(rx_engine_t *e) {
    rx_view_t *v = &e->view;
    for (int i = 0; i < RX_DECKS; i++) deck_view(e->deck[i], &v->deck[i]);
    v->page = e->ovl ? overlay_page_shown(e->ovl) : -1;
    v->deck_select = (int)ctl_get(e->c_deck_sel);
    v->wheel_deck[0] = (int)ctl_get(e->c_wheel_l);
    v->wheel_deck[1] = (int)ctl_get(e->c_wheel_r);
    static const double ZOOMS[] = {2, 4, 8, 16, 32};
    int z = (int)ctl_get(e->c_zoom); if (z < 0) z = 0; if (z > 4) z = 4;
    v->zoom = ZOOMS[z];
    v->cpu_pct = e->cpu_pct;
    mixer_view(e->mixer, v);
    if (e->ovl) overlay_publish(e->ovl, v);
    if (e->web) web_publish(e->web, v);
}

static void render(void *p, int16_t *out, int frames) {
    rx_engine_t *e = p;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ctl_drain();
    for (int i = 0; i < RX_DECKS; i++) deck_tick_controls(e->deck[i]);
    compat_tick();
    engine_actions(e);
    if (e->midi) midi_tick(e->midi);
    for (int i = 0; i < RX_DECKS; i++) { rx_deck_view_t dv; deck_view(e->deck[i], &dv); fx_set_channel_tempo(e->fx, i, dv.bpm > 0 ? dv.bpm : (e->host_bpm > 0 ? e->host_bpm : 120)); }
    { rx_deck_view_t dv; deck_view(e->deck[(int)ctl_get(e->c_deck_sel) & 3], &dv); fx_set_channel_tempo(e->fx, RX_DECKS, dv.bpm > 0 ? dv.bpm : (e->host_bpm > 0 ? e->host_bpm : 120)); }
    float *dl[RX_DECKS];
    for (int i = 0; i < RX_DECKS; i++) { dl[i] = e->dbuf[i]; deck_render(e->deck[i], dl[i], frames); }
    mixer_render(e->mixer, dl, e->mbuf, e->cbuf, frames, e->fx);
    rx_cue_push(e->cbuf, frames);
    for (int i = 0; i < frames * 2; i++) {
        float s = e->mbuf[i] * 32767.0f;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    e->cpu_acc += us / (2902.0 * frames / RX_BLOCK) * 100; e->cpu_n++;
    if (e->cpu_n >= 100) {
        e->cpu_pct = e->cpu_acc / e->cpu_n; e->cpu_acc = 0; e->cpu_n = 0;
        char t[32]; snprintf(t, sizeof t, "CPU %.0f%%", e->cpu_pct); ctl_set_text(e->c_cpu_text, t);
        {   /* the on-screen diagnosis in the wheel pages' TOUCH text: about every 1.5 s another line (touch state, then the live
             * screen's last log lines in 30-character pieces), so a photo of the page says what the logs say */
            static int step = 0; static char line[160]; static int piece = 0;
            if (++e->diag_tick % 5 == 0) {
                if (piece == 0) {
                    int k = step++ % 7;
                    if (k == 6) snprintf(line, sizeof line, "WEB %s", e->web ? web_url(e->web) : "off");
                    else if (k == 0) touch_status(e->touch, line, sizeof line);
                    else if (!overlay_status(e->ovl, k - 1, line, sizeof line)) { step = 0; snprintf(line, sizeof line, "WEB %s", e->web ? web_url(e->web) : "off"); }
                }
                char show[40]; snprintf(show, sizeof show, "%.30s", line + piece);
                ctl_set_text(e->c_touch_text, show);
                piece += 30;
                if (piece >= (int)strlen(line)) piece = 0;
            }
        }
        if (++e->dirty_tick % 20 == 0) rx_log("cpu: %.1f%% of the block budget", e->cpu_pct);
    }
    e->blocks++;
    {   /* the MASTER TEMPO lamp: lit ~80 ms at each beat of the sync master (else the first playing deck); held long enough for MPC's 40 ms poll */
        static ctl_id sm[RX_DECKS], ba[RX_DECKS], pl[RX_DECKS]; static int got = 0;
        if (!got) for (int i = 0; i < RX_DECKS; i++) { char g[16]; snprintf(g, sizeof g, "[Channel%d]", i + 1); sm[i] = ctl_lookup(g, "sync_master"); ba[i] = ctl_lookup(g, "beat_active"); pl[i] = ctl_lookup(g, "play"); got = 1; }
        int m = -1;
        for (int i = 0; i < RX_DECKS && m < 0; i++) if (sm[i] != CTL_NONE && ctl_get(sm[i]) >= 0.5) m = i;
        for (int i = 0; i < RX_DECKS && m < 0; i++) if (pl[i] != CTL_NONE && ctl_get(pl[i]) >= 0.5) m = i;
        if (m >= 0 && ba[m] != CTL_NONE && ctl_get(ba[m]) >= 0.5) e->mt_hold = 28;
        int on = e->mt_hold > 0; if (e->mt_hold > 0) e->mt_hold--;
        if (on != (int)ctl_get(e->c_mt_led)) ctl_set(e->c_mt_led, on);
    }
    if (e->web && ++e->qr_tick % 16 == 0) {      /* the QR code of the companion page's address, into its film-strip controls when it changed */
        unsigned char qb[RX_QR_N * RX_QR_BYTES]; int ok = web_qr(e->web, qb);
        if (ok) for (int i = 0; i < RX_QR_N * RX_QR_BYTES; i++) if (qb[i] != e->qr_sent[i]) { e->qr_sent[i] = qb[i]; ctl_set(e->c_qr[i], qb[i] / 255.0); }
    }
    if (e->ovl || e->web) publish_view(e);
    /* the sidecar of a deck whose cues changed: deck.c raises "[ChannelN],cues_dirty"; saved by the worker */
    for (int i = 0; i < RX_DECKS; i++) {
        static ctl_id cd[RX_DECKS] = {CTL_NONE, CTL_NONE, CTL_NONE, CTL_NONE};
        if (cd[i] == CTL_NONE) { char g[16]; snprintf(g, sizeof g, "[Channel%d]", i + 1); cd[i] = ctl_lookup(g, "cues_dirty"); }
        if (cd[i] != CTL_NONE && ctl_get(cd[i]) >= 0.5) { ctl_set(cd[i], 0); queue_job(e, 3, i, NULL); }
    }
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
