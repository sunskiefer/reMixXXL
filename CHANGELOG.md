# Changelog

## 0.1.25 (release, tested on a Force)
Cue (PFL) for beat matching, with the companion effect reMixXXL CUE 0.1.3. (1) Outputs 1/2 are the main mix only; a deck with PFL on goes ONLY to the cue bus and is taken out of the 1/2 mix (its fader, VU and effects still run). (2) The cue bus plays on outputs 3/4 (the headphones jack) through reMixXXL CUE on its own audio track: decks 1 and 3 on the left, 2 and 4 on the right; the cue is taken before the channel fader and crossfader. (3) New monitor levels PFL 1 - PFL 4 (MIXER page, under CUE MIX; Q-Link / automation too): each deck's own level on the cue bus, 50% = unity; reMixXXL CUE has an overall CUE LEVEL knob. (4) The cue ring is a memory-mapped file shared by the two plugins (no library lookup). (5) reMixXXL CUE shows a STATUS line (waiting, reMixXXL not running, no PFL on, playing). Parameters were only appended. Tested by the author on the Force (MPC OS 3.9.1). Full routing guide: README "Cue (PFL)".

## 0.1.24 (tested on a Force with 0.1.25)
The cue bus no longer needs the CUE effect to find reMixXXL's library (a /proc/self/maps + dlopen lookup that could fail): reMixXXL now writes the cue ring into a memory-mapped file (/dev/shm, else /tmp) and reMixXXL CUE 0.1.2 reads that file. Same sound: decks 1/3 left, 2/4 right, PFL decks only, never in the 1/2 mix. Put reMixXXL CUE on a DIFFERENT audio track from reMixXXL's own track (it replaces its track's audio with the cue), and route that audio track to outputs 3/4.

## 0.1.23 (tested on a Force with 0.1.25)
Cue output for beat matching. MPC plays only the first stereo pair (outputs 1/2) of a plugin, so the cue leaves through a small second plugin. (1) reMixXXL's outputs 1/2 are the mix only: PFL sound is never in it. (2) New cue bus: only the decks with PFL on, taken after pregain, EQ and effects but before the channel fader and crossfader; decks 1 and 3 fold to mono on the LEFT, decks 2 and 4 on the RIGHT. (3) New plugin reMixXXL CUE (reMixXXL-CUE-0.1.0-mpc-armv7.zip): an audio effect you put on an audio track's effect slot; it plays the cue bus (left = decks 1/3, right = decks 2/4) with a CUE LEVEL knob. Route that audio track to the Force outputs you want (for example 3/4). reMixXXL has to be on a track too; without it the CUE effect is silent. About 9 ms of cue delay (a cushion so either plugin may run first in a period).

## 0.1.22 (release, tested on a Force)
- VINYL / CDJ (WHEEL page): the button's tap is now taken by the plugin's own touch layer, which flips the deck's vinyl mode itself (logged as "touch: VINYL/CDJ deck N -> CDJ|VINYL"); the skin only draws the button (lit word = current mode) and no longer relies on MPC's toggle. Tested on a Force (MPC OS 3.9.1).

## 0.1.21 (tested on a Force with 0.1.22)
VINYL and CDJ do two different things again, and the VINYL / CDJ + JOG MODE button switches between them. (1) WHEEL page: the button's touch area was only 38 px wide (half of half the pair); the whole pair (VINYL / CDJ and JOG MODE) is now one button for the vinyl mode, and it shows the mode: VINYL lit and CDJ dimmed when it is on, the other way round when off. (2) WHEEL and WHEELS pages: since 0.1.16 the wheel scratched the track in both modes; now VINYL scratches the playing track (clockwise forward, anticlockwise back) and CDJ adjusts it (a pitch nudge, scaled by JOG ADJUST). A stopped track is moved by the wheel in both modes, as before.

## 0.1.20 (tested on a Force with 0.1.22)
WHEEL page, right-hand side only (everything else as 0.1.19): the right-hand column is cut from your own picture (`art/ui/controls/wheel_page_user_0.1.11.png`) at full size instead of the controls sheet squeezed into smaller boxes, so VINYL / CDJ + JOG MODE, BEAT SYNC / MASTER, KEY SYNC, TEMPO, MASTER TEMPO and TEMPO RESET are their real size, and the CUE/LOOP CALL / DELETE / MEMORY art and the tempo scale are back. The column is moved 43 px to the right so no space is left at the right edge. The JOG ADJUST knob (dial turning in place) is now left of the pitch slider, under TEMPO RESET, and about 1.6 times its old size on the page.

## 0.1.19 (tested on a Force with 0.1.22)
Two fixes to 0.1.18 (touch tested: works). (1) Changing between the WHEELS and WHEEL pages left the old page's wheel drawing on screen for a few seconds: the live screen now looks for the page every other frame while one is shown, and when the page has just gone it searches the whole picture straight away instead of waiting for its slow turn. (2) The wheel (and the skin's jog ring) no longer turns with the playing track, as on a CDJ: it turns only when it is scratched (VINYL, finger or phone), when it is adjusted in CDJ mode (by the amount the finger or phone adjusted it), and when it moves a stopped track.

