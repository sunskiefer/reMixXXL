/* reMixXXL -- raw MIDI from the host: MIDI learn, Mixxx controller mappings (<name>.midi.xml + scripts), and LED
 * feedback through an ALSA sequencer port.
 *
 * Threads
 *   midi_in()   AUDIO thread, inside the wrapper's lock, one 3-byte message at a time. It must not block, log or
 *               allocate. It only reads the mapping tables (swapped atomically by midi_load_mapping), posts control
 *               values with ctl_post/ctl_post_delta, queues script bindings for the JS worker (js_post_binding,
 *               lock-free) and queues LED bytes (lock-free ring). Learn bookkeeping writes a few atomics; the file
 *               save happens on the next midi_mapping_count/midi_learn_* call off the audio thread ("dirty" flag).
 *   output listeners (<output> LED feedback): ctl listeners fire in render() -> they only enqueue a MIDI message.
 *   the JS worker (midi_js.c) runs scripts and calls our idle hook every ~2 ms: that drains the feedback ring to ALSA
 *               and flushes the deferred learn file. So ALSA is never touched from the audio thread either.
 *   midi_tick() per block on the audio thread: kept for future use, does nothing today (see remix.h).
 *
 * Mixxx option semantics follow src/controllers/midi/midicontroller.cpp (computeValue / processInputMapping):
 *   normal            value 0..127 scaled to the control's min..max (CK_FLOAT/CK_INT); note messages on a toggle:
 *                     toggle on press; on a trigger: 1 on press, 0 on release (so hold-type triggers work)
 *   invert            127 - value, before everything else
 *   button            pressed = value != 0 (any status), i.e. the message is treated as a note
 *   switch            always "pressed"
 *   script-binding    <key> is a JS function called as fn(channel, control, value, status, group) on the worker
 *   selectknob        two's complement (v >= 64 -> v - 128), not accumulated: posted as a delta (SelectTrackKnob)
 *   diff              two's complement delta added to the current value (relative CC)
 *   spread64          v - 64 as a signed amount (jog-type controls get it as a delta)
 *   rot64/rot64inv    +-1 steps of 1/16 around 64 added to the current MIDI value; rot64fast x1.5
 *   hercjog/fast      two's complement delta (x3 for fast) added to the current value
 *   soft-takeover     Mixxx's SoftTakeover (midi_js.c js_st_*) on the normalised parameter
 *   fourteen-bit-msb/lsb   the two halves are joined when both have arrived (either order), value = 14bit/128
 *   pitch bend        (d2 << 7 | d1) / 128 as the MIDI value, like Mixxx
 *
 * MIDI learn: midi_learn_arm(target); the next note/CC/pitch-bend message becomes the mapping, saved as a line in
 * <plugin_dir>/midi_learn.txt ("status data1 group,name mode"; mode note|abs|rel|bend). A CC on a CK_FLOAT/CK_INT
 * control starts as "abs"; if its first values all look like an encoder (1..7 / 121..127, or 63/65) the entry turns
 * "rel". Learned entries take precedence over the loaded mapping's entries for the same status/data1 (Mixxx's
 * commitTemporaryInputMappings replaces duplicates the same way).
 *
 * Feedback: MPC OS ignores a VST's MIDI output, so an ALSA sequencer client "reMixXXL" with a port "LED Out" is
 * created through libasound.so.2 loaded with dlopen at runtime (no link dependency; the cross sysroot has no ALSA
 * headers, so the few struct/enum bits are declared below from the ALSA 1.x ABI). The user has to route that port
 * to the controller in MPC's MIDI preferences (or with aconnect); UNVERIFIED on the device: whether MPC OS lists
 * sequencer clients created by a plugin is unknown until tried. Without libasound the feedback is counted and
 * logged, nothing else. */
#define _GNU_SOURCE
#include "remix.h"
#include "midi_js.h"
#include "midi_xml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define MAX_INPUTS 1024
#define MAX_OUTPUTS 512
#define MAX_LEARN 256
#define MAX_SCRIPTS 8
#define FB_RING 512
#define MAX_MAPPING_FILES 512

/* Mixxx MidiOption bits */
enum {
    OPT_INVERT = 1 << 0, OPT_ROT64 = 1 << 1, OPT_ROT64INV = 1 << 2, OPT_ROT64FAST = 1 << 3, OPT_DIFF = 1 << 4,
    OPT_BUTTON = 1 << 5, OPT_SWITCH = 1 << 6, OPT_HERCJOG = 1 << 7, OPT_SPREAD64 = 1 << 8, OPT_SELECTKNOB = 1 << 9,
    OPT_SOFT_TAKEOVER = 1 << 10, OPT_SCRIPT = 1 << 11, OPT_14BIT_MSB = 1 << 12, OPT_14BIT_LSB = 1 << 13,
    OPT_HERCJOGFAST = 1 << 14,
};

typedef struct {
    uint16_t key;                        /* status << 8 | data1 (0xFF data1 for one-data-byte / pitch bend messages) */
    uint32_t options;
    ctl_id ctl;                          /* CTL_NONE for a script binding or an unknown control */
    char group[48], name[64];            /* name is the JS function path for a script binding */
} in_map_t;

typedef struct {
    ctl_id ctl;
    int listener;
    uint8_t status, data1, on, off;
    double min, max;
    atomic_int last;                     /* last byte sent, -1 none */
    struct rx_midi *owner;
} out_map_t;

typedef struct {
    in_map_t in[MAX_INPUTS]; int nin;
    out_map_t out[MAX_OUTPUTS]; int nout;
    char name[128], controller_id[64], file[RX_PATH];
    char prefixes[MAX_SCRIPTS][32]; int nprefix;
    /* 14-bit pairing: the half that arrived first, per control */
    struct { ctl_id ctl; int have, value, is_msb; } pend14[16];
    int npend14;
} mapping_t;

typedef struct {
    uint16_t key;
    ctl_id ctl;
    int mode;                            /* LM_* */
    int nseen;                           /* values seen so far (for the encoder detection) */
    int looks_rel;                       /* all values so far looked like an encoder */
    char group[48], name[64];
} learn_t;
enum { LM_NOTE, LM_ABS, LM_REL, LM_BEND };

struct rx_midi {
    char plugin_dir[RX_PATH];
    rx_engine_t *engine;
    rx_js_t *js;
    mapping_t *_Atomic map;              /* the loaded mapping, swapped whole */
    mapping_t *old_map;                  /* the previous one, freed after a grace period (midi_in may still read it) */
    double old_map_t;

    learn_t learn[MAX_LEARN]; atomic_int nlearn;
    atomic_int learn_target;             /* ctl_id armed, or CTL_NONE */
    atomic_int learn_dirty;              /* save the file on the next off-audio call */

