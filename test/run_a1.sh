#!/bin/sh
# native (x86) check of decode.c + analyze.c (+ analyze_qm.cpp and the qm-dsp sources in src/qm_sources.txt)
set -e; cd "$(dirname "$0")/.."; O=${TMPDIR:-/tmp}/rx_test_a1; mkdir -p "$O"; OBJS=""
for f in $(cat src/qm_sources.txt); do case $f in *.c) python3 -m ziglang cc -O2 -w -std=gnu11 -Dkiss_fft_scalar=double -Ithird_party/qm-dsp -c "$f" -o "$O/$(basename "$f").o";; *) python3 -m ziglang c++ -O2 -w -std=c++17 -Dkiss_fft_scalar=double -Ithird_party/qm-dsp -Ithird_party/qm-dsp/include -c "$f" -o "$O/$(basename "$f").o";; esac; OBJS="$OBJS $O/$(basename "$f").o"; done
python3 -m ziglang c++ -O2 -std=c++17 -Dkiss_fft_scalar=double -Isrc -Ithird_party/qm-dsp -c src/analyze_qm.cpp -o "$O/analyze_qm.o"
python3 -m ziglang cc -O2 -g -std=gnu11 -w -Isrc -Ithird_party -Ithird_party/dr_libs -Ithird_party/minimp3 -c src/decode.c -o "$O/decode.o"
python3 -m ziglang cc -O2 -g -std=gnu11 -Wall -Isrc -c src/analyze.c -o "$O/analyze.o"
python3 -m ziglang cc -O2 -g -std=gnu11 -Wall -Isrc -c test/test_analyze.c -o "$O/test.o"
python3 -m ziglang c++ -o "$O/test_a1" "$O/test.o" "$O/decode.o" "$O/analyze.o" "$O/analyze_qm.o" $OBJS -lm
"$O/test_a1"
