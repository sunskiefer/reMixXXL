/* reMixXXL native test of the MIDI side: midi_xml.c on the Denon MC3000 mapping, midi.c + midi_js.c loading the
 * mapping and its scripts (QuickJS), then a few messages: play (a plain note control), the jog wheel and wheel
 * touch (script bindings -> engine.scratchEnable/scratchTick/jog), a volume fader (plain CC), the pitch slider (a
 * pitch-bend script binding), the select knob (script -> [Playlist],SelectNextTrack), MIDI learn (CC and an
 * encoder), and the LED feedback path (a play change -> mc3000.playSetLed -> midi.sendShortMsg). Run by
 * test/run_c.sh with the stub control table (ctl_stub_c.c); the real control.c is another agent's. */
#define _GNU_SOURCE
#include "remix.h"
#include "midi_xml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

extern int stub_scratch_enable[], stub_scratch_calls[];
extern double stub_scratch_ticks[], stub_jog[];

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void send(rx_midi_t *m, int a, int b, int c) { uint8_t msg[3] = { (uint8_t)a, (uint8_t)b, (uint8_t)c }; midi_in(m, msg, 3); }
static void settle(void) { usleep(30000); }     /* a few worker periods: bindings and connection callbacks run */

static int count_file_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512]; int n = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/test_plugin";
    char xml[RX_PATH], logpath[RX_PATH];
    snprintf(xml, sizeof xml, "%s/Mappings/Denon MC3000.midi.xml", dir);
    snprintf(logpath, sizeof logpath, "%s/remixxxl_diag.log", dir);
    rx_log_open(dir);

    /* 1. the XML reader on the real file */
    char err[128];
    xml_node_t *root = xml_parse_file(xml, err, sizeof err);
    CHECK(root != NULL, "xml parses: %s", root ? "yes" : err);
    if (!root) return 1;
    CHECK(!strcmp(root->name, "MixxxMIDIPreset"), "root element %s", root->name);
    const char *sv = xml_attr(root, "schemaVersion");
    CHECK(sv && !strcmp(sv, "1"), "schemaVersion attr = %s", sv ? sv : "(none)");
    xml_node_t *info = xml_child(root, "info");
    CHECK(info && !strcmp(xml_child_text(info, "name"), "Denon MC3000 v0.995"), "info/name = %s", info ? xml_child_text(info, "name") : "?");
    xml_node_t *ctrl = xml_child(root, "controller");
    const char *cid = xml_attr(ctrl, "id");
    CHECK(cid && !strcmp(cid, "MC3000 MIDI 1"), "controller id = %s", cid ? cid : "?");
    int ncontrol = 0, nscript = 0;
    xml_node_t *controls = xml_child(ctrl, "controls");
    for (xml_node_t *c = xml_child(controls, "control"); c; c = xml_next(c, "control")) {
        ncontrol++;
        xml_node_t *o = xml_child(c, "options");
        if (o && xml_child(o, "script-binding")) nscript++;
    }
    CHECK(ncontrol == 158, "158 <control> entries (got %d)", ncontrol);
    CHECK(nscript == 108, "108 script bindings (got %d)", nscript);
    xml_node_t *file = xml_child(xml_child(ctrl, "scriptfiles"), "file");
    CHECK(file && !strcmp(xml_attr(file, "filename"), "Denon-MC3000-scripts.js") && !strcmp(xml_attr(file, "functionprefix"), "mc3000"), "scriptfiles/file attrs");
    /* a control with a hex status and a decimal midino, and the trimmed text */
    xml_node_t *c1 = xml_child(controls, "control");
    CHECK(xml_text_int(c1, "status", -1) == 0xb0 && xml_text_int(c1, "midino", -1) == 0x16, "first control status/midino 0x%x/0x%x", xml_text_int(c1, "status", -1), xml_text_int(c1, "midino", -1));
    CHECK(!strcmp(xml_child_text(c1, "group"), "[Master]") && !strcmp(xml_child_text(c1, "key"), "crossfader"), "first control [Master],crossfader");
    xml_free(root);
    /* entities and self-closing tags */
    const char *doc = "<?xml version='1.0'?><!DOCTYPE x><a b=\"1 &amp; 2\"><!-- c --><d/><e>x &lt;y&gt; &#65;&#x42; &quot;q&quot; &apos;</e><![CDATA[<raw>]]></a>";
    root = xml_parse(doc, strlen(doc), err, sizeof err);
    CHECK(root && !strcmp(xml_attr(root, "b"), "1 & 2"), "attribute entity");
    CHECK(root && xml_child(root, "d") && xml_child(root, "d")->first_child == NULL, "self-closing element");
    CHECK(root && !strcmp(xml_child_text(root, "e"), "x <y> AB \"q\" '"), "text entities: '%s'", root ? xml_child_text(root, "e") : "");
    CHECK(root && strstr(root->text, "<raw>"), "CDATA raw");
    xml_free(root);
    CHECK(xml_parse("<a><b></a>", 10, err, sizeof err) == NULL && err[0], "mismatched end tag reported: %s", err);

    /* 2. midi.c with the mapping and scripts */
    rx_midi_t *m = midi_start(dir, rx_engine_instance());
    CHECK(m != NULL, "midi_start");
    CHECK(midi_mapping_count(m) >= 1, "mapping files found: %d", midi_mapping_count(m));
    CHECK(midi_load_mapping(m, xml) == 0, "load mapping");
    CHECK(!strcmp(midi_mapping_name(m), "Denon MC3000 v0.995"), "mapping name %s", midi_mapping_name(m));
    settle();
    /* init() connected controls and set the vinyl-mode LEDs on (mc3000.allLedOff -> setled(i, 6, 1)) */
    CHECK(count_file_lines(logpath, "mc3000.init") == 1, "init called once (log)");
    CHECK(count_file_lines(logpath, "WITH ERRORS") == 0, "scripts loaded without errors");

    ctl_id play1 = ctl_lookup("[Channel1]", "play");
    ctl_id vol1 = ctl_lookup("[Channel1]", "volume");
    ctl_id rate1 = ctl_lookup("[Channel1]", "rate");
    ctl_id jog1 = ctl_lookup("[Channel1]", "jog");
    ctl_id next = ctl_lookup("[Playlist]", "SelectNextTrack");
    ctl_id prev = ctl_lookup("[Playlist]", "SelectPrevTrack");
    ctl_id cue1 = ctl_lookup("[Channel1]", "cue_default");

    /* play: 0x90 0x43 on [Channel1] (no options): a note on a toggle -> toggles on press only */
    send(m, 0x90, 0x43, 127);
    CHECK(ctl_get(play1) == 1, "play note-on -> play = 1");
    send(m, 0x80, 0x43, 0);
    CHECK(ctl_get(play1) == 1, "play note-off -> stays 1");
    send(m, 0x90, 0x43, 127);
    CHECK(ctl_get(play1) == 0, "play again -> 0");
    settle();
    /* the LED feedback: playSetLed -> midi.sendShortMsg(0xB0, 74/75, 39): counted as unrouted here (no ALSA in
     * the test) -> only check that the connection fired (log line from a print in the script? none) via the
     * cue_default binding below which prints */

    /* volume fader: 0xB0 0x05 plain CC on volume (0..1) */
    send(m, 0xB0, 0x05, 0);
    CHECK(fabs(ctl_get(vol1) - 0.0) < 1e-9, "volume CC 0 -> 0");
    send(m, 0xB0, 0x05, 127);
    CHECK(fabs(ctl_get(vol1) - 1.0) < 1e-9, "volume CC 127 -> 1");
    send(m, 0xB0, 0x05, 64);
    CHECK(fabs(ctl_get(vol1) - 64.0 / 127) < 1e-9, "volume CC 64 -> %.4f", ctl_get(vol1));

    /* cue: script binding mc3000.cue_default (prints, sets cue_default 1 on 0x9n, 0 on 0x8n, and an LED) */
    send(m, 0x90, 0x42, 127);
    settle();
    CHECK(ctl_get(cue1) == 1, "cue note-on (script) -> cue_default = 1");
    send(m, 0x80, 0x42, 0);
    settle();
    CHECK(ctl_get(cue1) == 0, "cue note-off (script) -> cue_default = 0");
    CHECK(count_file_lines(logpath, "cue_def - status") >= 2, "script print() reached the log");

    /* wheel touch + jog: scratch (mc3000.scratch default true) */
    send(m, 0x90, 81, 127);       /* touch deck 1 */
    settle();
    CHECK(stub_scratch_enable[0] == 1, "wheelTouch -> scratchEnable deck 1 (calls %d)", stub_scratch_calls[0]);
    send(m, 0xB0, 0x51, 70);      /* +6 */
    send(m, 0xB0, 0x51, 70);
    settle();
    CHECK(fabs(stub_scratch_ticks[0] - 2 * 6 * (720.0 / 100)) < 1e-6, "jogWheel while touched -> scratchTick 2 x 6 ticks scaled 720/100 = %.2f", stub_scratch_ticks[0]);
    send(m, 0x80, 81, 0);         /* release */
    settle();
    CHECK(stub_scratch_enable[0] == 0, "wheel release -> scratchDisable");
    send(m, 0xB0, 0x51, 60);      /* -4 untouched: nudge via [Channel1],jog */
    settle();
    CHECK(fabs(ctl_get(jog1) - (3.0 * -4 / (64 * 0.5)) / 2.0) < 1e-6, "jogWheel untouched -> jog = %.4f", ctl_get(jog1));

    /* pitch slider: 0xE0 pitch bend, script mc3000.rate: (MSB<<7|LSB - 8192)/8192 */
    send(m, 0xE0, 0x00, 0x60);    /* 0x60<<7 = 12288 -> (12288-8192)/8192 = 0.5 */
    settle();
    CHECK(fabs(ctl_get(rate1) - 0.5) < 1e-9, "pitch bend -> rate %.3f", ctl_get(rate1));

    /* select knob: script, value 0 -> SelectNextTrack, else Prev */
    send(m, 0xB0, 0x54, 0);
    settle();
    CHECK(ctl_get(next) == 1, "selectKnob 0 -> SelectNextTrack");
    send(m, 0xB0, 0x54, 127);
    settle();
    CHECK(ctl_get(prev) == 1, "selectKnob 127 -> SelectPrevTrack");

    /* keylock: plain note on a toggle, deck 2 (0x92 6) */
    ctl_id kl2 = ctl_lookup("[Channel2]", "keylock");
    send(m, 0x92, 6, 127);
    CHECK(ctl_get(kl2) == 1, "keylock deck 2 note -> toggled on");

    /* 3. MIDI learn: a CC on [Channel3],pregain (not in the MC3000 map on that status), then an encoder */
    ctl_id pre3 = ctl_lookup("[Channel3]", "pregain");
    midi_learn_arm(m, pre3);
    send(m, 0xB5, 0x20, 100);     /* learned, not applied */
    settle();
    CHECK(fabs(ctl_get(pre3) - 1.0) < 1e-9, "learn: the learning message itself is not applied (pregain still %.2f)", ctl_get(pre3));
    send(m, 0xB5, 0x20, 0);
    CHECK(fabs(ctl_get(pre3) - 0.0) < 1e-9, "learned CC 0 -> pregain 0");
    send(m, 0xB5, 0x20, 127);
    CHECK(fabs(ctl_get(pre3) - 1.0) < 1e-9, "learned CC 127 -> pregain 1");
    settle();
    char lf[RX_PATH]; snprintf(lf, sizeof lf, "%s/midi_learn.txt", dir);
    CHECK(count_file_lines(lf, "[Channel3],pregain abs") == 1, "learn file saved with the abs entry");
    /* an encoder on the crossfader: values 1 / 127 for six messages flip it to rel */
    ctl_id xf = ctl_lookup("[Master]", "crossfader");
    ctl_post(xf, 0);
    midi_learn_arm(m, xf);
    send(m, 0xB6, 0x10, 1);       /* learned */
    for (int i = 0; i < 6; i++) send(m, 0xB6, 0x10, 1);     /* detection window: abs applied, then rel */
    settle();
    CHECK(count_file_lines(lf, "[Master],crossfader rel") == 1, "encoder detected: learn file says rel");
    double before = ctl_get(xf);
    send(m, 0xB6, 0x10, 127);     /* -1 step */
    CHECK(ctl_get(xf) < before, "rel encoder step down: %.4f -> %.4f", before, ctl_get(xf));
    /* a learned note on play of deck 4 */
    ctl_id play4 = ctl_lookup("[Channel4]", "play");
    midi_learn_arm(m, play4);
    send(m, 0x97, 60, 100);
    send(m, 0x97, 60, 100);
    CHECK(ctl_get(play4) == 1, "learned note -> play 4 toggled on");
    send(m, 0x87, 60, 0);
    CHECK(ctl_get(play4) == 1, "learned note-off ignored by the toggle");
    CHECK(midi_learn_clear(m, play4) == 1, "learn clear removes one entry");
    send(m, 0x97, 60, 100);
    CHECK(ctl_get(play4) == 1, "cleared mapping no longer acts");

    /* 4. reload the mapping: a fresh JS context, init again, old connections gone */
    CHECK(midi_load_mapping(m, xml) == 0, "reload mapping");
    settle();
    CHECK(count_file_lines(logpath, "mc3000.init") == 2, "init called again on reload");
    send(m, 0x90, 0x42, 127); send(m, 0x80, 0x42, 0);
    settle();
    CHECK(count_file_lines(logpath, "cue_def - status") >= 4, "scripts work after reload");

    /* 5. feedback path: play change -> connection -> playSetLed -> midi.sendShortMsg -> ring (unrouted here) */
    send(m, 0x90, 0x43, 127);
    settle();
    midi_stop(m);
    CHECK(count_file_lines(logpath, "midi: stopped; feedback") == 1, "midi_stop logged the feedback counters");
    {   /* the play LED (mc3000.playSetLed -> midi.sendShortMsg) and init's allLedOff went into the ring: unrouted here */
        FILE *f = fopen(logpath, "r"); char line[512]; int unrouted = -1;
        while (f && fgets(line, sizeof line, f)) { char *p = strstr(line, "midi: stopped; feedback"); if (p) sscanf(p, "midi: stopped; feedback since the last report: %*d sent to ALSA, %d unrouted", &unrouted); }
        if (f) fclose(f);
        CHECK(unrouted > 60, "LED feedback messages reached the output ring (%d, no ALSA in the test)", unrouted);
    }
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
