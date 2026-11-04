/* reMixXXL -- QuickJS binding for Mixxx controller scripts: Mixxx's `engine`, `midi`, `console`/`print` script API,
 * one JS worker thread, lock-free rings in from the audio thread. See midi_js.h for the threading contract.
 *
 * Implemented script API (what Mixxx's legacy ControllerScriptInterfaceLegacy exposes, the parts mappings use):
 *   engine.getValue(group, name)                 engine.setValue(group, name, value)
 *   engine.getParameter(group, name)             engine.setParameter(group, name, 0..1)
 *   engine.getParameterForValue(group,name,v)    engine.reset(group, name)  (-> the control's default: min for
 *                                                                              lack of a default in the ctl API)
 *   engine.getDefaultValue / getDefaultParameter (same caveat: the ctl API has no default; returns min / 0)
 *   engine.makeConnection(group, name, fn)       -> connection {disconnect(), trigger(), isConnected, group, name}
 *   engine.makeUnbufferedConnection              (same as makeConnection here)
 *   engine.connectControl(group, name, fn|"code"[, disconnect])   (legacy; returns the connection, or true/false)
 *   engine.trigger(group, name)                  engine.log(msg)
 *   engine.beginTimer(ms, fn|"code", oneShot)    engine.stopTimer(id)
 *   engine.scratchEnable(deck, intervalsPerRev, rpm, alpha, beta[, ramp])   engine.scratchTick(deck, ticks)
 *   engine.scratchDisable(deck[, ramp])          engine.isScratching(deck)
 *   engine.softTakeover(group, name, on)         engine.softTakeoverIgnoreNextValue(group, name)
 *   engine.softTakeoverWillIgnore(group, name, parameter)
 *   engine.brake / spinback / softStart / isBrakeActive / isSpinbackActive / isSoftStartActive  (stubs: logged once,
 *                                                no-ops; the deck has no brake model yet)
 *   engine.getSetting(name) -> undefined         engine.getPlayer(group) -> undefined (QML-era API, not used by XML
 *                                                mappings)
 *   midi.sendShortMsg(status, d1, d2)            midi.sendSysexMsg(data, len)  (logged only: no sysex path to ALSA
 *                                                yet)   midi.send(array)  (3-byte arrays go out as a short message)
 *   console.log/info/warn/error/debug, print     -> the diag log
 *   script.*, Deck/Button/Control, ButtonState, LedState: plain JS from common-controller-scripts.js (midi.c loads
 *   it before the mapping's own script).
 *
 * Scratch note: the deck's deck_scratch_tick() takes wheel ticks at 720 per revolution (touch.c's convention, see
 * remix.h); scratchTick scales the script's ticks by 720/intervalsPerRev so a script's own resolution still means
 * one revolution per revolution. */
#define _GNU_SOURCE
#include "midi_js.h"
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <math.h>

#define JS_MEM_LIMIT (48u * 1024 * 1024)
#define JS_STACK_LIMIT (1024u * 1024)
#define JS_WORKER_PERIOD_US 2000          /* the worker's tick; script timers are rounded to it */
#define BIND_RING 256                     /* script bindings queued from the audio thread */
#define CONN_RING 1024                    /* control-change events queued from the audio thread */
#define LOG_RING 128
#define LOG_LINE 256
#define MAX_CONN 512
#define MAX_TIMERS 64
#define MAX_PREFIX 8
#define ST_MAX 4096                       /* soft-takeover slots (by ctl_id) */
#define ST_THRESHOLD (3.0 / 128)          /* Mixxx SoftTakeover::kDefaultTakeoverThreshold */
#define ST_OVERRIDE_MS 50                 /* Mixxx kSubsequentValueOverrideTime */

typedef struct { char fn[64]; char group[48]; int channel, control, value, status; } bind_ev_t;
typedef struct { int conn; double value; } conn_ev_t;

typedef struct {
    int active, gen;                     /* gen: bumps on reuse so stale ring events are dropped */
    ctl_id ctl;
    int listener;                        /* ctl_add_listener handle */
    JSValue callback;
    char group[48], name[64];
    char id[24];                         /* "conn-N", shown to scripts as .id */
    struct rx_js *owner;
} conn_t;

typedef struct {
    int active, oneshot;
    int id;
    double interval_ms, due;             /* due: monotonic ms */
    JSValue callback;
} timer_t_;

typedef struct {
    atomic_int enabled;
    atomic_int first;                    /* 1 = the next value is ignored (Mixxx's kFirstValueTime) */
    double prev;                         /* previous parameter received */
    double t_ms;                         /* when the last accepted value came */
} st_slot_t;

struct rx_js {
    JSRuntime *rt;
    JSContext *ctx;
    pthread_mutex_t lock;                /* the JS lock: ctx + conns + timers */
    pthread_t worker;
    atomic_int running;
    rx_engine_t *engine;
    rx_js_hooks_t hooks;

    bind_ev_t bind_ring[BIND_RING];      /* SPSC: audio thread -> worker */
    atomic_uint bind_w, bind_r;
    atomic_int dropped;

    conn_ev_t conn_ring[CONN_RING];      /* SPSC: audio thread (ctl listeners in render) -> worker */
    atomic_uint conn_w, conn_r;
    atomic_int conn_dropped;

    char log_ring[LOG_RING][LOG_LINE];   /* MPSC: any thread -> worker */
    atomic_int log_ready[LOG_RING];
    atomic_uint log_w, log_r;

    conn_t conns[MAX_CONN];
    int nconns;
    timer_t_ timers[MAX_TIMERS];
    int next_timer_id;
    char prefixes[MAX_PREFIX][32];       /* init'ed script prefixes, for shutdown() */
    int nprefix;
    char controller_id[64];

