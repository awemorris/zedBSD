#!/bin/sh
# ws083-p008: builds and runs the host test of libvulkan's result status query pools (host-libvulkan-status.c with
# query.c), as ANSI C (gnu89), plainly and under ASan/UBSan, in a new directory under build/tmp (nothing is removed
# here; Q1's plan/tools/q1-clean.sh removes the runs build/tmp/ws083-libvulkan-status does not point at).
#   sh plan/ws083/tests/run-host-libvulkan-status.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws083-libvulkan-status"
work=$fresh_dir
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
library="$repo/userland/desktop/libvulkan"
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	${CC:-cc} -std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections $extra \
		-I"$work/include" -I"$repo/include" -I"$library" \
		"$repo/plan/ws083/tests/host-libvulkan-status.c" \
		"$library/query.c" "$library/objects.c" "$library/wire.c" "$library/codec.c" \
		-Wl,--gc-sections -pthread -o "$work/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
echo "WS083 libvulkan result status host test PASS (plain, ASan/UBSan)"
