#!/bin/sh
# Complete V8 clear/store packet images against WS141's pinned 4.2 XML.
# Usage: sh plan/ws141/tests/clear-host-test.sh [output-directory] [pinned-XML]
set -eu
out=${1:-build/ws141-clear-host}
xml=${2:-plan/ws141/temp/mesa/src/broadcom/cle/v3d_packet.xml}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
    plan/ws141/tests/clear-host-test.c src/drivers/gpu/bcm2711/cl.c \
    -o "$out/clear-host-test"
"$out/clear-host-test" > "$out/clear-host.log"
python3 plan/ws141/tests/clear-packet-check.py "$xml" "$out/clear-host.log"
tail -1 "$out/clear-host.log"