    st_slot_t st[ST_MAX];
    int scratch_on[RX_DECKS + 1];        /* isScratching fallback, 1-based deck */
    double scratch_scale[RX_DECKS + 1];  /* 720 / intervalsPerRev */
    int warned_stub;                     /* bit per stubbed function, log once */
    char warned_ctl[32][80];             /* unknown controls already reported */
    int nwarned_ctl;
};

/* QuickJS measures stack depth from the stack top recorded at JS_NewRuntime (the creating thread). Every thread
 * that runs JS (the worker, the loader) must refresh it after taking the JS lock, or the first call throws
 * "Maximum call stack size exceeded". */
static void js_enter(rx_js_t *j) { pthread_mutex_lock(&j->lock); JS_UpdateStackTop(j->rt); }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------------------------------------ deferred log */
void js_log(rx_js_t *j, const char *fmt, ...) {
    if (!j) return;
    unsigned w = atomic_fetch_add(&j->log_w, 1);
    unsigned r = atomic_load(&j->log_r);
    if (w - r >= LOG_RING) return;                       /* full: drop (the worker is behind or dead) */
    char *line = j->log_ring[w % LOG_RING];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, LOG_LINE, fmt, ap);
    va_end(ap);
    atomic_store(&j->log_ready[w % LOG_RING], 1);
}

static void log_drain(rx_js_t *j) {
    for (;;) {
        unsigned r = atomic_load(&j->log_r);
        if (r == atomic_load(&j->log_w)) break;
        if (!atomic_load(&j->log_ready[r % LOG_RING])) break;   /* a writer is still filling this slot */
        rx_log("js: %s", j->log_ring[r % LOG_RING]);
        atomic_store(&j->log_ready[r % LOG_RING], 0);
        atomic_store(&j->log_r, r + 1);
    }
    /* writers that found the ring full advanced log_w past what we read: resync so they are not lost forever */
    unsigned w = atomic_load(&j->log_w), r = atomic_load(&j->log_r);
    if (w - r >= LOG_RING) atomic_store(&j->log_r, w);
}

/* ------------------------------------------------------------------------------------------------ JS helpers */
static void js_report_exception(rx_js_t *j, const char *where) {
    JSValue exc = JS_GetException(j->ctx);
    const char *msg = JS_ToCString(j->ctx, exc);
    rx_log("js: %s: %s", where, msg ? msg : "(exception)");
    if (msg) JS_FreeCString(j->ctx, msg);
    if (JS_IsError(exc)) {
        JSValue stack = JS_GetPropertyStr(j->ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *s = JS_ToCString(j->ctx, stack);
            if (s && s[0]) rx_log("js:   %s", s);
            if (s) JS_FreeCString(j->ctx, s);
        }
        JS_FreeValue(j->ctx, stack);
    }
    JS_FreeValue(j->ctx, exc);
}

static rx_js_t *J(JSContext *ctx) { return (rx_js_t *)JS_GetContextOpaque(ctx); }

static int arg_str(JSContext *ctx, JSValueConst v, char *out, int n) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) { out[0] = 0; return 0; }
    snprintf(out, n, "%s", s);
    JS_FreeCString(ctx, s);
    return 1;
}

static double arg_num(JSContext *ctx, JSValueConst v, double def) {
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0 || isnan(d)) return def;
    return d;
}

/* look a control up by (group, name); report an unknown one once */
static ctl_id lookup_ctl(rx_js_t *j, const char *group, const char *name, const char *what) {
    ctl_id id = ctl_lookup(group, name);
    if (id == CTL_NONE) {
        char key[80];
        snprintf(key, sizeof key, "%s,%s", group, name);
        int seen = 0;
        for (int i = 0; i < j->nwarned_ctl; i++) if (!strcmp(j->warned_ctl[i], key)) { seen = 1; break; }
        if (!seen) {
            if (j->nwarned_ctl < 32) snprintf(j->warned_ctl[j->nwarned_ctl++], 80, "%s", key);
            rx_log("js: %s: no such control %s (ignored)", what, key);
        }
    }
    return id;
}

static double ctl_param(ctl_id id, double v) {          /* value -> normalised 0..1 */
    double lo = ctl_min(id), hi = ctl_max(id);
    if (hi - lo == 0) return 0;
    double p = (v - lo) / (hi - lo);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

/* resolve "a.b.c" on the global object; returns a new reference (or JS_UNDEFINED) */
static JSValue resolve_dotted(JSContext *ctx, const char *path) {
    JSValue cur = JS_GetGlobalObject(ctx);
    char buf[128];
    snprintf(buf, sizeof buf, "%s", path);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
        JSValue next = JS_GetPropertyStr(ctx, cur, tok);
        JS_FreeValue(ctx, cur);
        cur = next;
        if (JS_IsException(cur) || JS_IsUndefined(cur) || JS_IsNull(cur)) {
            if (JS_IsException(cur)) { JS_FreeValue(ctx, JS_GetException(ctx)); cur = JS_UNDEFINED; }
            return cur;
        }
    }
    return cur;
}

