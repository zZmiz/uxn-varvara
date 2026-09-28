#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
UXN_TEST_DIR=$(mktemp -d /tmp/uxn-host.XXXXXX)
trap 'rm -rf "$UXN_TEST_DIR"' EXIT HUP INT TERM
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/audio_clock_test.c -o "$UXN_TEST_DIR/clock"
"$UXN_TEST_DIR/clock"
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/audio_gain_test.c -o "$UXN_TEST_DIR/gain"
"$UXN_TEST_DIR/gain"
clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/screen_scale_test.c -o "$UXN_TEST_DIR/screen-scale"
"$UXN_TEST_DIR/screen-scale"