    /* feedback ring: MPSC (audio-thread listeners and the JS worker write; the worker's idle drains) */
    struct { uint8_t b[3]; } fb[FB_RING];
    atomic_int fb_ready[FB_RING];
    atomic_uint fb_w, fb_r;
    atomic_int fb_dropped, fb_sent, fb_unrouted;

    /* ALSA, through dlopen */
    void *asound;
    void *seq;
    int port;
    int alsa_ok;

    char files[MAX_MAPPING_FILES][RX_PATH]; int nfiles;
    pthread_mutex_t files_lock;          /* the list is rebuilt by midi_rescan (worker); readers try-lock */
    atomic_int dropped_logged;

    /* the last raw note/CC/bend message (the web map builder shows it and learns from it) */
    atomic_int last_seq, last_st, last_d1, last_d2;
    uint8_t unseen[65536 / 8];           /* messages seen with no binding (audio thread sets, the idle hook logs each once) */
    atomic_uint un_w, un_r; atomic_int un_q[32];

    /* what the last midi_load_mapping found (text for the web page; written by the loader, read by the web thread) */
    pthread_mutex_t rep_lock;
    char rep_file[RX_PATH], rep_name[128];
    int rep_in, rep_script, rep_unknown, rep_out, rep_out_unknown;
    char rep_unknown_list[2048];
};

/* ------------------------------------------------------------------------------------------------ ALSA (dlopen) */
/* The bits of <alsa/seq_event.h> / <alsa/seq.h> we use, from the ALSA 1.x ABI (unchanged since 1.0). */
typedef struct { unsigned char client, port; } a_addr_t;
typedef struct { unsigned int tv_sec, tv_nsec; } a_real_time_t;
typedef union { unsigned int tick; a_real_time_t time; } a_timestamp_t;
typedef struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } a_ev_note_t;
typedef struct { unsigned char channel, unused[3]; unsigned int param; signed int value; } a_ev_ctrl_t;
typedef struct { unsigned int len; void *ptr; } a_ev_ext_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    a_timestamp_t time;
    a_addr_t source, dest;
    union { a_ev_note_t note; a_ev_ctrl_t control; unsigned char raw8[12]; unsigned int raw32[3]; a_ev_ext_t ext; } data;
} a_event_t;                             /* 28 bytes on 32-bit, 32 on 64-bit: same as snd_seq_event_t */
enum { A_EV_NOTEON = 6, A_EV_NOTEOFF = 7, A_EV_KEYPRESS = 8, A_EV_CONTROLLER = 10, A_EV_PGMCHANGE = 11, A_EV_CHANPRESS = 12, A_EV_PITCHBEND = 13 };
#define A_QUEUE_DIRECT 253
#define A_ADDRESS_SUBSCRIBERS 254
#define A_OPEN_OUTPUT 1
#define A_PORT_CAP_READ (1 << 0)
#define A_PORT_CAP_SUBS_READ (1 << 5)
#define A_PORT_TYPE_MIDI_GENERIC (1 << 1)
#define A_PORT_TYPE_APPLICATION (1 << 20)

static int (*a_open)(void **, const char *, int, int);
static int (*a_close)(void *);
static int (*a_set_client_name)(void *, const char *);
static int (*a_create_simple_port)(void *, const char *, unsigned int, unsigned int);
static int (*a_event_output_direct)(void *, a_event_t *);
static int (*a_client_id)(void *);
static const char *(*a_strerror)(int);

static void alsa_open(rx_midi_t *m) {
    m->asound = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!m->asound) { rx_log("midi: libasound.so.2 not available (%s): LED feedback is logged only", dlerror()); return; }
    a_open = dlsym(m->asound, "snd_seq_open");
    a_close = dlsym(m->asound, "snd_seq_close");
    a_set_client_name = dlsym(m->asound, "snd_seq_set_client_name");
    a_create_simple_port = dlsym(m->asound, "snd_seq_create_simple_port");
    a_event_output_direct = dlsym(m->asound, "snd_seq_event_output_direct");
    a_client_id = dlsym(m->asound, "snd_seq_client_id");
    a_strerror = dlsym(m->asound, "snd_strerror");
    if (!a_open || !a_close || !a_set_client_name || !a_create_simple_port || !a_event_output_direct) {
        rx_log("midi: libasound.so.2 lacks the sequencer API: LED feedback is logged only");
        dlclose(m->asound); m->asound = NULL; return;
    }
    int rc = a_open(&m->seq, "default", A_OPEN_OUTPUT, 0);
    if (rc < 0) { rx_log("midi: snd_seq_open failed: %s (no /dev/snd/seq?) -> LED feedback logged only", a_strerror ? a_strerror(rc) : "?"); dlclose(m->asound); m->asound = NULL; return; }
    a_set_client_name(m->seq, "reMixXXL");
    m->port = a_create_simple_port(m->seq, "LED Out", A_PORT_CAP_READ | A_PORT_CAP_SUBS_READ, A_PORT_TYPE_MIDI_GENERIC | A_PORT_TYPE_APPLICATION);
    if (m->port < 0) { rx_log("midi: snd_seq_create_simple_port failed: %s", a_strerror ? a_strerror(m->port) : "?"); a_close(m->seq); m->seq = NULL; dlclose(m->asound); m->asound = NULL; return; }
    m->alsa_ok = 1;
    rx_log("midi: ALSA sequencer client %d port %d \"reMixXXL:LED Out\" open; route it to the controller in MPC's MIDI "
           "preferences (unverified on MPC OS: whether a plugin's sequencer client is listed there)", a_client_id ? a_client_id(m->seq) : -1, m->port);
}

static void alsa_send(rx_midi_t *m, const uint8_t *b) {
    a_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.queue = A_QUEUE_DIRECT;
    ev.source.port = (unsigned char)m->port;
    ev.dest.client = A_ADDRESS_SUBSCRIBERS;
    ev.dest.port = 0;
    int ch = b[0] & 0x0f;
    switch (b[0] & 0xf0) {
    case 0x80: ev.type = A_EV_NOTEOFF; ev.data.note.channel = ch; ev.data.note.note = b[1]; ev.data.note.velocity = b[2]; break;
    case 0x90: ev.type = A_EV_NOTEON; ev.data.note.channel = ch; ev.data.note.note = b[1]; ev.data.note.velocity = b[2]; break;
    case 0xA0: ev.type = A_EV_KEYPRESS; ev.data.note.channel = ch; ev.data.note.note = b[1]; ev.data.note.velocity = b[2]; break;
    case 0xB0: ev.type = A_EV_CONTROLLER; ev.data.control.channel = ch; ev.data.control.param = b[1]; ev.data.control.value = b[2]; break;
    case 0xC0: ev.type = A_EV_PGMCHANGE; ev.data.control.channel = ch; ev.data.control.value = b[1]; break;
    case 0xD0: ev.type = A_EV_CHANPRESS; ev.data.control.channel = ch; ev.data.control.value = b[1]; break;
    case 0xE0: ev.type = A_EV_PITCHBEND; ev.data.control.channel = ch; ev.data.control.value = ((b[2] << 7) | b[1]) - 8192; break;
    default: return;
    }
    int rc = a_event_output_direct(m->seq, &ev);
    if (rc < 0) { static int warned; if (warned++ < 5) rx_log("midi: snd_seq_event_output_direct: %s", a_strerror ? a_strerror(rc) : "?"); }
    else atomic_fetch_add(&m->fb_sent, 1);
}