/* a callback given as a string ("mc3000.playSetLed" or any JS expression) becomes a function */
static JSValue callback_from_value(rx_js_t *j, JSValueConst v) {
    if (JS_IsFunction(j->ctx, v)) return JS_DupValue(j->ctx, v);
    if (JS_IsString(v)) {
        const char *code = JS_ToCString(j->ctx, v);
        if (!code) return JS_UNDEFINED;
        JSValue fn = resolve_dotted(j->ctx, code);
        if (!JS_IsFunction(j->ctx, fn)) {
            JS_FreeValue(j->ctx, fn);
            char wrapped[1024];
            snprintf(wrapped, sizeof wrapped, "(%s)", code);   /* Mixxx evaluates the string as an expression */
            fn = JS_Eval(j->ctx, wrapped, strlen(wrapped), "<callback>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(fn)) { js_report_exception(j, "callback string"); fn = JS_UNDEFINED; }
            else if (!JS_IsFunction(j->ctx, fn)) { JS_FreeValue(j->ctx, fn); fn = JS_UNDEFINED; }
        }
        JS_FreeCString(j->ctx, code);
        return fn;
    }
    return JS_UNDEFINED;
}

static void call_void(rx_js_t *j, JSValueConst fn, int argc, JSValueConst *argv, const char *where) {
    JSValue r = JS_Call(j->ctx, fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(r)) js_report_exception(j, where);
    JS_FreeValue(j->ctx, r);
}

/* ------------------------------------------------------------------------------------------------ connections */
static void conn_listener(ctl_id id, double value, void *user) {
    /* AUDIO THREAD (render -> ctl_drain -> listeners): enqueue only, no JS, no log, no lock */
    conn_t *c = (conn_t *)user;
    rx_js_t *j = c->owner;
    (void)id;
    unsigned w = atomic_load(&j->conn_w), r = atomic_load(&j->conn_r);
    if (w - r >= CONN_RING) { atomic_fetch_add(&j->conn_dropped, 1); return; }
    j->conn_ring[w % CONN_RING].conn = (int)(c - j->conns) | (c->gen << 16);
    j->conn_ring[w % CONN_RING].value = value;
    atomic_store(&j->conn_w, w + 1);
}

static void conn_fire(rx_js_t *j, conn_t *c, double value) {
    JSValue args[3] = { JS_NewFloat64(j->ctx, value), JS_NewString(j->ctx, c->group), JS_NewString(j->ctx, c->name) };
    call_void(j, c->callback, 3, args, c->name);
    for (int i = 0; i < 3; i++) JS_FreeValue(j->ctx, args[i]);
}

static void conn_drop(rx_js_t *j, conn_t *c) {
    if (!c->active) return;
    ctl_remove_listener(c->listener);
    JS_FreeValue(j->ctx, c->callback);
    c->callback = JS_UNDEFINED;
    c->active = 0;
    c->gen = (c->gen + 1) & 0x7fff;
}

/* connection.disconnect() / trigger(): the object carries its slot index and gen as hidden properties */
static conn_t *conn_from_this(JSContext *ctx, JSValueConst this_val) {
    rx_js_t *j = J(ctx);
    JSValue vi = JS_GetPropertyStr(ctx, this_val, "__slot");
    JSValue vg = JS_GetPropertyStr(ctx, this_val, "__gen");
    int32_t slot = -1, gen = -1;
    JS_ToInt32(ctx, &slot, vi);
    JS_ToInt32(ctx, &gen, vg);
    JS_FreeValue(ctx, vi);
    JS_FreeValue(ctx, vg);
    if (slot < 0 || slot >= MAX_CONN) return NULL;
    conn_t *c = &j->conns[slot];
    if (!c->active || c->gen != gen) return NULL;
    return c;
}

static JSValue conn_js_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    conn_t *c = conn_from_this(ctx, this_val);
    if (!c) return JS_NewBool(ctx, 0);
    conn_drop(J(ctx), c);
    JS_SetPropertyStr(ctx, this_val, "isConnected", JS_NewBool(ctx, 0));
    return JS_NewBool(ctx, 1);
}

static JSValue conn_js_trigger(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    conn_t *c = conn_from_this(ctx, this_val);
    if (c) conn_fire(J(ctx), c, ctl_get(c->ctl));
    return JS_UNDEFINED;
}

static JSValue conn_make_object(rx_js_t *j, conn_t *c) {
    JSContext *ctx = j->ctx;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__slot", JS_NewInt32(ctx, (int)(c - j->conns)));
    JS_SetPropertyStr(ctx, o, "__gen", JS_NewInt32(ctx, c->gen));
    JS_SetPropertyStr(ctx, o, "id", JS_NewString(ctx, c->id));
    JS_SetPropertyStr(ctx, o, "group", JS_NewString(ctx, c->group));
    JS_SetPropertyStr(ctx, o, "key", JS_NewString(ctx, c->name));
    JS_SetPropertyStr(ctx, o, "isConnected", JS_NewBool(ctx, 1));
    JS_SetPropertyStr(ctx, o, "disconnect", JS_NewCFunction(ctx, conn_js_disconnect, "disconnect", 0));
    JS_SetPropertyStr(ctx, o, "trigger", JS_NewCFunction(ctx, conn_js_trigger, "trigger", 0));
    return o;
}

static JSValue make_connection(rx_js_t *j, const char *group, const char *name, JSValueConst cb_in) {
    ctl_id id = lookup_ctl(j, group, name, "makeConnection");
    if (id == CTL_NONE) return JS_NewBool(j->ctx, 0);
    JSValue cb = callback_from_value(j, cb_in);
    if (!JS_IsFunction(j->ctx, cb)) {
        JS_FreeValue(j->ctx, cb);
        rx_log("js: makeConnection(%s,%s): callback is not a function", group, name);
        return JS_NewBool(j->ctx, 0);
    }
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONN; i++) if (!j->conns[i].active) { c = &j->conns[i]; break; }
    if (!c) { JS_FreeValue(j->ctx, cb); rx_log("js: makeConnection: too many connections (%d)", MAX_CONN); return JS_NewBool(j->ctx, 0); }
    c->ctl = id;
    c->callback = cb;
    snprintf(c->group, sizeof c->group, "%s", group);
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->id, sizeof c->id, "conn-%d", ++j->nconns);
    c->active = 1;
    c->listener = ctl_add_listener(id, conn_listener, c);
    return conn_make_object(j, c);
}

/* ------------------------------------------------------------------------------------------------ engine.* */
static JSValue e_getValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewFloat64(ctx, 0);
    ctl_id id = lookup_ctl(J(ctx), g, n, "getValue");
    return JS_NewFloat64(ctx, id == CTL_NONE ? 0 : ctl_get(id));
}

