# Changelog

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