/* ------------------------------------------------------------------------------------------------ feedback ring */
static void fb_push(rx_midi_t *m, uint8_t status, uint8_t d1, uint8_t d2) {
    /* any thread, lock-free: claim a slot, fill, mark ready */
    unsigned w = atomic_fetch_add(&m->fb_w, 1);
    if (w - atomic_load(&m->fb_r) >= FB_RING) { atomic_fetch_add(&m->fb_dropped, 1); return; }
    m->fb[w % FB_RING].b[0] = status; m->fb[w % FB_RING].b[1] = d1; m->fb[w % FB_RING].b[2] = d2;
    atomic_store(&m->fb_ready[w % FB_RING], 1);
}

static void fb_drain(rx_midi_t *m) {
    int n = 0;
    for (;;) {
        unsigned r = atomic_load(&m->fb_r);
        if (r == atomic_load(&m->fb_w)) break;
        if (!atomic_load(&m->fb_ready[r % FB_RING])) break;
        if (m->alsa_ok) alsa_send(m, m->fb[r % FB_RING].b);
        else atomic_fetch_add(&m->fb_unrouted, 1);
        atomic_store(&m->fb_ready[r % FB_RING], 0);
        atomic_store(&m->fb_r, r + 1);
        if (++n > FB_RING) break;
    }
    unsigned w = atomic_load(&m->fb_w), r = atomic_load(&m->fb_r);
    if (w - r >= FB_RING) atomic_store(&m->fb_r, w);        /* overrun by writers: resync */
}

static void hook_send(void *user, uint8_t status, uint8_t d1, uint8_t d2) { fb_push((rx_midi_t *)user, status, d1, d2); }

/* ------------------------------------------------------------------------------------------------ learn file */
static const char *lm_name(int mode) { return mode == LM_NOTE ? "note" : mode == LM_REL ? "rel" : mode == LM_BEND ? "bend" : "abs"; }

static void learn_save(rx_midi_t *m) {
    char path[RX_PATH + 32];
    snprintf(path, sizeof path, "%s/midi_learn.txt", m->plugin_dir);
    FILE *f = fopen(path, "w");
    if (!f) { rx_log("midi: cannot write %s: %s", path, strerror(errno)); return; }
    fprintf(f, "# reMixXXL MIDI learn: status data1 group,name mode   (data1 255 = pitch bend / one-byte message)\n");
    int n = atomic_load(&m->nlearn);
    for (int i = 0; i < n; i++) {
        learn_t *l = &m->learn[i];
        fprintf(f, "0x%02X %d %s,%s %s\n", l->key >> 8, l->key & 0xff, l->group, l->name, lm_name(l->mode));
    }
    fclose(f);
    rx_log("midi: saved %d learned mappings to %s", n, path);
}

static void learn_load(rx_midi_t *m) {
    char path[RX_PATH + 32];
    snprintf(path, sizeof path, "%s/midi_learn.txt", m->plugin_dir);
    FILE *f = fopen(path, "r");
    if (!f) { rx_log("midi: no %s (nothing learned yet)", path); return; }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < MAX_LEARN) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned st, d1;
        char gn[120], mode[16];
        if (sscanf(line, "%i %u %119s %15s", &st, &d1, gn, mode) != 4) continue;
        char *comma = strchr(gn, ',');
        if (!comma) continue;
        *comma = 0;
        ctl_id id = ctl_lookup(gn, comma + 1);
        if (id == CTL_NONE) { rx_log("midi: learn file: unknown control %s,%s skipped", gn, comma + 1); continue; }
        learn_t *l = &m->learn[n];
        l->key = (uint16_t)((st << 8) | (d1 & 0xff));
        l->ctl = id;
        l->mode = !strcmp(mode, "note") ? LM_NOTE : !strcmp(mode, "rel") ? LM_REL : !strcmp(mode, "bend") ? LM_BEND : LM_ABS;
        l->nseen = 8;                                        /* loaded: the mode is final, no more detection */
        snprintf(l->group, sizeof l->group, "%s", gn);
        snprintf(l->name, sizeof l->name, "%s", comma + 1);
        n++;
    }
    fclose(f);
    atomic_store(&m->nlearn, n);
    rx_log("midi: loaded %d learned mappings from %s", n, path);
}

int midi_learn_arm(rx_midi_t *m, ctl_id target) {
    if (!m) return -1;
    atomic_store(&m->learn_target, target);
    if (target != CTL_NONE) rx_log("midi: learn armed for %s,%s: move a control on the MIDI device", ctl_group(target), ctl_name(target));
    else rx_log("midi: learn disarmed");
    return 0;
}

int midi_learn_clear(rx_midi_t *m, ctl_id target) {
    if (!m) return -1;
    int n = atomic_load(&m->nlearn), w = 0, removed = 0;
    atomic_store(&m->nlearn, 0);                               /* midi_in sees an empty table while we compact */
    for (int i = 0; i < n; i++) {
        if (target == CTL_NONE || m->learn[i].ctl == target) { removed++; continue; }
        if (w != i) m->learn[w] = m->learn[i];
        w++;
    }
    atomic_store(&m->nlearn, w);
    learn_save(m);
    rx_log("midi: learn cleared %d mapping(s)", removed);
    return removed;
}

/* ------------------------------------------------------------------------------------------------ mapping files */
static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

