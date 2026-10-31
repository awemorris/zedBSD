#!/bin/sh
# WS333 p019: builds and runs the host test of sessiond's THERMAL rules (host-thermal-rules.c with
# userland/desktop/sessiond/thermal-rules.c) as gnu89, plainly and under ASan/UBSan, in a new directory under
# OUT (default /tmp/ws333-sessiond-thermal; nothing is removed here).
#   sh plan/ws333/tests/sessiond-thermal/run.sh [OUT]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "${1:-/tmp/ws333-sessiond-thermal}"
work=$fresh_dir
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	${CC:-cc} -std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g $extra \
		-I"$repo" \
		"$repo/plan/ws333/tests/sessiond-thermal/host-thermal-rules.c" "$repo/userland/desktop/sessiond/thermal-rules.c" \
		-o "$work/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
echo "WS333 p019 sessiond THERMAL rules host test PASS (plain, ASan/UBSan)"