static JSValue e_setValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(J(ctx), g, n, "setValue");
    if (id == CTL_NONE) return JS_UNDEFINED;
    double v;
    if (JS_IsBool(argv[2])) v = JS_ToBool(ctx, argv[2]);       /* scripts pass !engine.getValue(...) */
    else v = arg_num(ctx, argv[2], 0);
    ctl_post(id, v);
    return JS_UNDEFINED;
}

static JSValue e_getParameter(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewFloat64(ctx, 0);
    ctl_id id = lookup_ctl(J(ctx), g, n, "getParameter");
    return JS_NewFloat64(ctx, id == CTL_NONE ? 0 : ctl_param(id, ctl_get(id)));
}

static JSValue e_setParameter(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(J(ctx), g, n, "setParameter");
    if (id == CTL_NONE) return JS_UNDEFINED;
    double p = arg_num(ctx, argv[2], 0);
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    ctl_post(id, ctl_min(id) + p * (ctl_max(id) - ctl_min(id)));
    return JS_UNDEFINED;
}

static JSValue e_getParameterForValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewFloat64(ctx, 0);
    ctl_id id = lookup_ctl(J(ctx), g, n, "getParameterForValue");
    return JS_NewFloat64(ctx, id == CTL_NONE ? 0 : ctl_param(id, arg_num(ctx, argv[2], 0)));
}

static JSValue e_reset(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(J(ctx), g, n, "reset");
    if (id == CTL_NONE) return JS_UNDEFINED;
    /* the ctl API carries no default; a bipolar control (rate -1..1, crossfader) resets to its centre, others to min */
    double lo = ctl_min(id), hi = ctl_max(id);
    ctl_post(id, (lo < 0 && hi > 0) ? 0 : lo);
    return JS_UNDEFINED;
}

static JSValue e_getDefaultValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewFloat64(ctx, 0);
    ctl_id id = lookup_ctl(J(ctx), g, n, "getDefaultValue");
    if (id == CTL_NONE) return JS_NewFloat64(ctx, 0);
    double lo = ctl_min(id), hi = ctl_max(id);
    return JS_NewFloat64(ctx, (lo < 0 && hi > 0) ? 0 : lo);
}

static JSValue e_getDefaultParameter(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewFloat64(ctx, 0);
    ctl_id id = lookup_ctl(J(ctx), g, n, "getDefaultParameter");
    if (id == CTL_NONE) return JS_NewFloat64(ctx, 0);
    double lo = ctl_min(id), hi = ctl_max(id);
    return JS_NewFloat64(ctx, ctl_param(id, (lo < 0 && hi > 0) ? 0 : lo));
}

static JSValue e_makeConnection(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewBool(ctx, 0);
    return make_connection(J(ctx), g, n, argv[2]);
}

/* legacy connectControl(group, name, callback[, disconnect]): disconnect=true removes the connection(s) to that
 * control with that callback (any, if the callback does not match); a connection object passed as callback is
 * disconnected; otherwise a duplicate connection is not made (Mixxx returns the existing one) */
static JSValue e_connectControl(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewBool(ctx, 0);
    int disconnect = argc >= 4 && JS_ToBool(ctx, argv[3]) > 0;
    if (JS_IsObject(argv[2]) && !JS_IsFunction(ctx, argv[2])) {       /* a connection object: disconnect it */
        conn_t *c = conn_from_this(ctx, argv[2]);
        if (c) conn_drop(j, c);
        return JS_NewBool(ctx, 0);
    }
    JSValue cb = callback_from_value(j, argv[2]);
    ctl_id id = ctl_lookup(g, n);
    if (disconnect) {
        int any = 0;
        for (int i = 0; i < MAX_CONN; i++) {
            conn_t *c = &j->conns[i];
            if (c->active && c->ctl == id && (!JS_IsFunction(ctx, cb) || JS_IsStrictEqual(ctx, c->callback, cb))) { conn_drop(j, c); any = 1; }
        }
        JS_FreeValue(ctx, cb);
        return JS_NewBool(ctx, any);
    }
    if (JS_IsFunction(ctx, cb)) {                                      /* an existing identical connection is returned */
        for (int i = 0; i < MAX_CONN; i++) {
            conn_t *c = &j->conns[i];
            if (c->active && c->ctl == id && JS_IsStrictEqual(ctx, c->callback, cb)) { JS_FreeValue(ctx, cb); return conn_make_object(j, c); }
        }
    }
    JSValue r = make_connection(j, g, n, cb);
    JS_FreeValue(ctx, cb);
    return r;
}

static JSValue e_trigger(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(j, g, n, "trigger");
    if (id == CTL_NONE) return JS_UNDEFINED;
    double v = ctl_get(id);
    for (int i = 0; i < MAX_CONN; i++) if (j->conns[i].active && j->conns[i].ctl == id) conn_fire(j, &j->conns[i], v);
    return JS_UNDEFINED;
}

static JSValue e_log(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char s[LOG_LINE];
    if (argc >= 1 && arg_str(ctx, argv[0], s, sizeof s)) rx_log("js: %s", s);
    return JS_UNDEFINED;
}

static JSValue e_beginTimer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    if (argc < 2) return JS_NewInt32(ctx, 0);
    double ms = arg_num(ctx, argv[0], 0);
    if (ms < 20) { rx_log("js: beginTimer(%g ms) too short, using 20 ms (Mixxx's minimum)", ms); ms = 20; }
    int oneshot = argc >= 3 && JS_ToBool(ctx, argv[2]) > 0;
    JSValue cb;
    if (JS_IsString(argv[1])) {                       /* a code string: wrap lazily like Mixxx's ()=>code */
        const char *code = JS_ToCString(ctx, argv[1]);
        char wrapped[1024];
        snprintf(wrapped, sizeof wrapped, "(()=>{%s})", code ? code : "");
        if (code) JS_FreeCString(ctx, code);
        cb = JS_Eval(ctx, wrapped, strlen(wrapped), "<timer>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(cb)) { js_report_exception(j, "beginTimer"); return JS_NewInt32(ctx, 0); }
    } else if (JS_IsFunction(ctx, argv[1])) cb = JS_DupValue(ctx, argv[1]);
    else { rx_log("js: beginTimer: callback is neither a function nor a string"); return JS_NewInt32(ctx, 0); }
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_ *tm = &j->timers[i];
        if (tm->active) continue;
        tm->active = 1;
        tm->oneshot = oneshot;
        tm->id = ++j->next_timer_id;
        tm->interval_ms = ms;
        tm->due = now_ms() + ms;
        tm->callback = cb;
        return JS_NewInt32(ctx, tm->id);
    }
    JS_FreeValue(ctx, cb);
    rx_log("js: beginTimer: too many timers (%d)", MAX_TIMERS);
    return JS_NewInt32(ctx, 0);
}