## 0.1.18 (tested on a Force with 0.1.22)
The WHEEL page's wheel behaviour (tested: perfect) now also drives the two small wheels of the WHEELS page: each ring turns freely with the finger and with its deck's platter (`tools/gen_ring.py` now also makes `Screen/ring_420.png` and `ring_light_420.png`), and a clockwise circle moves that deck's track forward, anticlockwise back, stopped or playing. The live screen and the touch layer now work both pages. The wheels' faces are the skin's own again: the live screen no longer draws a photo label disc, a position arc or any text (deck, BPM, time) over or round the wheels, so the skin's centre and its BPM digits show as before. The ring turn is now integer maths (about 5 to 10 times cheaper): 0.1.17 took about 105 ms a frame on the Force (6 frames a second).

## 0.1.17 (tested on a Force with 0.1.22)
Fixes 0.1.16, which showed nothing new on the Force: the live screen (`src/overlay.c`) and the engine's touch reader (`src/touch.c`) both assumed the display is not turned, but the Force's panel is portrait (800 x 1280) and MPC draws its screen turned 90 degrees into it, so neither ever found the page or read a touch in the right place (that is also why the waveforms never showed). They now find the turn, the page and the plane the way XYZ-STACK does (rotation-aware marker search, the page turned into the plane, touch mapped back through the turn). Limited to the WHEEL page in this test: the live screen draws only that page, and the touch layer works only that page's wheel (the buttons stay the skin's own). Everything else as in 0.1.16.

## 0.1.16 (tested on a Force with 0.1.22)
WHEEL page only (the one-deck page): the wheel picture is the endless encoder, drawn by the live screen turned to any angle like XYZ-STACK's wheel (`tools/gen_ring.py` makes `Screen/ring_574.png` and `ring_light_574.png`; the skin's own ring stays under it as the fallback). It turns exactly with the finger around the wheel's centre and with the platter. A clockwise circle moves the track forward, an anticlockwise circle moves it back (one turn is about 1.8 s of track, scaled by JOG ADJUST), stopped or playing, VINYL or CDJ alike (a playing track scratches; a stopped one moves its playhead). The WHEELS page and the other pages are unchanged.

## 0.1.15 (release)
Packaged build `reMixXXL-0.1.15-mpc-armv7.zip` (install.sh, uninstall.sh, ARMv7 `remixxxl.so`, skin, Mappings, Screen, Layout). The source in this repo is the 0.1.15 source (it includes the 0.1.13 MAPS page and map builder). Changes made in 0.1.14 and 0.1.15 are not itemised here.
New: `reMixXXL-map-builder.html`, a saved copy of the map builder you open from your computer and connect to the Force (address, port 6731). Its **Mixer** tab now lists exactly the controls of the plugin's MIXER page, in page order (per deck VOLUME, FILTER, GAIN, HIGH, MID, LOW, PFL, FX 1, FX 2, XF SIDE; then MASTER, CUE MIX, CROSSFADER) under the page's own names; the per-deck FX enables also show on the FX tab; controls the page does not have (EQ kills, VU meters, Library, Playlist and the like) are under Other. From a saved copy SAVE TO FORCE is hidden (the browser blocks it): use DOWNLOAD `.midi.xml` and put the file in the plugin's `Mappings` folder. The plugin's own `/map` page is unchanged in this release (the fix is in the saved copy only).

