#!/bin/sh
# ws083 R-S6: builds and runs the host tests of libvulkan's native video path before a device exists:
# host-libvulkan-native.c (instance.c and device.c compiled into it, against a stand-in transport) and
# host-libvulkan-capset.c (context.c's reading of the capability record, with open and ioctl stood in), as
# ANSI C (gnu89), plainly and under ASan/UBSan, in a new directory under build/tmp (nothing is removed here;
# Q1's plan/tools/q1-clean.sh removes the runs build/tmp/ws083-libvulkan-native does not point at).
#   sh plan/ws083/tests/run-host-libvulkan-native.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws083-libvulkan-native"
work=$fresh_dir
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
library="$repo/userland/desktop/libvulkan"
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	flags="-std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g -ffunction-sections -fdata-sections $extra -I$work/include -I$repo/include -I$library"
	${CC:-cc} $flags "$repo/plan/ws083/tests/host-libvulkan-native.c" \
		"$library/external-properties.c" "$library/video.c" "$library/objects.c" "$library/wire.c" "$library/codec.c" \
		-Wl,--gc-sections -pthread -o "$work/native-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/native-$mode"
	${CC:-cc} $flags "$repo/plan/ws083/tests/host-libvulkan-capset.c" \
		"$library/context.c" "$library/objects.c" "$library/wire.c" "$library/codec.c" \
		-Wl,--gc-sections -pthread -o "$work/capset-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/capset-$mode"
done
echo "WS083 libvulkan native video path host test PASS (plain, ASan/UBSan)"