static JSValue e_stopTimer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_UNDEFINED;
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_ *tm = &j->timers[i];
        if (tm->active && tm->id == id) { tm->active = 0; JS_FreeValue(ctx, tm->callback); tm->callback = JS_UNDEFINED; return JS_UNDEFINED; }
    }
    rx_log("js: stopTimer(%d): no such timer", id);
    return JS_UNDEFINED;
}

static rx_deck_t *deck_of(rx_js_t *j, JSContext *ctx, JSValueConst v, int *deck_out) {
    int32_t deck = 0;
    JS_ToInt32(ctx, &deck, v);
    if (deck_out) *deck_out = deck;
    if (deck < 1 || deck > RX_DECKS || !j->engine) return NULL;
    return rx_deck(j->engine, deck - 1);
}

static JSValue e_scratchEnable(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int deck;
    rx_deck_t *d = argc >= 5 ? deck_of(j, ctx, argv[0], &deck) : NULL;
    if (!d) return JS_UNDEFINED;
    double ipr = arg_num(ctx, argv[1], 720), rpm = arg_num(ctx, argv[2], 33.333333);
    double alpha = arg_num(ctx, argv[3], 1.0 / 8), beta = arg_num(ctx, argv[4], 1.0 / 256);
    if (ipr <= 0 || rpm <= 0) { rx_log("js: scratchEnable: invalid rpm/intervalsPerRev, ignored"); return JS_UNDEFINED; }
    j->scratch_scale[deck] = 720.0 / ipr;
    j->scratch_on[deck] = 1;
    deck_scratch_enable(d, 1, rpm, alpha, beta);
    return JS_UNDEFINED;
}

static JSValue e_scratchTick(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int deck;
    rx_deck_t *d = argc >= 2 ? deck_of(j, ctx, argv[0], &deck) : NULL;
    if (!d) return JS_UNDEFINED;
    double s = j->scratch_scale[deck] > 0 ? j->scratch_scale[deck] : 1;
    deck_scratch_tick(d, arg_num(ctx, argv[1], 0) * s);
    return JS_UNDEFINED;
}

static JSValue e_scratchDisable(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int deck;
    rx_deck_t *d = argc >= 1 ? deck_of(j, ctx, argv[0], &deck) : NULL;
    if (!d) return JS_UNDEFINED;
    j->scratch_on[deck] = 0;
    deck_scratch_enable(d, 0, 0, 0, 0);                  /* the deck ramps back by itself (ramp arg ignored) */
    return JS_UNDEFINED;
}

static JSValue e_isScratching(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int deck = 0;
    if (argc >= 1) JS_ToInt32(ctx, &deck, argv[0]);
    if (deck < 1 || deck > RX_DECKS) return JS_NewBool(ctx, 0);
    char g[24];
    snprintf(g, sizeof g, "[Channel%d]", deck);
    ctl_id id = ctl_lookup(g, "scratch2_enable");
    return JS_NewBool(ctx, id != CTL_NONE ? ctl_get(id) > 0 : j->scratch_on[deck]);
}

/* soft takeover (Mixxx SoftTakeover semantics, see midi_js.h) */
void js_st_enable(rx_js_t *j, ctl_id id, int on) {
    if (!j || id < 0 || id >= ST_MAX) return;
    st_slot_t *s = &j->st[id];
    if (on && !atomic_load(&s->enabled)) { atomic_store(&s->first, 1); s->prev = 0; s->t_ms = 0; }
    atomic_store(&s->enabled, on);
}

void js_st_ignore_next(rx_js_t *j, ctl_id id) {
    if (!j || id < 0 || id >= ST_MAX) return;
    atomic_store(&j->st[id].first, 1);
}

static int st_will_ignore(rx_js_t *j, ctl_id id, double param, double t) {
    st_slot_t *s = &j->st[id];
    if (atomic_load(&s->first)) return 1;
    if (t < s->t_ms + ST_OVERRIDE_MS) return 0;
    double cur = ctl_param(id, ctl_get(id));
    double diff = cur - param, prev = cur - s->prev;
    if (signbit(prev) != signbit(diff)) return 0;
    if (fabs(diff) <= ST_THRESHOLD || fabs(prev) <= ST_THRESHOLD) return 0;
    return 1;
}

int js_st_check(rx_js_t *j, ctl_id id, double param) {
    if (!j || id < 0 || id >= ST_MAX) return 0;
    st_slot_t *s = &j->st[id];
    if (!atomic_load(&s->enabled)) return 0;
    double t = now_ms();
    int ignore;
    if (atomic_load(&s->first)) { ignore = 1; atomic_store(&s->first, 0); s->t_ms = 0; }
    else ignore = st_will_ignore(j, id, param, t);
    if (!ignore) s->t_ms = t;
    s->prev = param;
    return ignore;
}

static JSValue e_softTakeover(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(J(ctx), g, n, "softTakeover");
    if (id != CTL_NONE) js_st_enable(J(ctx), id, JS_ToBool(ctx, argv[2]) > 0);
    return JS_UNDEFINED;
}

static JSValue e_softTakeoverIgnoreNextValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 2 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_UNDEFINED;
    ctl_id id = lookup_ctl(J(ctx), g, n, "softTakeoverIgnoreNextValue");
    if (id != CTL_NONE) js_st_ignore_next(J(ctx), id);
    return JS_UNDEFINED;
}

static JSValue e_softTakeoverWillIgnore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char g[48], n[64];
    if (argc < 3 || !arg_str(ctx, argv[0], g, sizeof g) || !arg_str(ctx, argv[1], n, sizeof n)) return JS_NewBool(ctx, 0);
    rx_js_t *j = J(ctx);
    ctl_id id = lookup_ctl(j, g, n, "softTakeoverWillIgnore");
    if (id == CTL_NONE || id >= ST_MAX || !atomic_load(&j->st[id].enabled)) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, st_will_ignore(j, id, arg_num(ctx, argv[2], 0), now_ms()));
}

/* stubs: brake/spinback/softStart have no deck model yet; logged once each so a mapping that uses them says so */
static JSValue stub(JSContext *ctx, int bit, const char *name, JSValue ret) {
    rx_js_t *j = J(ctx);
    if (!(j->warned_stub & (1 << bit))) { j->warned_stub |= 1 << bit; rx_log("js: engine.%s is not implemented (no-op)", name); }
    return ret;
}
static JSValue e_brake(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return stub(ctx, 0, "brake", JS_UNDEFINED); }
static JSValue e_spinback(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return stub(ctx, 1, "spinback", JS_UNDEFINED); }
static JSValue e_softStart(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return stub(ctx, 2, "softStart", JS_UNDEFINED); }
static JSValue e_isBrakeActive(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewBool(ctx, 0); }
static JSValue e_getSetting(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) { (void)ctx; (void)t; (void)argc; (void)argv; return JS_UNDEFINED; }

/* ------------------------------------------------------------------------------------------------ midi.* */
static JSValue m_sendShortMsg(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    int32_t a = 0, b = 0, c = 0;
    if (argc < 3) return JS_UNDEFINED;
    JS_ToInt32(ctx, &a, argv[0]); JS_ToInt32(ctx, &b, argv[1]); JS_ToInt32(ctx, &c, argv[2]);
    if (j->hooks.send) j->hooks.send(j->hooks.user, (uint8_t)a, (uint8_t)(b & 0x7f), (uint8_t)(c & 0x7f));
    return JS_UNDEFINED;
}

static JSValue m_sendSysexMsg(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t len = 0;
    if (argc >= 2) JS_ToInt32(ctx, &len, argv[1]);
    rx_log("js: midi.sendSysexMsg(%d bytes): sysex output is not routed (ignored)", len);
    return JS_UNDEFINED;
}

static JSValue m_send(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rx_js_t *j = J(ctx);
    if (argc < 1) return JS_UNDEFINED;
    JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    if (len == 3) {
        int32_t b[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) { JSValue e = JS_GetPropertyUint32(ctx, argv[0], i); JS_ToInt32(ctx, &b[i], e); JS_FreeValue(ctx, e); }
        if (j->hooks.send) j->hooks.send(j->hooks.user, (uint8_t)b[0], (uint8_t)(b[1] & 0x7f), (uint8_t)(b[2] & 0x7f));
    } else rx_log("js: midi.send(%d bytes): only 3-byte messages are routed (ignored)", len);
    return JS_UNDEFINED;
}

/* console.log and friends: every argument stringified, joined with spaces */
static JSValue c_log(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    char line[LOG_LINE];
    int n = 0;
    for (int i = 0; i < argc && n < (int)sizeof line - 1; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    rx_log("js: %s", line);
    return JS_UNDEFINED;
}

static void bind_fn(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int nargs) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, nargs));
}

static void install_api(rx_js_t *j) {
    JSContext *ctx = j->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_NewObject(ctx);
    bind_fn(ctx, engine, "getValue", e_getValue, 2);
    bind_fn(ctx, engine, "setValue", e_setValue, 3);
    bind_fn(ctx, engine, "getParameter", e_getParameter, 2);
    bind_fn(ctx, engine, "setParameter", e_setParameter, 3);
    bind_fn(ctx, engine, "getParameterForValue", e_getParameterForValue, 3);
    bind_fn(ctx, engine, "reset", e_reset, 2);
    bind_fn(ctx, engine, "getDefaultValue", e_getDefaultValue, 2);
    bind_fn(ctx, engine, "getDefaultParameter", e_getDefaultParameter, 2);
    bind_fn(ctx, engine, "makeConnection", e_makeConnection, 3);
    bind_fn(ctx, engine, "makeUnbufferedConnection", e_makeConnection, 3);
    bind_fn(ctx, engine, "connectControl", e_connectControl, 4);
    bind_fn(ctx, engine, "trigger", e_trigger, 2);
    bind_fn(ctx, engine, "log", e_log, 1);
    bind_fn(ctx, engine, "beginTimer", e_beginTimer, 3);
    bind_fn(ctx, engine, "stopTimer", e_stopTimer, 1);
    bind_fn(ctx, engine, "scratchEnable", e_scratchEnable, 6);
    bind_fn(ctx, engine, "scratchTick", e_scratchTick, 2);
    bind_fn(ctx, engine, "scratchDisable", e_scratchDisable, 2);
    bind_fn(ctx, engine, "isScratching", e_isScratching, 1);
    bind_fn(ctx, engine, "softTakeover", e_softTakeover, 3);
    bind_fn(ctx, engine, "softTakeoverIgnoreNextValue", e_softTakeoverIgnoreNextValue, 2);
    bind_fn(ctx, engine, "softTakeoverWillIgnore", e_softTakeoverWillIgnore, 3);
    bind_fn(ctx, engine, "brake", e_brake, 4);
    bind_fn(ctx, engine, "spinback", e_spinback, 4);
    bind_fn(ctx, engine, "softStart", e_softStart, 3);
    bind_fn(ctx, engine, "isBrakeActive", e_isBrakeActive, 1);
    bind_fn(ctx, engine, "isSpinbackActive", e_isBrakeActive, 1);
    bind_fn(ctx, engine, "isSoftStartActive", e_isBrakeActive, 1);
    bind_fn(ctx, engine, "getSetting", e_getSetting, 1);
    bind_fn(ctx, engine, "getPlayer", e_getSetting, 1);
    JS_SetPropertyStr(ctx, global, "engine", engine);

    JSValue midi = JS_NewObject(ctx);
    bind_fn(ctx, midi, "sendShortMsg", m_sendShortMsg, 3);
    bind_fn(ctx, midi, "sendSysexMsg", m_sendSysexMsg, 2);
    bind_fn(ctx, midi, "send", m_send, 1);
    JS_SetPropertyStr(ctx, global, "midi", midi);

    JSValue console = JS_NewObject(ctx);
    bind_fn(ctx, console, "log", c_log, 1);
    bind_fn(ctx, console, "info", c_log, 1);
    bind_fn(ctx, console, "warn", c_log, 1);
    bind_fn(ctx, console, "error", c_log, 1);
    bind_fn(ctx, console, "debug", c_log, 1);
    bind_fn(ctx, console, "trace", c_log, 1);
    JS_SetPropertyStr(ctx, global, "console", console);
    bind_fn(ctx, global, "print", c_log, 1);           /* common-controller-scripts.js redefines it on top of console.log */
    JS_FreeValue(ctx, global);
}

