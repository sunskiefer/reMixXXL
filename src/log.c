/* reMixXXL -- the diag log: <plugin dir>/remixxxl_diag.log, timestamped, fresh at every open.
 *
 * Threading: rx_log() takes a mutex and does buffered stdio; it is safe from any thread EXCEPT the audio thread
 * (render()/midi() inside the wrapper's lock). The audio thread must not call rx_log: a disk write there is an
 * audio drop-out. Parts that run on the audio thread (midi_in, ctl listeners) hand their messages to a worker
 * thread (midi_js.c's worker) and that thread logs. touch.c and the JS worker log freely.
 *
 * Before rx_log_open() (or if the file cannot be created) messages go to stderr, so a failing start still says
 * why in MPC's console. */
#define _GNU_SOURCE
#include "remix.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log;
static char g_plugin_dir[RX_PATH];
static struct timeval g_t0;           /* when the log was opened: the timestamp column is seconds since then */

const char *rx_plugin_dir(void) { return g_plugin_dir; }

void rx_log_open(const char *plugin_dir) {
    pthread_mutex_lock(&g_log_mutex);
    if (plugin_dir && plugin_dir != g_plugin_dir) {
        snprintf(g_plugin_dir, sizeof g_plugin_dir, "%s", plugin_dir);
        size_t n = strlen(g_plugin_dir);
        while (n > 1 && g_plugin_dir[n - 1] == '/') g_plugin_dir[--n] = 0;
    }
    if (g_log) { fclose(g_log); g_log = NULL; }
    char path[RX_PATH + 32];
    snprintf(path, sizeof path, "%s/remixxxl_diag.log", g_plugin_dir[0] ? g_plugin_dir : ".");
    g_log = fopen(path, "w");                       /* fresh at open: the user sends one file, this run only */
    gettimeofday(&g_t0, NULL);
    if (g_log) {
        time_t now = time(NULL);
        char stamp[64];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(g_log, "reMixXXL %s diag log, opened %s, plugin dir %s, pid %d\n",
                RX_VERSION_STR, stamp, g_plugin_dir, (int)getpid());
        fflush(g_log);
    } else {
        fprintf(stderr, "reMixXXL: cannot open %s for the diag log, logging to stderr\n", path);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void rx_log(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double t = (tv.tv_sec - g_t0.tv_sec) + (tv.tv_usec - g_t0.tv_usec) * 1e-6;
    pthread_mutex_lock(&g_log_mutex);
    FILE *f = g_log ? g_log : stderr;
    fprintf(f, "%10.3f %s\n", t, line);
    fflush(f);                                      /* every line: a crash must not lose the last ones */
    pthread_mutex_unlock(&g_log_mutex);
}
