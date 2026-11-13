#!/bin/sh
# ws113-p004a: builds and runs the host test of the compositor's output moved to another display and of the follow of
# the displays with its hotplug thread (host-output-switch.c with userland/desktop/wayland/output-switch.c and threads.c)
# as gnu89, plainly and under ASan/UBSan, in a new directory under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws113/tests/host-output-switch.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws113-output-switch"
work=$fresh_dir
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	${CC:-cc} -std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g $extra \
		-I"$work/include" -I"$repo" -I"$repo/include" -I"$repo/userland/desktop/wayland" -I"$repo/userland/desktop/include" \
		"$repo/plan/ws113/tests/host-output-switch.c" "$repo/userland/desktop/wayland/output-switch.c" \
		"$repo/userland/desktop/wayland/threads.c" -pthread -o "$work/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
echo "WS113 p004a output switch host test PASS (plain, ASan/UBSan)"