static void scan_mappings(rx_midi_t *m) {
    char dir[RX_PATH + 16];
    snprintf(dir, sizeof dir, "%s/Mappings", m->plugin_dir);
    pthread_mutex_lock(&m->files_lock);
    m->nfiles = 0;
    DIR *d = opendir(dir);
    if (!d) { pthread_mutex_unlock(&m->files_lock); rx_log("midi: no %s folder: no controller mappings", dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) && m->nfiles < MAX_MAPPING_FILES) {
        if (!ends_with(e->d_name, ".midi.xml")) continue;
        snprintf(m->files[m->nfiles++], RX_PATH, "%s/%s", dir, e->d_name);
    }
    closedir(d);
    /* sorted, so the list is stable on the screen */
    for (int i = 1; i < m->nfiles; i++)
        for (int k = i; k > 0 && strcasecmp(m->files[k - 1], m->files[k]) > 0; k--) {
            char t[RX_PATH]; memcpy(t, m->files[k], RX_PATH); memcpy(m->files[k], m->files[k - 1], RX_PATH); memcpy(m->files[k - 1], t, RX_PATH);
        }
    pthread_mutex_unlock(&m->files_lock);
    rx_log("midi: %d mapping file(s) in %s", m->nfiles, dir);
    for (int i = 0; i < m->nfiles && i < 40; i++) rx_log("midi:   %s", strrchr(m->files[i], '/') + 1);
}

int midi_mapping_count(rx_midi_t *m) {
    if (!m) return 0;
    if (atomic_exchange(&m->learn_dirty, 0)) learn_save(m);
    return m->nfiles;
}

const char *midi_mapping_file(rx_midi_t *m, int i) { return (m && i >= 0 && i < m->nfiles) ? m->files[i] : NULL; }

const char *midi_mapping_name(rx_midi_t *m) {
    mapping_t *map = m ? atomic_load(&m->map) : NULL;
    return map ? map->name : "";
}

static uint16_t midi_key(uint8_t status, uint8_t d1) {
    int op = status & 0xf0;
    int two = op == 0x80 || op == 0x90 || op == 0xA0 || op == 0xB0 || status == 0xF3;    /* Mixxx isMessageTwoBytes */
    return (uint16_t)((status << 8) | (two ? d1 : 0xFF));
}

static uint32_t parse_options(xml_node_t *control) {
    uint32_t o = 0;
    xml_node_t *opts = xml_child(control, "options");
    for (xml_node_t *c = opts ? opts->first_child : NULL; c; c = c->next) {
        const char *n = c->name;
        if (!strcasecmp(n, "invert")) o |= OPT_INVERT;
        else if (!strcasecmp(n, "rot64")) o |= OPT_ROT64;
        else if (!strcasecmp(n, "rot64inv")) o |= OPT_ROT64INV;
        else if (!strcasecmp(n, "rot64fast")) o |= OPT_ROT64FAST;
        else if (!strcasecmp(n, "diff")) o |= OPT_DIFF;
        else if (!strcasecmp(n, "button")) o |= OPT_BUTTON;
        else if (!strcasecmp(n, "switch")) o |= OPT_SWITCH;
        else if (!strcasecmp(n, "hercjog")) o |= OPT_HERCJOG;
        else if (!strcasecmp(n, "hercjogfast")) o |= OPT_HERCJOGFAST;
        else if (!strcasecmp(n, "spread64")) o |= OPT_SPREAD64;
        else if (!strcasecmp(n, "selectknob")) o |= OPT_SELECTKNOB;
        else if (!strcasecmp(n, "soft-takeover")) o |= OPT_SOFT_TAKEOVER;
        else if (!strcasecmp(n, "script-binding")) o |= OPT_SCRIPT;
        else if (!strcasecmp(n, "fourteen-bit-msb")) o |= OPT_14BIT_MSB;
        else if (!strcasecmp(n, "fourteen-bit-lsb")) o |= OPT_14BIT_LSB;
        /* "normal" and unknown names: no option */
    }
    return o;
}

static void output_listener(ctl_id id, double value, void *user);

static void free_mapping(rx_midi_t *m, mapping_t *map) {
    (void)m;
    if (!map) return;
    for (int i = 0; i < map->nout; i++) if (map->out[i].listener >= 0) ctl_remove_listener(map->out[i].listener);
    free(map);
}

