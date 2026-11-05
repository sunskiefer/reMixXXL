# Map any controller (0.1.13, not yet tested on a Force)

Two parts: the MAPS page on the Force screen loads a controller map, and the map builder web page makes one for any controller.

## MAPS page (on the Force)
Press the BROWSE tab key twice: the second page is MAPS. It lists every `.midi.xml` in the plugin's `Mappings` folder (up to 512), six rows a page.
- Tap a row (or turn the data wheel) to select it, then LOAD. `*` marks the loaded map. NONE unloads it. < PAGE and PAGE > turn pages.
- The status line says what is loaded: number of controls and how many of them this plugin does not have.
- The controller you loaded last is remembered (`midi_pick.txt` next to the plugin) and loads again at the next start.

## Map builder (phone or PC)
Open `http://<Force IP>:6731/map` (the address is on the WHEEL page QR code and in `remixxxl_diag.log`, `web: companion page at ...`; if port 6731 is taken the log shows the real port). Phone or PC and Force on the same network. The controller is connected to the Force and selected as the MIDI input of the reMix track.
1. Choose a filter (Deck 1, Mixer, FX...) or search, tap a control, then press or turn the matching control on your controller. The top of the page shows every message the Force receives.
2. AUTO NEXT moves to the next control by itself, so you can walk down the list.
3. The type (button, fader/knob, jog or encoder) is guessed from what the controller sends; change it in the table if needed.
4. COPY DECK 1 TO DECKS 2-4 fills in the other decks by shifting the MIDI channel, or the note/CC number by a step you set.
5. Give the map a name, then SAVE TO FORCE (writes `Mappings/<name>.midi.xml` and loads it) or DOWNLOAD .midi.xml. IMPORT opens an existing `.midi.xml` for editing.
6. "On the Force now" lists the controls of the loaded map that this plugin does not have.

Maps can also be shared: put the `.midi.xml` (and a script file if it needs one) on git, others copy them into `Mappings`.

Notes: saving from the page needs no password (anyone on the network who opens the page can write a `.midi.xml` into `Mappings`, nothing else). Script-based Mixxx maps work as in 0.1.12; the builder makes plain maps without scripts, and LED feedback needs a map with outputs.

## Saved copy (0.1.15)

`reMixXXL-map-builder.html` (in the repo root and attached to the 0.1.15 release) is the same builder as a file. Open it in a browser on your computer, type the Force's address (port 6731) and press CONNECT. Its Mixer tab lists exactly what the plugin's MIXER page has. The browser blocks saving to the Force from a file, so SAVE TO FORCE is hidden there: press DOWNLOAD, copy the `.midi.xml` into the plugin's `Mappings` folder, and load it on the MAPS page.
