#!/bin/sh
# Native record XML and inverse pixel layout checks; no physical GPU claims.
# Usage: sh plan/ws141/tests/native-state-host-test.sh [output-directory] [pinned-XML]
set -eu
out=${1:-build/ws141-native-state-host}
xml=${2:-plan/ws141/temp/mesa/src/broadcom/cle/v3d_packet.xml}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/native-state-host-test.c \
    src/drivers/gpu/bcm2711/native-bin.c src/drivers/gpu/bcm2711/native-shader.c src/drivers/gpu/bcm2711/native-texture.c \
    src/drivers/gpu/bcm2711/native-viewport.c -lm -o "$out/native-state-host-test"
"$out/native-state-host-test" > "$out/native-state-host.log"
python3 plan/ws141/tests/native-state-check.py "$xml" "$out/native-state-host.log"
tail -1 "$out/native-state-host.log"
