#!/bin/sh
# ws177-p001: builds and runs the host test of wl_surface.enter and leave (host-surface-outputs.c with
# userland/desktop/wayland/surface-outputs.c) as gnu89, plainly and under ASan/UBSan, in a new directory under build/tmp
# (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-surface-outputs.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-surface-outputs"
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
		"$repo/plan/ws177/tests/host-surface-outputs.c" "$repo/userland/desktop/wayland/surface-outputs.c" \
		-o "$work/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
echo "WS177 p001 surface outputs host test PASS (plain, ASan/UBSan)"
