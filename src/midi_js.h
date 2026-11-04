/* reMixXXL -- the QuickJS binding for Mixxx controller scripts (midi_js.c).
 *
 * One JS runtime/context, guarded by one mutex (the "JS lock"), and ONE thread that runs JS: the JS worker
 * (ours). It runs, in turn, every few ms:
 *   - script bindings queued by midi_in() (js_post_binding: a lock-free ring filled on the audio thread);
 *   - control-change callbacks (engine.makeConnection / connectControl): ctl listeners fire on the audio thread and
 *     only push (id, value) into a ring; the worker drains it and calls the JS callbacks;
 *   - script timers (engine.beginTimer);
 *   - the deferred log (js_log is safe from any thread; the worker writes the lines with rx_log);
 *   - the owner's idle hook (midi.c pumps its LED feedback ring to ALSA there), outside the JS lock.
 * So NO JS ever runs on the audio thread and midi_in() never blocks: it only enqueues. This is stricter than Mixxx
 * (whose MIDI thread calls JS directly) and costs one worker period (~3 ms) of latency per message.
 *
 * Build: compile third_party/quickjs/{quickjs.c, libregexp.c, libunicode.c, dtoa.c} with this file
 * (src/quickjs_sources.txt lists them). cutils.h is header-only in quickjs-ng 0.17. */
#pragma once
#include <stdint.h>
#include "remix.h"

typedef struct rx_js rx_js_t;

/* midi.c's hooks. send: one short MIDI message for the controller, called from the worker thread (inside a script);
 * it should only enqueue. idle: called by the worker every period outside the JS lock (drain that queue to ALSA). */
typedef struct {
    void (*send)(void *user, uint8_t status, uint8_t d1, uint8_t d2);
    void (*idle)(void *user);
    void *user;
} rx_js_hooks_t;

rx_js_t *js_start(rx_engine_t *e, const rx_js_hooks_t *hooks);   /* runtime + worker thread; NULL on failure */
void js_stop(rx_js_t *j);                                        /* shutdown() the scripts, join the worker, free */

/* Script loading (any non-audio thread; takes the JS lock). Files are evaluated in the global scope in the order
 * given. js_call_init calls <prefix>.init(id, false) (Mixxx passes the controller id and the debug flag);
 * js_unload calls shutdown() on every prefix that was init'ed, drops every connection and timer, and makes a fresh
 * context so the next mapping starts clean. */
int js_load_file(rx_js_t *j, const char *path);                  /* 0 ok, -1 error (logged) */
int js_call_init(rx_js_t *j, const char *prefix, const char *controller_id);
void js_unload(rx_js_t *j);

/* The audio-thread entry: queue a script binding call for the worker. fn is the XML <key> of a script-binding
 * control ("mc3000.jogWheel"), a dotted path resolved on the global object at call time, called as
 * fn(channel, control, value, status, group). Lock-free; returns 0, or -1 when the ring is full (counted). */
int js_post_binding(rx_js_t *j, const char *fn, int channel, int control, int value, int status, const char *group);
int js_dropped_messages(rx_js_t *j);

/* Deferred log: safe from any thread (the audio thread included). */
void js_log(rx_js_t *j, const char *fmt, ...);

/* Soft takeover (Mixxx's SoftTakeover, shared by the XML <soft-takeover/> option and engine.softTakeover):
 * js_st_check says whether an incoming normalised parameter (0..1) for a control should be ignored. Lock-free, so
 * midi_in may call it on the audio thread. */
void js_st_enable(rx_js_t *j, ctl_id id, int on);
void js_st_ignore_next(rx_js_t *j, ctl_id id);
int js_st_check(rx_js_t *j, ctl_id id, double param);           /* 1 = ignore this value */