static int new_context(rx_js_t *j) {
    j->ctx = JS_NewContext(j->rt);
    if (!j->ctx) return -1;
    JS_SetContextOpaque(j->ctx, j);
    install_api(j);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the worker */
static void drain_bindings(rx_js_t *j) {
    for (;;) {
        unsigned r = atomic_load(&j->bind_r);
        if (r == atomic_load(&j->bind_w)) break;
        bind_ev_t *ev = &j->bind_ring[r % BIND_RING];
        JSValue fn = resolve_dotted(j->ctx, ev->fn);
        if (JS_IsFunction(j->ctx, fn)) {
            JSValue args[5] = { JS_NewInt32(j->ctx, ev->channel), JS_NewInt32(j->ctx, ev->control), JS_NewInt32(j->ctx, ev->value),
                                JS_NewInt32(j->ctx, ev->status), JS_NewString(j->ctx, ev->group) };
            call_void(j, fn, 5, args, ev->fn);
            JS_FreeValue(j->ctx, args[4]);
        } else {
            static int warned;
            if (warned < 20) { warned++; rx_log("js: script binding %s is not a function", ev->fn); }
        }
        JS_FreeValue(j->ctx, fn);
        atomic_store(&j->bind_r, r + 1);
    }
}

static void drain_connections(rx_js_t *j) {
    int budget = CONN_RING;
    while (budget-- > 0) {
        unsigned r = atomic_load(&j->conn_r);
        if (r == atomic_load(&j->conn_w)) break;
        conn_ev_t ev = j->conn_ring[r % CONN_RING];
        atomic_store(&j->conn_r, r + 1);
        int slot = ev.conn & 0xffff, gen = ev.conn >> 16;
        if (slot >= 0 && slot < MAX_CONN) {
            conn_t *c = &j->conns[slot];
            if (c->active && c->gen == gen) conn_fire(j, c, ev.value);
        }
    }
    int d = atomic_exchange(&j->conn_dropped, 0);
    if (d) rx_log("js: %d control-change events dropped (ring full)", d);
}

static void run_timers(rx_js_t *j) {
    double t = now_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_ *tm = &j->timers[i];
        if (!tm->active || t < tm->due) continue;
        JSValue cb = JS_DupValue(j->ctx, tm->callback);       /* the callback may stop or replace the timer */
        if (tm->oneshot) { tm->active = 0; JS_FreeValue(j->ctx, tm->callback); tm->callback = JS_UNDEFINED; }
        else { tm->due += tm->interval_ms; if (tm->due < t) tm->due = t + tm->interval_ms; }
        call_void(j, cb, 0, NULL, "timer");
        JS_FreeValue(j->ctx, cb);
    }
}

static void *worker_main(void *arg) {
    rx_js_t *j = (rx_js_t *)arg;
    while (atomic_load(&j->running)) {
        log_drain(j);
        js_enter(j);
        if (j->ctx) {
            drain_bindings(j);
            drain_connections(j);
            run_timers(j);
            JSContext *pctx;
            while (JS_ExecutePendingJob(j->rt, &pctx) > 0) {}
        }
        pthread_mutex_unlock(&j->lock);
        if (j->hooks.idle) j->hooks.idle(j->hooks.user);
        struct timespec ts = { 0, JS_WORKER_PERIOD_US * 1000L };
        nanosleep(&ts, NULL);
    }
    log_drain(j);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ public */
rx_js_t *js_start(rx_engine_t *e, const rx_js_hooks_t *hooks) {
    rx_js_t *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->engine = e;
    if (hooks) j->hooks = *hooks;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);   /* a timer callback may call js_* through midi.c */
    pthread_mutex_init(&j->lock, &ma);
    pthread_mutexattr_destroy(&ma);
    j->rt = JS_NewRuntime();
    if (!j->rt) { free(j); return NULL; }
    JS_SetMemoryLimit(j->rt, JS_MEM_LIMIT);
    JS_SetMaxStackSize(j->rt, JS_STACK_LIMIT);
    for (int i = 0; i < MAX_CONN; i++) { j->conns[i].callback = JS_UNDEFINED; j->conns[i].owner = j; }
    for (int i = 0; i < MAX_TIMERS; i++) j->timers[i].callback = JS_UNDEFINED;
    if (new_context(j) < 0) { JS_FreeRuntime(j->rt); free(j); return NULL; }
    atomic_store(&j->running, 1);
    if (pthread_create(&j->worker, NULL, worker_main, j) != 0) {
        rx_log("js: cannot start the worker thread: %s", strerror(errno));
        atomic_store(&j->running, 0);
        JS_FreeContext(j->ctx);
        JS_FreeRuntime(j->rt);
        free(j);
        return NULL;
    }
    rx_log("js: QuickJS %d.%d.%d started (worker period %d us, %u MB limit)", QJS_VERSION_MAJOR, QJS_VERSION_MINOR,
           QJS_VERSION_PATCH, JS_WORKER_PERIOD_US, JS_MEM_LIMIT >> 20);
    return j;
}

