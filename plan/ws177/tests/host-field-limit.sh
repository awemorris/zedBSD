#!/bin/sh
# ws177-p004: builds and runs the host test of a field's limit (host-field-limit.c with
# userland/desktop/libkeiland/ui/field.c) as gnu89, plainly and under ASan/UBSan, in a new directory under build/tmp
# (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-field-limit.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-field-limit"
work=$fresh_dir
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	${CC:-cc} -std=gnu89 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g $extra \
		-I"$repo" -I"$repo/userland/desktop/include" -I"$repo/userland/desktop/libkeiland/ui" \
		"$repo/plan/ws177/tests/host-field-limit.c" "$repo/userland/desktop/libkeiland/ui/field.c" \
		-o "$work/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
echo "WS177 p004 field limit host test PASS (plain, ASan/UBSan)"
