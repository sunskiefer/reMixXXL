# reMixXXL user guide (0.1.22)

reMixXXL is a plugin instrument on a Force / MPC track: four DJ decks, a mixer, two effect units and a track browser,
all on the plugin's own screen pages. Audio comes out of the track like any plugin's; MIDI into the track reaches the
engine (controller mappings, MIDI learn). The pages are MPC's tabs at the bottom of the plugin screen; the Q-Links
follow each page (and each page's banks).

Every button and knob on every page is also a Mixxx control, so a Mixxx controller mapping drives the same things.

## The stripe

The dark band across the top of every page: the page name at the left, then the page's own things (deck buttons,
readouts, the ZOOM popup). The DECK 1-4 buttons on OVERVIEW and DECK pick the deck the DECK page shows; on WHEELS
each side has its own four (which deck that wheel drives), WHEEL uses the left wheel's.

## OVERVIEW

Four strips, one per deck, top to bottom: the deck's whole-track waveform with its playhead and loop. Tap a strip to
make that deck the DECK page's deck (then switch to the DECK tab). The title of the selected deck is in the stripe.

## DECK

The selected deck (DECK 1-4 in the stripe), Mixxx style: the scrolling waveform (the window is ZOOM seconds wide,
2 / 4 / 8 / 16 / 32 S), the overview strip under it with the playhead, hot cue marks and the loop.

Row 1: PLAY, CUE (press: cue / return to cue, Mixxx's cue_default), SYNC (one-shot beat sync), SLIP, REV, KEYLOCK,
QUANT (quantize cues and loops to the beat), VINYL (vinyl mode: the wheel scratches the playing track; off = CDJ: it nudges the pitch). On the WHEEL page the VINYL / CDJ button switches it: the lit word is the current mode. The wheel never spins on its own while a track plays; it turns when you scratch or adjust it.
Row 2: hot cues 1-8 (empty: set; set: jump; CLR CUE + a hot cue clears it, from a mapping or a Q-Link).
Row 3: LOOP IN, LOOP OUT, RELOOP (on / off), LOOP /2, LOOP X2, < JUMP, JUMP > (the beat jump size), EJECT.
Under the rows: the values the DECK Q-Link bank edits (TEMPO, FILTER, GAIN, HIGH, MID, LOW, LOOP size, JUMP size).
Stripe readouts: title, BPM, time left, tempo in %.

Q-Link banks DECK 1 .. DECK 4 (one per deck, since the Q-Links cannot follow the stripe's deck buttons):
rate, filter, gain, high, mid, low, loop size, jump size, zoom.

## BROWSE

The track list of the current folder (the path in the stripe), six rows a page. Tap a row to select it, LOAD 1-4
loads the selected row into that deck. FOLDER lists the folders instead (tap one to enter it, the first row goes up
one level). < PAGE and PAGE > turn the pages (the page number in the stripe, the SCROLL fader runs the whole list).
SORT: NAME, BPM or ARTIST. Tracks are WAV, FLAC or MP3; the plugin's own `Tracks` folder is the start folder. Tracks
are analysed when loaded (beat grid, BPM, key) in the background.

Q-Links: scroll, select, page, sort.

## MIXER

One channel strip per deck: GAIN, HIGH, MID, LOW and FILTER (the quick effect: left low-pass, right high-pass, middle
off) as knobs that turn in place; PFL (cue the channel in the headphone mix), FX 1 and FX 2 (send the channel through
that unit), the volume fader, a VU meter, and the crossfader side L / MID / R (MID: not on the crossfader). MASTER
column: MASTER (gain) and CUE MIX (headphones: cue to master), the master VU. The crossfader along the bottom.

Q-Link banks: MIXER 1 (the four volumes, the four filters), MIXER 2 (gains, highs), MIXER 3 (mids, lows), MASTER
(master, cue mix, crossfader).

## FX

Both units on one page, unit 1 on top, unit 2 below. Each unit: three slots (the effect popup 1.1 .. 2.3: NONE, ECHO,
FILTER, REVERB, FLANGER, PHASER, BITCRUSH, MOOG LP, LOOP ROLL; ON; META, the effect's one-knob macro; P1-P4, the
effect's own parameters, named after the effect), then the unit's row: UNIT ON, MIX (dry / wet), SUPER (all three
metas at once), MASTER (the unit on the master output) and DECKS 1 2 3 4 (which decks go through the unit; the same
switches as FX 1 / FX 2 on the mixer).

Q-Link banks: FX 1 and FX 2 (the three metas, mix, super), FX 1 PARAMS and FX 2 PARAMS (P1-P4 of the three slots).

## WHEELS and WHEEL

(0.1.11/0.1.12: the WHEEL page uses the author's own layout, see `docs/img/wheel.png`; the part ids below are the same, only their places changed.)

The CDJ decks, made from the CDJ photo. WHEELS: two side by side, each with DECK 1-4 buttons in the stripe for the
deck it drives; WHEEL: one deck full size (the left wheel's deck). The parts (the numbers are the ids in
`Layout/wheels_overlay.png` / `Layout/wheel_overlay.png`):

| id | part | does |
|---|---|---|
| 1 | the platter | touch the top: scratch (VINYL on) or nudge; the outer ring: nudge / pitch bend. The centre label spins with the track |
| 3 | CUE | cue (press / release, as Mixxx's cue_default) |
| 4 | PLAY | play / pause |
| 5 | tempo fader | tempo (rate); 0 in the middle, + at the bottom as on the CDJ; the range is part 8 |
| 6 | tempo reset | tempo back to 0 |
| 7 | master tempo | keylock |
| 8 | tempo range | a popup: 6 %, 10 %, 16 %, WIDE |
| 9 | beat loop 4 / 8 | left: a 4-beat loop, right: an 8-beat loop |
| 10 | in / out adjust | left: loop in, right: loop out |
| 12 | beat jump | left: jump back, right: jump forward (the jump size) |
| 13 | slip rev | left: slip, right: reverse |
| 14 | track search | left: previous track, right: next track |
| 15 | search | left: back, right: forward (a beat a tap) |
| 16 | jog feel | the platter's sensitivity (a knob that turns in place) |
| 17 | vinyl / jog mode | vinyl mode on / off |
| 18 | beat sync / master | left: sync, right: sync master |
| 19 | key sync | match the key to the other deck |

A lit part shows its ON state in orange. The stripe shows each wheel's deck, BPM and time, and TOUCH (what the
touchscreen reader sees, for checking). At the bottom (WHEELS) or the left (WHEEL): the deck's filter, volume, gain,
high, mid and low values, which the page's Q-Link banks edit (WHEELS 1 .. 4 / WHEEL 1 .. 4: rate, jog feel, filter,
volume, gain, high, mid, low of that deck).

## MIDI

Set MIDI MAP (a Q-Link or a mapping's own control) to one of the `.midi.xml` mappings in the plugin's `Mappings`
folder (the Denon MC3000 ships first); NONE turns mappings off. MIDI LEARN on: the next CC or note received is bound
to the control last touched on the screen; the bindings are saved next to the plugin and loaded at start.

## Files next to the plugin

`Tracks/` (your music, or pick another folder on BROWSE), `Mappings/` (Mixxx controller mappings), `Fonts/`
(Titillium Web, for the overlay's text), `Screen/platter_frames.png` (the spinning label), `Layout/` (the page
geometry `pages.json` and the numbered overlays of the wheel pages, to say "move 5 to x,y"), `remixxxl_diag.log` and
`remixxxl_overlay.log` (the logs, for reporting a problem).
