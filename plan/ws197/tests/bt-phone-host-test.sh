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
status=0
# Each test program: its name and its sources besides the test.
for test in "bt-phone-host-test userland/base/bluetoothd/rfcomm.c" \
	"bt-obex-host-test userland/base/bluetoothd/obex.c"; do
	set -- $test
	name=$1
	shift
	if ! cc $flags -o "$OUT/$name" "plan/ws197/tests/$name.c" "$@"; then
		echo "$name: build FAILED"
		status=1
		continue
	fi
	if ! timeout 300 "$OUT/$name"; then
		status=1
	fi
done
if [ $status -eq 0 ]; then
	echo "bt-phone-host-test: PASS"
else
	echo "bt-phone-host-test: FAIL"
	exit 1
fi
