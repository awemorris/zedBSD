#!/bin/sh
# Complete pass packet/geometry oracle, no native launch or physical GPU claims.
# Usage: sh plan/ws141/tests/native-pass-host-test.sh [output-directory] [pinned-XML]
set -eu
out=${1:-build/ws141-native-pass-host}
xml=${2:-plan/ws141/temp/mesa/src/broadcom/cle/v3d_packet.xml}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/native-pass-host-test.c \
    src/drivers/gpu/bcm2711/native-pass.c -o "$out/native-pass-host-test"
"$out/native-pass-host-test" > "$out/native-pass-host.log"
python3 plan/ws141/tests/native-pass-check.py "$xml" "$out/native-pass-host.log"
tail -1 "$out/native-pass-host.log"
