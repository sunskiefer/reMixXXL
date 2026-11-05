# The source lists, shared by build.sh and the test scripts.
# The plugin (C, gnu11): engine.c owns the wrapper contract; the rest are the parts of src/remix.h.
PLUGIN_SRCS="src/engine.c src/control.c src/deck.c src/mixer.c src/fx.c src/decode.c src/analyze.c src/overlay.c src/screen.c src/font.c src/touch.c src/web.c src/qr.c src/midi.c src/midi_xml.c src/midi_js.c src/log.c src/compat.c"
INCS="-Isrc -Ithird_party -Ithird_party/stb -Ithird_party/dr_libs -Ithird_party/minimp3 -Ithird_party/quickjs"

# QuickJS (MIT): the controller scripts' JavaScript. src/quickjs_sources.txt, when present, lists the files to compile.
if [ -f src/quickjs_sources.txt ]; then
  QJS_SRCS="$(grep -v '^#' src/quickjs_sources.txt | tr '\n' ' ')"
else
  QJS_SRCS="third_party/quickjs/quickjs.c third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c third_party/quickjs/dtoa.c"
fi
QJS_FLAGS="-D_GNU_SOURCE -DCONFIG_VERSION=\"2025-04-26\" -DCONFIG_BIGNUM"

# qm-dsp (GPL): beat tracking and key detection, C++17. src/qm_sources.txt lists the files (empty: no qm-dsp in this build).
if [ -f src/qm_sources.txt ]; then
  QM_SRCS="$(grep -v '^#' src/qm_sources.txt | tr '\n' ' ')"
else
  QM_SRCS=""
fi
QM_INCS="-Ithird_party/qm-dsp -Ithird_party/qm-dsp/include -Dkiss_fft_scalar=double"

# the C bridge to qm-dsp (C++17, exceptions caught inside)
QM_SRCS="src/analyze_qm.cpp $QM_SRCS"
