#!/bin/sh
# Host check of V7's 1x1 noop images against WS141's pinned packet XML.
# Usage: plan/ws141/tests/noop-host-test.sh [build-dir] [pinned-XML]
set -eu
out=${1:-build/ws141-noop-host}
xml=${2:-plan/ws141/temp/mesa/src/broadcom/cle/v3d_packet.xml}
mkdir -p "$out"
cc -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
	plan/ws141/tests/noop-host-test.c src/drivers/gpu/bcm2711/cl.c \
	-o "$out/noop-host-test"
"$out/noop-host-test" > "$out/noop-host.log"
python3 plan/ws141/tests/noop-packet-check.py "$xml" "$out/noop-host.log"
tail -1 "$out/noop-host.log"
