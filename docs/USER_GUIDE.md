# reMixXXL user guide (0.1.25)

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
off) as knobs that turn in place; PFL (send the channel to the cue on outputs 3/4 and take it out of the 1/2 mix; see "Cue (PFL) and the headphones"), FX 1 and FX 2 (send the channel through
that unit), the volume fader, a VU meter, and the crossfader side L / MID / R (MID: not on the crossfader). MASTER
column: MASTER (gain) and CUE MIX (a Mixxx control kept for mappings: it does not change the sound of the cue on 3/4), the four PFL 1-4 cue levels, the master VU. The crossfader along the bottom.

Q-Link banks: MIXER 1 (the four volumes, the four filters), MIXER 2 (gains, highs), MIXER 3 (mids, lows), MASTER
(master, cue mix, crossfader).

## Cue (PFL) and the headphones

reMixXXL makes two separate signals from one plugin: **outputs 1/2 are the mix to the main outs**, and the **cue (PFL) goes to outputs 3/4, which you listen to on the headphones jack**. MPC's plugin host only plays the first stereo pair of a plugin, so the cue leaves through a small companion effect, **reMixXXL CUE**, that sits on an audio track.

### What you hear where

| | what it carries |
|---|---|
| **Outputs 1/2 (main)** | the full mix of the decks **without** any deck that has PFL on. A deck with PFL on is taken out of the main mix, so the room never hears it. |
| **Outputs 3/4 (headphones)** | only the decks with PFL on. **Decks 1 and 3 on the LEFT, decks 2 and 4 on the RIGHT**, so you can hear one deck in each ear and beat match. With no PFL on, the cue is silent. |

The cue is taken after each deck's gain, EQ, filter and effects but **before the channel fader and crossfader**: a deck with its fader all the way down is still heard at full level in the cue. That is what lets you cue and beat match a track before you bring it in.

### Volume: where the monitoring is controlled

The monitoring volume is set on the knobs in the plugin, not on the main faders:

- **PFL 1, PFL 2, PFL 3, PFL 4** (MIXER page, under CUE MIX; also Q-Link / automation): each deck's own level in the cue. 50% is normal level, up is louder. Use them to balance the two ears while beat matching.
- **CUE LEVEL** (on the reMixXXL CUE screen): the overall level of the whole cue output.
- The headphones jack's own volume on the Force still applies on top.

The main mix on 1/2 is not touched by any of these knobs: the main faders, crossfader and MASTER knob control 1/2 only.

### Set-up, step by step

1. **Install both zips** on the Force (each has its own `install.sh`): `reMixXXL-0.1.25-mpc-armv7.zip` and `reMixXXL-CUE-0.1.3-mpc-armv7.zip`. Restart MPC so both plugins appear.
2. **Track A: reMixXXL.** Load **reMixXXL** on a track as the instrument. Leave that track's output on the main outputs (**1/2**). Do **not** put reMixXXL CUE on this track: the effect replaces the audio of the track it sits on with the cue.
3. **Track B: the cue track.** Make a separate **audio track**. In its effect slot load **reMixXXL CUE**. This track needs no recording and no input: the effect makes its own sound from reMixXXL's cue.
4. **Route track B to outputs 3/4**: set that audio track's output to **3/4** (not Main), so none of the cue reaches 1/2.
5. **Headphones:** set the Force so the headphones jack plays outputs 3/4 (MPC's audio output / headphone settings; the exact menu name depends on your MPC OS version). Now the headphones play the cue and the main outs play the mix.
6. Load tracks on the decks, press **PFL** on a deck (MIXER page, or the deck's PFL button): the deck disappears from 1/2 and you hear it in the headphones: deck 1 and 3 on the left, 2 and 4 on the right. Adjust with the PFL knobs.

Both plugins must be running in MPC at the same time (they talk through a small shared buffer in memory). The order they were loaded in does not matter. The cue has about 9 ms of delay (a small cushion), which does not matter for beat matching.

### The STATUS line on reMixXXL CUE

The CUE screen shows what it is doing:

| STATUS | meaning |
|---|---|
| WAITING: EFFECT NOT RUN YET | the track has not started processing yet: press play in MPC |
| NO RING FILE | the shared buffer could not be created (should not happen) |
| reMixXXL NOT RUNNING | reMixXXL is not loaded or has not started: load it on a track and press play |
| reMixXXL STOPPED | reMixXXL stopped being processed (its track is muted or MPC is stopped) |
| NO PFL ON (SILENT) | everything works; press PFL on a deck |
| PLAYING THE CUE | the cue is playing |

### Troubleshooting

- **You hear the cue on the main outs (1/2):** reMixXXL CUE is on the reMixXXL track, or the cue track is still routed to Main. Move CUE to its own audio track and set that track's output to 3/4.
- **Nothing in the headphones:** read the STATUS line; check the headphones are set to outputs 3/4; check the PFL knob is not at zero.
- **A deck is in both the main mix and the cue:** its PFL button is off; with PFL on it leaves the main mix.

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
