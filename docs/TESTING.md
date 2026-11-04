# Testing reMixXXL

## Offline (done here, before any device)

| Check | How | Result |
|---|---|---|
| ARM compile of every source (armv7-a, glibc 2.31, Zig) | `./build.sh` | see build log |
| install.sh / uninstall.sh parse (`sh -n`, dash, bash) and the escaped `L'\''Cronx` | CI step, `build.sh` | |
| Skin: no overlapping touch boxes, nothing past 1280 x 628, Q-Links only on shown params | `tools/check_skin.py` | |
| Decoder: WAV/FLAC/MP3 to 44.1 kHz stereo int16 | `test/run_a1.sh` | |
| Analyzer: 120 BPM click track -> 120 +/- 1, waveform columns sane | `test/run_a1.sh` | |
| Deck: rate, reverse, loop bounds, scratch, EQ kills, no NaN | `test/run_a2.sh` | |
| Screen renderer: every page drawn to PNG, ms/frame on x86 | `test/run_b.sh` | |
| MIDI: Denon MC3000 .midi.xml + scripts load, play/jog/fader messages reach controls | `test/run_c.sh` | |

## On the Force (the user; nothing below is verified until a row has a date and an MPC OS version)

Install the release zip (`sh install.sh`, restarts MPC). Then, with the diag logs in the plugin folder
(`/sdcard/Synths/L'Cronx - VST - reMixXXL/`: `remixxxl_diag.log`, `remixxxl_overlay.log`):

| # | Test | Expected | Result / date / MPC OS |
|---|---|---|---|
| 1 | Insert the plugin on a track; open its screen | seven tabs: OVERVIEW DECK BROWSE MIXER FX WHEELS WHEEL | |
| 2 | BROWSE: tracks in `Tracks/` listed (6 rows, < > pages); FOLDER opens a subfolder; LOAD 1 | the track loads on deck 1 (readout in the stripe); the analysis cache `.rxa` and sidecar `.remixxxl` appear next to the file | |
| 3 | DECK: the scrolling waveform moves while playing; playhead centred; beat grid; the overview strip's line moves | `remixxxl_overlay.log`: "shown", frames per second | |
| 4 | OVERVIEW: four strips, lines move; tap a strip -> DECK's deck changes | | |
| 5 | WHEELS: touch the platter top: with VINYL on the track scratches; the ring nudges; the centre label turns with the track | `remixxxl_diag.log`: touch device found, events/s, down/up, slots | |
| 6 | WHEELS: play, cue, tempo fader, beat loop, beat jump, slip/rev, search, sync | each acts on the wheel's deck | |
| 7 | WHEEL: the same at full size | | |
| 8 | MIXER: faders, EQ kills, filter, crossfader, VU on the overlay | | |
| 9 | FX: an echo on unit 1 enabled for deck 1; mix | | |
| 10 | Q-Links on each tab and bank | | |
| 11 | Save the project, reload: tracks reload, cues and loops back | | |
| 12 | Denon MC3000 on the Force's USB, its MIDI routed to the track: MIDI MAP = the MC3000 map | play/cue/jog/faders work; LED feedback (needs the ALSA port routed) | |
| 13 | MIDI LEARN: arm, move a knob, the control follows | `midi_learn.txt` written | |
| 14 | CPU readout on DECK with four decks playing | under 60% of the block budget | |
| 15 | Four decks loaded with 6-minute tracks | no memory trouble (about 64 MB per deck) | |
