#!/bin/sh
# Integer clear conversion against an independent host IEEE oracle; no GPU claims.
# Usage: sh plan/ws141/tests/native-colour-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-native-colour-host}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/native-colour-host-test.c \
    src/drivers/gpu/bcm2711/native-colour.c -lm -o "$out/native-colour-host-test"
"$out/native-colour-host-test"