int midi_load_mapping(rx_midi_t *m, const char *xml_path) {
    if (!m || !xml_path) return -1;
    char err[128];
    xml_node_t *root = xml_parse_file(xml_path, err, sizeof err);
    if (!root) { rx_log("midi: %s: %s", xml_path, err); return -1; }
    xml_node_t *controller = xml_child(root, "controller");
    if (!controller) { rx_log("midi: %s: no <controller> element", xml_path); xml_free(root); return -1; }

    mapping_t *map = calloc(1, sizeof *map);
    if (!map) { xml_free(root); return -1; }
    snprintf(map->file, sizeof map->file, "%s", xml_path);
    const char *cid = xml_attr(controller, "id");
    snprintf(map->controller_id, sizeof map->controller_id, "%s", cid ? cid : "");
    xml_node_t *info = xml_child(root, "info");
    const char *nm = info ? xml_child_text(info, "name") : "";
    if (!nm[0]) { nm = strrchr(xml_path, '/'); nm = nm ? nm + 1 : xml_path; }
    snprintf(map->name, sizeof map->name, "%s", nm);

    /* <controls><control> */
    int unknown = 0, scripts = 0;
    char unk_list[2048]; unk_list[0] = 0; size_t unk_len = 0;
    xml_node_t *controls = xml_child(controller, "controls");
    for (xml_node_t *c = controls ? xml_child(controls, "control") : NULL; c; c = xml_next(c, "control")) {
        if (map->nin >= MAX_INPUTS) { rx_log("midi: more than %d controls, the rest is ignored", MAX_INPUTS); break; }
        in_map_t *im = &map->in[map->nin];
        int status = xml_text_int(c, "status", 0), midino = xml_text_int(c, "midino", 0);
        im->key = midi_key((uint8_t)status, (uint8_t)midino);
        im->options = parse_options(c);
        snprintf(im->group, sizeof im->group, "%s", xml_child_text(c, "group"));
        snprintf(im->name, sizeof im->name, "%s", xml_child_text(c, "key"));
        if (im->options & OPT_SCRIPT) { im->ctl = CTL_NONE; scripts++; }
        else {
            im->ctl = ctl_lookup(im->group, im->name);
            if (im->ctl == CTL_NONE) {
                unknown++;
                if (unk_len < sizeof unk_list - 100) { int w = snprintf(unk_list + unk_len, sizeof unk_list - unk_len, "%s%s,%s", unk_len ? "\n" : "", im->group, im->name); if (w > 0) unk_len += (size_t)w; }
              }
            if (im->ctl == CTL_NONE) { if (unknown <= 40) rx_log("midi:   control %s,%s (status 0x%02X midino 0x%02X) does not exist here: ignored", im->group, im->name, status, midino); }
        }
        map->nin++;
    }
    /* <outputs><output> */
    int bad_out = 0;
    xml_node_t *outputs = xml_child(controller, "outputs");
    for (xml_node_t *o = outputs ? xml_child(outputs, "output") : NULL; o; o = xml_next(o, "output")) {
        if (map->nout >= MAX_OUTPUTS) break;
        out_map_t *om = &map->out[map->nout];
        om->ctl = ctl_lookup(xml_child_text(o, "group"), xml_child_text(o, "key"));
        om->listener = -1;
        if (om->ctl == CTL_NONE) { bad_out++; continue; }
        om->status = (uint8_t)xml_text_int(o, "status", 0);
        om->data1 = (uint8_t)xml_text_int(o, "midino", 0);
        om->on = (uint8_t)xml_text_int(o, "on", 0x7F);
        om->off = (uint8_t)xml_text_int(o, "off", 0x00);
        const char *mn = xml_child_text(o, "minimum"), *mx = xml_child_text(o, "maximum");
        om->min = mn[0] ? atof(mn) : 0.0;
        om->max = mx[0] ? atof(mx) : 1.0;
        atomic_store(&om->last, -1);
        om->owner = m;
        map->nout++;
    }
    /* <scriptfiles><file filename= functionprefix=> */
    char dir[RX_PATH];
    snprintf(dir, sizeof dir, "%s", xml_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    char script_files[MAX_SCRIPTS][RX_PATH];
    int nscript = 0;
    xml_node_t *sf = xml_child(controller, "scriptfiles");
    for (xml_node_t *f = sf ? xml_child(sf, "file") : NULL; f && nscript < MAX_SCRIPTS; f = xml_next(f, "file")) {
        const char *fn = xml_attr(f, "filename"), *pre = xml_attr(f, "functionprefix");
        if (!fn || !fn[0]) continue;
        snprintf(script_files[nscript], RX_PATH, "%s/%s", dir, fn);
        if (pre && pre[0] && map->nprefix < MAX_SCRIPTS) snprintf(map->prefixes[map->nprefix++], 32, "%s", pre);
        nscript++;
    }
    xml_free(root);

    /* scripts: common-controller-scripts.js first (Mixxx loads it for every mapping), then the mapping's files */
    if (m->js) {
        js_unload(m->js);
        char common[RX_PATH + 32];
        snprintf(common, sizeof common, "%s/common-controller-scripts.js", dir);
        if (access(common, R_OK) == 0) js_load_file(m->js, common);
        else rx_log("midi: %s missing: script.* helpers will be undefined", common);
        for (int i = 0; i < nscript; i++) {
            int already = ends_with(script_files[i], "common-controller-scripts.js");
            if (!already) js_load_file(m->js, script_files[i]);
        }
    } else if (nscript) rx_log("midi: no JS runtime: %d script file(s) and %d script bindings are inert", nscript, scripts);

    /* swap the tables in; the old mapping is freed after a grace period (midi_in may be reading it right now) */
    mapping_t *old = atomic_exchange(&m->map, map);
    if (m->old_map) { free_mapping(m, m->old_map); m->old_map = NULL; }
    if (old) {
        for (int i = 0; i < old->nout; i++) if (old->out[i].listener >= 0) { ctl_remove_listener(old->out[i].listener); old->out[i].listener = -1; }
        m->old_map = old;
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);                       /* a few audio blocks: any midi_in still inside it has returned */
        free_mapping(m, m->old_map);
        m->old_map = NULL;
    }
    /* soft takeover on the controls that ask for it; output listeners; init() */
    for (int i = 0; i < map->nin; i++) if ((map->in[i].options & OPT_SOFT_TAKEOVER) && map->in[i].ctl != CTL_NONE) js_st_enable(m->js, map->in[i].ctl, 1);
    for (int i = 0; i < map->nout; i++) {
        out_map_t *om = &map->out[i];
        om->listener = ctl_add_listener(om->ctl, output_listener, om);
        output_listener(om->ctl, ctl_get(om->ctl), om);     /* Mixxx's updateAllOutputs */
    }
    if (m->js) for (int i = 0; i < map->nprefix; i++) js_call_init(m->js, map->prefixes[i], map->controller_id);
    pthread_mutex_lock(&m->rep_lock);
    snprintf(m->rep_file, sizeof m->rep_file, "%s", xml_path);
    snprintf(m->rep_name, sizeof m->rep_name, "%s", map->name);
    m->rep_in = map->nin; m->rep_script = scripts; m->rep_unknown = unknown; m->rep_out = map->nout; m->rep_out_unknown = bad_out;
    snprintf(m->rep_unknown_list, sizeof m->rep_unknown_list, "%s", unk_list);
    pthread_mutex_unlock(&m->rep_lock);
    rx_log("midi: mapping \"%s\" loaded: %d controls (%d script bindings, %d unknown here), %d outputs (%d unknown), %d script file(s)",
           map->name, map->nin, scripts, unknown, map->nout, bad_out, nscript);
    return 0;
}


/* ------------------------------------------------------------------------------------------------ the CONTROLLERS page and the web map builder
 * Everything below runs on the engine's worker thread or the web thread, never the audio thread (except the two try-lock readers). */
void midi_rescan(rx_midi_t *m) { if (m) scan_mappings(m); }

/* the list entry i: its path / its shown name (file name without ".midi.xml"); -1 when the list is being rebuilt */
int midi_mapping_path_at(rx_midi_t *m, int i, char *out, int n) {
    if (!m || n < 1 || pthread_mutex_trylock(&m->files_lock)) return -1;
    int ok = i >= 0 && i < m->nfiles;
    if (ok) snprintf(out, (size_t)n, "%s", m->files[i]); else out[0] = 0;
    pthread_mutex_unlock(&m->files_lock);
    return ok ? 0 : -1;
}
int midi_mapping_name_at(rx_midi_t *m, int i, char *out, int n) {
    char path[RX_PATH];
    if (midi_mapping_path_at(m, i, path, sizeof path)) { if (n > 0) out[0] = 0; return -1; }
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    snprintf(out, (size_t)n, "%s", b);
    size_t l = strlen(out), e = strlen(".midi.xml");
    if (l > e && !strcasecmp(out + l - e, ".midi.xml")) out[l - e] = 0;
    return 0;
}
int midi_mapping_total(rx_midi_t *m) {
    if (!m || pthread_mutex_trylock(&m->files_lock)) return -1;
    int n = m->nfiles;
    pthread_mutex_unlock(&m->files_lock);
    return n;
}

/* the last note / CC / pitch-bend message: returns a counter that grows with each one */
int midi_last_msg(rx_midi_t *m, int *st, int *d1, int *d2) {
    if (!m) return 0;
    *st = atomic_load(&m->last_st); *d1 = atomic_load(&m->last_d1); *d2 = atomic_load(&m->last_d2);
    return atomic_load(&m->last_seq);
}