## 0.1.13 (test build, not yet tested on a Force)
New MAPS page (second page of the BROWSE key): every `.midi.xml` in `Mappings`, LOAD / NONE, remembered at start. New map builder web page (`/map` on the companion address) that makes a `.midi.xml` for any controller by pressing its controls, with AUTO NEXT and a deck 1 to 2-4 copy; saves to the Force or downloads. 14 parameters appended (the wrapper's hidden popup parameters move up by 14). See docs/MAP_BUILDER.md.

## 0.1.12 (release)
The engine's own touch layer (`src/boxes.h`) now uses the WHEEL page's new button places (generated from `tools/wheel_geom.py` by `tools/remix_pages.py`), so a tap on track search / search / beat jump / slip / reverse hits only its own button; with 0.1.11 an old box could also fire. Skin as in 0.1.11. Source in this repo builds the release's `remixxxl.so` (SHA-256 00a9bafa62ddf93ed0fa2a7f8c4ce7aaba855dddf51344d988920f05a86b60c0).

## 0.1.11 (release)
WHEEL page: every control cut from the author's new transparent picture and placed where it puts them (loop, 4/8, track search, search, cue, play, beat jump, direction, vinyl, sync, key sync, tempo range, master tempo, tempo reset, jog adjust knob); new connector lines, tempo scale marks, VINYL/CDJ colours, red master tempo lamp, new jog adjust knob (turns in place). The tempo fader, the wheels and the ring are untouched; the Q-Link readouts stay on the left. Skin only: the engine file is the 0.1.10 one.
Known (fixed in 0.1.12): the engine's own touch layer still had the 0.1.10 button boxes.

## 0.1.10 (release, tested on a Force)
On a stopped or paused track, turning the wheel moves the playhead forward or back (VINYL and CDJ), one turn about 1.8 s of track; the ring picture follows. While playing, VINYL scrubs and CDJ nudges as in 0.1.9. Engine change only; skin as in 0.1.9.

## 0.1.9 (release, tested on a Force)
Skin-only changes on top of the engine in this source (the engine file is identical from the 0.1.4 build to 0.1.9): 48 "jog knob"
components in the skin and the ring film strips reversed so the ring turns the same way as the touch. Wheel touch, ring spin,
BPM digits, QR and companion page verified by the author on the Force. Nothing else changed.

## 0.1.4 (internal build label of the engine in this source; shipped as part of 0.1.5-0.1.9)
- The jog ring takes touch under the black plate. VINYL spins the track, CDJ nudges the pitch (scaled by JOG ADJUST). The ring turns with the platter when no finger is on it.
- Companion page: a dot next to ZOOM makes it full screen.
- The QR code is only drawn while SHOW QR is on (the small black circle toggles it); no cover square.
- Light and shadow on the wheel softened.
- The wheel never runs out: one transparent dial over the whole wheel (under the plate and the buttons round it); drag sideways, right = forward, left = back, with no end to run into.

## 0.1.2 (pre-release, untested on a device)

- The OVERVIEW page is gone (the plugin starts on DECK); the two black squares on the DECK page are gone.
- JOG WHEEL (WHEELS and WHEEL pages) is now a knob: your ring art turns on a film strip tied to the deck's pitch (touch it anywhere
  and drag to change the pitch; a sync / pitch match turns it), the still black centre disc sits on top, aligned to the centre, and the
  BPM shows on it in white DSEG14 digits (the BAZZLINE display font). On the WHEEL page a tap on the disc shows the companion page's
  address as a QR code (tap again for the BPM). The companion page now listens from port 6731 (VADER's range) and the QR follows the
  Force's address.
- WHEEL and WHEELS pages: the controls are cut from your controls sheet (`art/ui/controls/controls_user.png`) and placed as in your page
  picture (no selection frame on any button; a lit button is its own bright pixels turned orange). The MASTER TEMPO lamp flashes at the
  sync master's beat (else the first playing deck). A soft fixed light (top left) and shadow (bottom right) lie over the wheel.
- WHEELS page: both wheels show decks 1-4; the deck that is on the other wheel is greyed out, any other pair is allowed.

- COMPANION WEB PAGE (open the `WEB http://<force ip>:6731` address shown in the diagnosis line, from a phone or PC on the same
  network): one lane per deck filling the screen; `show 1/2/4` and the deck buttons choose which; Mixxx RGB waveform colours;
  SELECT shows PLAY, CUE (real press/release), SYNC, MASTER, QUANT, KEY LOCK, VINYL/CDJ and `bpm.` over the waves; dragging a
  lane scrolls/scratches the track (VINYL: touch holds, CDJ: nudges the tempo); ZOOM shows the whole track, two-finger pinch or
  mouse wheel zooms, one finger pans; with SELECT on, a tap sends the playhead there (playing: jumps and plays on, stopped:
  cues from there). New hidden deck control `web_seek`.
- `<< SEARCH` on the DECK page is now `<< START` (back to the start of the track).
- INSTALLER: your `Tracks` folder inside the plugin folder is kept when you install a newer version (0.1.1 replaced the whole
  folder and deleted it). Installed with `--user-data Tracks`.
- BROWSE: the data wheel (and a Q-Link) now scrolls through the whole list, track by track, turning the page by itself at the
  top and bottom of a page. New hidden parameter `br_row` (appended). The row buttons now sit above the row text, so a tap
  reaches them (the text covered them before). The row text always sits exactly on its row, and row 1 shows again.
- BROWSE: BACK goes up, and from the top of a drive to a list of DRIVES (internal drive, SD card, USB); open one to browse it.
- WAVEFORMS / LIVE SCREEN (the overlay) and DIRECT TOUCH: both were dead on the Force in 0.1.0 and 0.1.1 for one reason. The
  live screen finds the page by two marker pixels, and the second one sat at the page's bottom-right corner; the Force's
  display is 1280 x 720 with MPC's 110 px header above the 628 px page, so the page's bottom is below the display and the
  marker was never seen; the search also required the whole page to fit in the picture, so it never even ran. Now both
  markers sit in the top row of the page, only the markers must be in the picture, and the plane is clipped to the display.
  Direct touch (the jog wheels) waits for the same page detection, so it comes alive with it. Still untested on a device.
- The WHEEL / WHEELS pages' TOUCH text now cycles through the plugin's own diagnosis (the touch device and event count, the live
  screen's last log lines), so a photo of the page shows what the logs say.
- FX: the MIX and SUPER knob pictures no longer cover their names.
- BROWSE: a finger dragged up or down over the list scrolls it (one entry per row of travel); the data wheel does the same.
- DECK: hold CUE (or a hot cue) and press PLAY, then let go: the deck keeps playing (Mixxx's play latch). Before, PLAY paused it
  while CUE was held.
- MIXER: the gain, high, mid and low knobs of decks 2, 3 and 4 use the same big knob picture and the same place in their
  column as deck 1; only FILTER stays small. The layout is lined up neatly: one x per column, even spacing, pictures centred on
  their knobs (MIXER, FX).

## 0.1.1 (pre-release, untested on a device)

The first build from the user's own layout (UI editor).

- DECK, BROWSE, MIXER and FX sit where the user put them in the UI editor (`art/ui/reMixXXL_layout.json`, one editable
  layout file); the DECK waveform and overview boxes follow it (the live overlay uses the same boxes).
- The user's knob picture (`art/ui/knob.png`) is every knob, turning in place; where the user placed a bigger picture the
  knob has that size.
- BROWSE: a BACK button (up one folder) and a page number box.
- MIXER: XFDR MODE (SLOW, SMOOTH, FAST, CUT, REV SLOW, REV FAST) drives the Mixxx crossfader curve, constant power and
  reverse.
- WHEELS and WHEEL are unchanged until the jog wheel panels are finished in the editor.

## 0.1.0 (pre-release, untested on a device)

First build of reMixXXL for the Akai Force / MPC OS.

- Four Mixxx-style decks: play, cue, sync, master, slip, reverse, keylock, quantize, vinyl mode, eight hot cues, loop
  in / out / reloop / halve / double, beat loops 4 and 8, beat jump, track search, search, eject, tempo with reset and
  range, jog feel, key sync.
- Mixer: gain, three-band EQ, filter, volume, PFL, crossfader side and FX sends per deck; master, cue mix, crossfader;
  VU meters.
- Two effect units of three slots each (NONE, ECHO, FILTER, REVERB, FLANGER, PHASER, BITCRUSH, MOOG LP, LOOP ROLL),
  meta and super knobs, dry / wet, master and per-deck enables.
- Track browser: folders, six rows a page, load to deck 1-4, sort by name / BPM / artist, a scroll Q-Link; WAV, FLAC
  and MP3; beat grid, BPM and key analysis in the background.
- Seven pages: OVERVIEW, DECK, BROWSE, MIXER, FX, WHEELS (two CDJ decks from the CDJ photo), WHEEL (one, full size);
  waveforms, overview strips, VU meters and the spinning platter label on the live overlay plane; the platter read
  from the touchscreen (scratch, nudge, bend).
- MIDI: Mixxx controller mappings (`.midi.xml` + scripts in QuickJS), the Denon MC3000 mapping included; MIDI learn.
- Skin from `tools/remix_skin.py` on the user's panel texture and part crops; every label 26 px or larger; knobs and
  faders as in-place filmstrips; Q-Link banks per page.
- Packaging with the escaped L'Cronx installers (release.py), `tested.json` empty until a device test.
