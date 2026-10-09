#!/bin/sh
# The host test of bluetoothd's phone link parts (ws197-p002, plan/ws197/phase002/phase.md section 12.1): builds
# userland/base/bluetoothd's RFCOMM, OBEX and SDP server parts with the host's compiler under ASan and UBSan and runs
# plan/ws197/tests/bt-phone-host-test.c.  The last line is "bt-phone-host-test: PASS" or "... FAIL".
# usage: plan/ws197/tests/bt-phone-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${OUT:-build/ws197-phone-host}
mkdir -p "$OUT"
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I."
if ! cc $flags -o "$OUT/bt-phone-host-test" plan/ws197/tests/bt-phone-host-test.c userland/base/bluetoothd/rfcomm.c; then
	echo "bt-phone-host-test: FAIL (build)"
	exit 1
fi
if timeout 300 "$OUT/bt-phone-host-test"; then
	echo "bt-phone-host-test: PASS"
else
	echo "bt-phone-host-test: FAIL"
	exit 1
fi