/* what the loaded mapping is: name, counts, and the controls it names that this plugin does not have ("[Group],key" lines) */
int midi_report(rx_midi_t *m, char *file, int fn, char *name, int nn, int *nin, int *nscript, int *nunknown, char *unknown_list, int un, int try_only) {
    if (!m) return 0;
    if (try_only) { if (pthread_mutex_trylock(&m->rep_lock)) return -1; } else pthread_mutex_lock(&m->rep_lock);
    int ok = m->rep_file[0] != 0;
    if (file) snprintf(file, (size_t)fn, "%s", m->rep_file);
    if (name) snprintf(name, (size_t)nn, "%s", m->rep_name);
    if (nin) *nin = m->rep_in;
    if (nscript) *nscript = m->rep_script;
    if (nunknown) *nunknown = m->rep_unknown;
    if (unknown_list) snprintf(unknown_list, (size_t)un, "%s", m->rep_unknown_list);
    pthread_mutex_unlock(&m->rep_lock);
    return ok;
}

int midi_unload_mapping(rx_midi_t *m) {
    if (!m) return -1;
    if (m->js) js_unload(m->js);
    mapping_t *old = atomic_exchange(&m->map, NULL);
    if (old) {
        for (int i = 0; i < old->nout; i++) if (old->out[i].listener >= 0) { ctl_remove_listener(old->out[i].listener); old->out[i].listener = -1; }
        m->old_map = old;
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);
        free_mapping(m, m->old_map);
        m->old_map = NULL;
    }
    pthread_mutex_lock(&m->rep_lock);
    m->rep_file[0] = 0; m->rep_name[0] = 0; m->rep_in = m->rep_script = m->rep_unknown = 0; m->rep_unknown_list[0] = 0;
    pthread_mutex_unlock(&m->rep_lock);
    rx_log("midi: mapping unloaded");
    return 0;
}

/* the picked controller is remembered in <plugin>/midi_pick.txt (the mapping's file name) and loaded again at the next start */
static void pick_save(rx_midi_t *m, const char *path) {
    char p[RX_PATH + 24]; snprintf(p, sizeof p, "%s/midi_pick.txt", m->plugin_dir);
    if (!path || !path[0]) { unlink(p); return; }
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "%s\n", b); fclose(f);
}
int midi_pick_mapping(rx_midi_t *m, const char *path) {
    int r = midi_load_mapping(m, path);
    if (r == 0) pick_save(m, path);
    return r;
}
int midi_pick_none(rx_midi_t *m) { int r = midi_unload_mapping(m); pick_save(m, NULL); return r; }
void midi_autoload(rx_midi_t *m) {
    if (!m) return;
    char p[RX_PATH + 24], line[512]; snprintf(p, sizeof p, "%s/midi_pick.txt", m->plugin_dir);
    FILE *f = fopen(p, "r"); if (!f) return;
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || strchr(line, '/')) return;
    char path[RX_PATH]; snprintf(path, sizeof path, "%s/Mappings/%s", m->plugin_dir, line);
    if (access(path, R_OK) == 0) { rx_log("midi: loading the remembered controller %s", line); midi_load_mapping(m, path); }
}

