#!/bin/sh
# reMixXXL: build and run the offline screen test (native, x86: it exercises screen.c + font.c only, nothing of the
# device; the Force binary is built by the release tooling). Writes build/screen_test/*.png and prints ms a frame.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/screen_test
CC="${CC:-cc}"
$CC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc -Ithird_party \
    src/screen.c src/font.c test/test_screen.c -lm -o build/screen_test/test_screen
./build/screen_test/test_screen build/screen_test third_party/fonts/TitilliumWeb-SemiBold.ttf
