/* reMixXXL: start the wheel overlay and the touch layer where there is nothing for them to use (no DRM card among the
 * process's files, no touch device): they must not fail the plugin. overlay_start still returns an object (it keeps retrying in the background, nothing is drawn: live=0), the touch
 * layer reports no device, and both stop cleanly. That is what a machine whose display or touch device the code cannot use
 * would do: the decks, mixer, browser, FX, MIDI and the companion page keep working. Run by test/arm_sim.sh (armv7, aarch64). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "remix.h"


/* what the engine and the decks would provide: nothing is called here (no finger ever touches a wheel), they only have to link */
int rx_wave_columns(int deck, rx_wave_col_t *out, int first_col, int n) { (void)deck; (void)out; (void)first_col; (void)n; return 0; }
int rx_wave_total_cols(int deck) { (void)deck; return 0; }
int rx_overview(int deck, rx_wave_col_t *out) { (void)deck; (void)out; return 0; }
rx_deck_t *rx_deck(rx_engine_t *e, int i) { (void)e; (void)i; return NULL; }
void deck_scratch_enable(rx_deck_t *d, int enable, double rpm, double alpha, double beta) { (void)d; (void)enable; (void)rpm; (void)alpha; (void)beta; }
void deck_scratch_tick(rx_deck_t *d, double ticks) { (void)d; (void)ticks; }
int deck_is_playing(rx_deck_t *d) { (void)d; return 0; }
void deck_seek_by_turns(rx_deck_t *d, double turns) { (void)d; (void)turns; }
void deck_ext_jog(rx_deck_t *d, int on) { (void)d; (void)on; }
void deck_jog(rx_deck_t *d, double amount) { (void)d; (void)amount; }

int main(void) {
    const char *dir = "/tmp/rx_noovl";
    mkdir(dir, 0755);
    ctl_init();
    int ok = -1;
    rx_overlay_t *o = overlay_start(dir, &ok);
    rx_touch_t *t = touch_start(dir, o, NULL);
    usleep(1500000);   /* the threads run, and look for a card and a device */
    int live = 0, avail = 0;
    char last[160] = "";
    if (o) {
        rx_view_t v;
        memset(&v, 0, sizeof v);
        overlay_publish(o, &v);
        overlay_read_view(o, &v);
        live = overlay_live(o);
        overlay_status(o, 0, last, sizeof last);
    }
    if (t) avail = touch_available(t);
    printf("overlay_start ok=%d (object %s), live=%d; touch %s, available=%d; last overlay log line: %s\n", ok, o ? "returned" : "NULL",
           live, t ? "started" : "NULL", avail, last[0] ? last : "(none)");
    if (t) touch_stop(t);
    if (o) overlay_stop(o);
    puts("stopped cleanly");
    return (o && !live && !avail) ? 0 : 1;   /* ok: the overlay thread started and retries every 3 s (1), or found no plane at once (0); either way nothing is drawn and nothing fails */
}