/* write a map from the web builder into Mappings/<name>.midi.xml: the name keeps letters, digits, space . _ - only */
int midi_save_map(rx_midi_t *m, const char *name, const char *data, size_t len, char *saved, int sn) {
    if (!m || !name || !data || len < 20 || len > 262144) return -1;
    if (!strstr(data, "<MixxxMIDIPreset")) return -2;
    char clean[96]; int k = 0;
    for (const char *c = name; *c && k < 80; c++) if (isalnum((unsigned char)*c) || *c == ' ' || *c == '.' || *c == '_' || *c == '-') clean[k++] = *c;
    clean[k] = 0;
    while (clean[0] == '.' || clean[0] == ' ') memmove(clean, clean + 1, strlen(clean));
    size_t cl = strlen(clean), e = strlen(".midi.xml");
    if (cl > e && !strcasecmp(clean + cl - e, ".midi.xml")) clean[cl - e] = 0;
    if (!clean[0]) return -3;
    char dir[RX_PATH + 16], path[RX_PATH + 120], tmp[RX_PATH + 130];
    snprintf(dir, sizeof dir, "%s/Mappings", m->plugin_dir);
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/%s.midi.xml", dir, clean);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -4;
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { unlink(tmp); return -4; }
    if (rename(tmp, path)) { unlink(tmp); return -4; }
    if (saved) snprintf(saved, (size_t)sn, "%s", path);
    rx_log("midi: map builder saved %s (%zu bytes)", path, len);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ output feedback */
static void output_listener(ctl_id id, double value, void *user) {
    /* AUDIO thread (render -> listeners), or the loader at load time: compute the byte, enqueue if it changed */
    (void)id;
    out_map_t *om = (out_map_t *)user;
    int byte3 = (value >= om->min && value <= om->max) ? om->on : om->off;
    if (atomic_exchange(&om->last, byte3) == byte3) return;
    if (byte3 != 0xFF) fb_push(om->owner, om->status, om->data1, (uint8_t)byte3);
}

/* ------------------------------------------------------------------------------------------------ midi_in */
static double midi_param_of(ctl_id id) {           /* the control's current value as a 0..127 MIDI value */
    double lo = ctl_min(id), hi = ctl_max(id);
    if (hi - lo == 0) return 0;
    double p = (ctl_get(id) - lo) / (hi - lo);
    return (p < 0 ? 0 : p > 1 ? 1 : p) * 127.0;
}

/* Mixxx MidiController::computeValue; returns the new MIDI value, or sets *delta for the relative options that
 * Mixxx applies on top of the current value (we post those as deltas so jog ticks accumulate) */
static double compute_value(uint32_t o, double prev, double v, int *is_delta) {
    *is_delta = 0;
    if (!o) return v;
    if (o & OPT_INVERT) v = 127.0 - v;
    if (o & (OPT_ROT64 | OPT_ROT64INV)) {
        double diff = v - 64.0;
        if (diff == -1 || diff == 1) diff /= 16; else diff += (diff > 0 ? -1 : +1);
        double t = (o & OPT_ROT64) ? prev + diff : prev - diff;
        return t < 0 ? 0 : t > 127 ? 127 : t;
    }
    if (o & OPT_ROT64FAST) { double t = prev + (v - 64.0) * 1.5; return t < 0 ? 0 : t > 127 ? 127 : t; }
    if (o & OPT_DIFF) { if (v >= 64) v -= 128; return prev + v; }
    if (o & OPT_SELECTKNOB) { if (v >= 64) v -= 128; *is_delta = 1; return v; }
    if (o & OPT_BUTTON) return v != 0;
    if (o & OPT_SWITCH) return 1;
    if (o & OPT_SPREAD64) { *is_delta = 1; return v - 64.0; }
    if (o & OPT_HERCJOG) { if (v > 64) v -= 128; return prev + v; }
    if (o & OPT_HERCJOGFAST) { if (v > 64) v -= 128; return prev + v * 3; }
    return v;
}

/* post a MIDI-scale value (0..127, or a signed delta) to a control, by its kind, with note semantics for buttons */
static void apply_to_control(rx_midi_t *m, ctl_id id, uint32_t o, int opcode, double mv, int is_delta, int raw_value) {
    int kind = ctl_kind(id);
    double lo = ctl_min(id), hi = ctl_max(id);
    int note_like = opcode == 0x80 || opcode == 0x90 || (o & (OPT_BUTTON | OPT_SWITCH));
    if (note_like) {
        int pressed = (o & OPT_SWITCH) ? 1 : (o & OPT_BUTTON) ? mv != 0 : !(opcode == 0x80 || raw_value == 0);
        if (kind == CK_TOGGLE) { if (pressed) ctl_post(id, ctl_get(id) > 0.5 ? 0 : 1); }
        else if (kind == CK_TRIGGER) ctl_post(id, pressed ? 1 : 0);
        else ctl_post(id, pressed ? hi : lo);           /* a continuous control on a button: full/none (rate_temp_up) */
        return;
    }
    if (is_delta) {
        if (kind == CK_TOGGLE || kind == CK_TRIGGER) { if (mv != 0) ctl_post(id, 1); }
        else ctl_post_delta(id, mv);                    /* jog / SelectTrackKnob: raw signed amount, like Mixxx's value */
        return;
    }
    if (o & OPT_SOFT_TAKEOVER) { if (js_st_check(m->js, id, mv / 127.0)) return; }
    if (mv < 0) mv = 0;
    if (mv > 127) mv = 127;
    double v = lo + (mv / 127.0) * (hi - lo);
    if (kind == CK_FLOAT && lo == 0.0 && hi == 4.0) { double p = mv / 127.0; v = p < 0.5 ? p * 2 : 1 + (p - 0.5) * 6; }   /* Mixxx EQ / gain pots: 64 = unity (1.0), 127 = 4.0 */
    if (kind == CK_INT) v = floor(v + 0.5);
    if (kind == CK_TOGGLE) v = mv >= 64 ? 1 : 0;
    if (kind == CK_TRIGGER) { if (mv != 0) ctl_post(id, 1); return; }
    ctl_post(id, v);
}

static void handle_entry(rx_midi_t *m, mapping_t *map, in_map_t *im, uint8_t status, uint8_t d1, uint8_t d2) {
    int opcode = status & 0xf0, channel = status & 0x0f;
    if (im->options & OPT_SCRIPT) {
        if (js_post_binding(m->js, im->name, channel, d1, d2, status, im->group) < 0) {
            if (atomic_fetch_add(&m->dropped_logged, 1) < 1) js_log(m->js, "midi: script binding ring full, messages dropped");
        }
        return;
    }
    if (im->ctl == CTL_NONE) return;
    ctl_id id = im->ctl;
    double mv;
    int is_delta = 0;
    if (im->options & (OPT_14BIT_MSB | OPT_14BIT_LSB)) {
        /* pair with the other half; keep the first half until its partner arrives */
        int is_msb = (im->options & OPT_14BIT_MSB) != 0, found = -1;
        for (int i = 0; i < map->npend14; i++) if (map->pend14[i].have && map->pend14[i].ctl == id) { found = i; break; }
        if (found >= 0 && map->pend14[found].is_msb != is_msb) {
            int msb = is_msb ? d2 : map->pend14[found].value, lsb = is_msb ? map->pend14[found].value : d2;
            map->pend14[found].have = 0;
            mv = ((msb << 7) | lsb) / 128.0;
            if (mv > 127) mv = 127;
        } else {
            int slot = found >= 0 ? found : -1;
            if (slot < 0) for (int i = 0; i < 16; i++) if (!map->pend14[i].have) { slot = i; break; }
            if (slot < 0) return;
            map->pend14[slot].have = 1; map->pend14[slot].ctl = id; map->pend14[slot].value = d2; map->pend14[slot].is_msb = is_msb;
            if (slot >= map->npend14) map->npend14 = slot + 1;
            return;
        }
    } else if (opcode == 0xE0) {
        mv = ((d2 << 7) | d1) / 128.0;
        if (mv > 127) mv = 127;
    } else {
        mv = compute_value(im->options, midi_param_of(id), opcode == 0xC0 || opcode == 0xD0 ? d1 : d2, &is_delta);
    }
    apply_to_control(m, id, im->options, opcode, mv, is_delta, d2);
}

static int looks_like_encoder(int v) { return (v >= 1 && v <= 7) || (v >= 121 && v <= 127) || v == 63 || v == 65; }

static void handle_learned(rx_midi_t *m, learn_t *l, uint8_t status, uint8_t d1, uint8_t d2) {
    int opcode = status & 0xf0;
    ctl_id id = l->ctl;
    int kind = ctl_kind(id);
    double lo = ctl_min(id), hi = ctl_max(id);
    if (l->mode == LM_NOTE) { apply_to_control(m, id, 0, opcode, d2, 0, d2); return; }
    if (l->mode == LM_BEND) { double mv = ((d2 << 7) | d1) / 128.0; apply_to_control(m, id, 0, opcode, mv > 127 ? 127 : mv, 0, d2); return; }
    /* CC: encoder detection during the first values */
    if (l->nseen < 6) {
        if (l->nseen == 0) l->looks_rel = 1;
        if (!looks_like_encoder(d2)) l->looks_rel = 0;
        l->nseen++;
        if (l->nseen == 6 && l->looks_rel && (kind == CK_FLOAT || kind == CK_INT)) { l->mode = LM_REL; atomic_store(&m->learn_dirty, 1); js_log(m->js, "midi: learn: %s,%s looks like an encoder, mode rel", l->group, l->name); }
    }
    if (l->mode == LM_REL) {
        int d = abs(d2 - 64) <= 7 ? d2 - 64 : (d2 >= 64 ? d2 - 128 : d2);     /* 63/65 style, else two's complement */
        double step = (hi - lo) / 100.0;                                       /* 100 detents across the range */
        if (kind == CK_INT) step = 1;
        ctl_post_delta(id, d * step);
    } else apply_to_control(m, id, 0, opcode, d2, 0, d2);
}

void midi_in(rx_midi_t *m, const uint8_t *msg, int len) {
    /* AUDIO THREAD: no blocking, no logging (js_log defers), no allocation */
    if (!m || len < 1) return;
    uint8_t status = msg[0];
    if (status >= 0xF8) return;                       /* realtime (clock, active sensing): not mappable, like Mixxx */
    if (status == 0xF0) return;                       /* sysex: not supported */
    uint8_t d1 = len > 1 ? msg[1] : 0, d2 = len > 2 ? msg[2] : 0;
    int opcode = status & 0xf0;
    if (opcode < 0x80) return;
    uint16_t key = midi_key(status, d1);
    if (opcode == 0xB0 || opcode == 0xE0 || (opcode == 0x90 && d2 > 0)) {   /* for the web map builder: a release must not hide the press */
        atomic_store(&m->last_st, status); atomic_store(&m->last_d1, d1); atomic_store(&m->last_d2, d2);
        atomic_fetch_add(&m->last_seq, 1);
    }

    /* learn: the armed target takes the next note/CC/bend message */
    int target = atomic_load(&m->learn_target);
    if (target != CTL_NONE && (opcode == 0x80 || opcode == 0x90 || opcode == 0xB0 || opcode == 0xE0)) {
        if (!(opcode == 0x80 || (opcode == 0x90 && d2 == 0))) {    /* a release alone does not learn */
            int n = atomic_load(&m->nlearn);
            int slot = -1;
            for (int i = 0; i < n; i++) if (m->learn[i].ctl == target || m->learn[i].key == key) { slot = i; break; }
            if (slot < 0 && n < MAX_LEARN) slot = n;
            if (slot >= 0) {
                learn_t *l = &m->learn[slot];
                l->key = key; l->ctl = target;
                l->mode = opcode == 0x90 ? LM_NOTE : opcode == 0xE0 ? LM_BEND : LM_ABS;
                l->nseen = 0; l->looks_rel = 0;
                snprintf(l->group, sizeof l->group, "%s", ctl_group(target));
                snprintf(l->name, sizeof l->name, "%s", ctl_name(target));
                if (slot == n) atomic_store(&m->nlearn, n + 1);
                atomic_store(&m->learn_target, CTL_NONE);
                atomic_store(&m->learn_dirty, 1);
                js_log(m->js, "midi: learned 0x%02X %d -> %s,%s (%s)", status, d1, l->group, l->name, lm_name(l->mode));
            }
            return;
        }
    }

    /* learned entries first (they override the mapping's for the same key), then the mapping's (all matches) */
    int n = atomic_load(&m->nlearn), hit = 0;
    for (int i = 0; i < n; i++) if (m->learn[i].key == key) { handle_learned(m, &m->learn[i], status, d1, d2); hit = 1; }
    if (hit) return;
    mapping_t *map = atomic_load(&m->map);
    int matched = 0;
    if (map) for (int i = 0; i < map->nin; i++) if (map->in[i].key == key) { matched = 1; handle_entry(m, map, &map->in[i], status, d1, d2); }
    if (!matched && !(m->unseen[key >> 3] & (1u << (key & 7)))) {      /* a control of the hardware nothing is mapped to: say so once in the log */
        m->unseen[key >> 3] |= (uint8_t)(1u << (key & 7));
        unsigned w = atomic_load(&m->un_w);
        if (w - atomic_load(&m->un_r) < 32) { atomic_store(&m->un_q[w & 31], (int)key); atomic_store(&m->un_w, w + 1); }
    }
}

void midi_tick(rx_midi_t *m) {
    /* AUDIO THREAD, once per block. Nothing to do: scripts, timers and the ALSA output all run on the JS worker.
     * Kept so engine.c has a hook if a per-block job turns up (e.g. MIDI clock out). */
    (void)m;
}

/* ------------------------------------------------------------------------------------------------ worker idle hook */
static void hook_idle(void *user) {
    rx_midi_t *m = (rx_midi_t *)user;
    fb_drain(m);
    while (atomic_load(&m->un_r) != atomic_load(&m->un_w)) {
        unsigned r = atomic_load(&m->un_r); int k = atomic_load(&m->un_q[r & 31]); atomic_store(&m->un_r, r + 1);
        static int nlog; if (++nlog > 300) continue;
        mapping_t *mp = atomic_load(&m->map);
        rx_log("midi: message 0x%02X %d (channel %d) has no binding%s", k >> 8, k & 0xFF, (k >> 8 & 15) + 1, mp ? " in the loaded map" : " (no map loaded: pick one on the MAPS page)");
    }
    if (atomic_exchange(&m->learn_dirty, 0)) learn_save(m);
    int d = js_dropped_messages(m->js);
    if (d) rx_log("midi: %d script-binding messages dropped (JS worker behind)", d);
    static double last_stat;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (now - last_stat > 10) {
        last_stat = now;
        int sent = atomic_exchange(&m->fb_sent, 0), unrouted = atomic_exchange(&m->fb_unrouted, 0), dropped = atomic_exchange(&m->fb_dropped, 0);
        if (sent || unrouted || dropped) rx_log("midi: feedback last 10 s: %d sent to ALSA, %d unrouted (no ALSA), %d dropped (ring full)", sent, unrouted, dropped);
    }
}

/* ------------------------------------------------------------------------------------------------ start / stop */
rx_midi_t *midi_start(const char *plugin_dir, rx_engine_t *e) {
    rx_midi_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->plugin_dir, sizeof m->plugin_dir, "%s", plugin_dir ? plugin_dir : ".");
    m->engine = e;
    atomic_store(&m->learn_target, CTL_NONE);
    pthread_mutex_init(&m->files_lock, NULL);
    pthread_mutex_init(&m->rep_lock, NULL);
    alsa_open(m);
    rx_js_hooks_t hooks = { hook_send, hook_idle, m };
    m->js = js_start(e, &hooks);
    if (!m->js) rx_log("midi: JS runtime failed to start: script mappings and LED feedback are off, MIDI learn and plain XML controls still work");
    learn_load(m);
    scan_mappings(m);
    rx_log("midi: started (learn file %s/midi_learn.txt)", m->plugin_dir);
    return m;
}

void midi_stop(rx_midi_t *m) {
    if (!m) return;
    if (atomic_load(&m->learn_dirty)) learn_save(m);
    mapping_t *map = atomic_exchange(&m->map, NULL);
    if (m->js) js_stop(m->js);                      /* shutdown() scripts, join the worker (no more idle hooks) */
    m->js = NULL;
    free_mapping(m, map);
    free_mapping(m, m->old_map);
    if (m->alsa_ok) { fb_drain(m); a_close(m->seq); }
    rx_log("midi: stopped; feedback since the last report: %d sent to ALSA, %d unrouted, %d dropped",
           atomic_load(&m->fb_sent), atomic_load(&m->fb_unrouted), atomic_load(&m->fb_dropped));
    if (m->asound) dlclose(m->asound);
    free(m);
}