int js_load_file(rx_js_t *j, const char *path) {
    if (!j) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { rx_log("js: cannot open %s: %s", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((n > 0 ? n : 0) + 1);
    if (!src) { fclose(f); return -1; }
    size_t got = fread(src, 1, n > 0 ? n : 0, f);
    fclose(f);
    src[got] = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    js_enter(j);
    int rc = 0;
    JSValue r = JS_Eval(j->ctx, src, got, base, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { js_report_exception(j, base); rc = -1; }
    JS_FreeValue(j->ctx, r);
    pthread_mutex_unlock(&j->lock);
    free(src);
    rx_log("js: loaded %s (%zu bytes)%s", base, got, rc ? " WITH ERRORS" : "");
    return rc;
}

int js_call_init(rx_js_t *j, const char *prefix, const char *controller_id) {
    if (!j || !prefix || !prefix[0]) return -1;
    js_enter(j);
    snprintf(j->controller_id, sizeof j->controller_id, "%s", controller_id ? controller_id : "");
    char path[64];
    snprintf(path, sizeof path, "%s.init", prefix);
    JSValue fn = resolve_dotted(j->ctx, path);
    int rc = -1;
    if (JS_IsFunction(j->ctx, fn)) {
        JSValue self = resolve_dotted(j->ctx, prefix);
        JSValue args[2] = { JS_NewString(j->ctx, j->controller_id), JS_NewBool(j->ctx, 0) };
        JSValue r = JS_Call(j->ctx, fn, self, 2, args);
        if (JS_IsException(r)) js_report_exception(j, path); else rc = 0;
        JS_FreeValue(j->ctx, r);
        JS_FreeValue(j->ctx, args[0]);
        JS_FreeValue(j->ctx, self);
        if (j->nprefix < MAX_PREFIX) snprintf(j->prefixes[j->nprefix++], 32, "%s", prefix);
        rx_log("js: %s(\"%s\") %s", path, j->controller_id, rc ? "threw" : "ok");
    } else rx_log("js: %s is not a function (the script has no init; continuing)", path);
    JS_FreeValue(j->ctx, fn);
    pthread_mutex_unlock(&j->lock);
    return rc;
}

void js_unload(rx_js_t *j) {
    if (!j) return;
    js_enter(j);
    for (int i = 0; i < j->nprefix; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s.shutdown", j->prefixes[i]);
        JSValue fn = resolve_dotted(j->ctx, path);
        if (JS_IsFunction(j->ctx, fn)) {
            JSValue self = resolve_dotted(j->ctx, j->prefixes[i]);
            JSValue r = JS_Call(j->ctx, fn, self, 0, NULL);
            if (JS_IsException(r)) js_report_exception(j, path);
            JS_FreeValue(j->ctx, r);
            JS_FreeValue(j->ctx, self);
        }
        JS_FreeValue(j->ctx, fn);
    }
    j->nprefix = 0;
    for (int i = 0; i < MAX_CONN; i++) conn_drop(j, &j->conns[i]);
    for (int i = 0; i < MAX_TIMERS; i++) if (j->timers[i].active) { j->timers[i].active = 0; JS_FreeValue(j->ctx, j->timers[i].callback); j->timers[i].callback = JS_UNDEFINED; }
    for (int i = 0; i < ST_MAX; i++) atomic_store(&j->st[i].enabled, 0);
    /* throw the old rings away: their events referred to the old context */
    atomic_store(&j->bind_r, atomic_load(&j->bind_w));
    atomic_store(&j->conn_r, atomic_load(&j->conn_w));
    JS_FreeContext(j->ctx);
    j->ctx = NULL;
    JS_RunGC(j->rt);
    new_context(j);
    j->nwarned_ctl = 0;
    j->warned_stub = 0;
    pthread_mutex_unlock(&j->lock);
    rx_log("js: scripts unloaded, fresh context");
}

void js_stop(rx_js_t *j) {
    if (!j) return;
    js_unload(j);
    atomic_store(&j->running, 0);
    pthread_join(j->worker, NULL);
    pthread_mutex_lock(&j->lock);
    if (j->ctx) JS_FreeContext(j->ctx);
    j->ctx = NULL;
    JS_FreeRuntime(j->rt);
    pthread_mutex_unlock(&j->lock);
    pthread_mutex_destroy(&j->lock);
    free(j);
}

int js_post_binding(rx_js_t *j, const char *fn, int channel, int control, int value, int status, const char *group) {
    if (!j) return -1;
    unsigned w = atomic_load(&j->bind_w), r = atomic_load(&j->bind_r);
    if (w - r >= BIND_RING) { atomic_fetch_add(&j->dropped, 1); return -1; }
    bind_ev_t *ev = &j->bind_ring[w % BIND_RING];
    snprintf(ev->fn, sizeof ev->fn, "%s", fn ? fn : "");
    snprintf(ev->group, sizeof ev->group, "%s", group ? group : "");
    ev->channel = channel; ev->control = control; ev->value = value; ev->status = status;
    atomic_store(&j->bind_w, w + 1);
    return 0;
}

int js_dropped_messages(rx_js_t *j) { return j ? atomic_exchange(&j->dropped, 0) : 0; }
