#!/bin/sh
# Host test of the console's leaving of the display on the boards other than the PC/AT:
# text-end-boards-host-test.c and text-end-boards-stubs.c with the real src/kern/text-display.c,
# src/kern/klog.c, the PC-98 text layer, /dev/graphics and backend, the Raspberry Pi 4 and X68000
# consoles over the boot framebuffer's text console (src/drivers/generic/boot-fb.c, fb-text.c),
# the VisionFive 2's binding (included by the stubs), the 16550 UART driver, the device
# tree reader and the serial console; ordinary, with the undefined-behaviour sanitizer, and with the
# address and undefined-behaviour sanitizers (without the PC-98 scenes: the PC-98 text memory's
# fixed kernel address lies in the address sanitizer's shadow memory); each scene in its own process.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws290/tests/text-end-boards-host-test.sh   (from anywhere)
# Output goes to a new directory below build/tmp; nothing is removed.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/text-end-boards-host.XXXXXX")
status=0
scenes='1 2 3 4 5 6 7 8 9 10 11 12 13 14 15'
for mode in ordinary undefined address; do
	extra=
	pc98="$repo/src/drivers/platform/pc98/graphics/text.c $repo/src/drivers/platform/pc98/graphics/pc98-graphics.c $repo/src/drivers/platform/pc98/graphics/backend.c"
	case "$mode" in
	undefined)
		extra='-fsanitize=undefined -fno-sanitize=alignment -fno-omit-frame-pointer'
		;;
	address)
		extra='-fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer -DTEXT_END_NO_PC98'
		pc98=
		;;
	esac
	# shellcheck disable=SC2086
	timeout 180 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-DKERN_USER_ABI_LP64 -DKERN_UAPI_NATIVE -DCONFIG_DRIVER_POWERVR=1 \
		-I"$repo/include" -I"$repo/src" -I"$repo/include/libc" \
		"$repo/plan/ws290/tests/text-end-boards-host-test.c" "$repo/plan/ws290/tests/text-end-boards-stubs.c" \
		"$repo/src/kern/text-display.c" "$repo/src/kern/klog.c" \
		"$repo/src/drivers/generic/fdt.c" "$repo/src/drivers/generic/serial-console.c" \
		"$repo/src/drivers/serial/ns16550.c" \
		"$repo/src/drivers/platform/rpi-common/rpi-console.c" "$repo/src/drivers/platform/x68k/x68k-console.c" \
		"$repo/src/drivers/generic/boot-fb.c" "$repo/src/drivers/generic/fb-text.c" \
		"$repo/src/drivers/platform/pcat/graphics/vgafont.c" \
		$pc98 -o "$out/$mode"
	for scene in $scenes; do
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 60 "$out/$mode" "$scene" || status=1
	done
done
if test "$status" -eq 0; then
	echo "PASS text-end-boards-host-test ($out)"
else
	echo "FAIL text-end-boards-host-test ($out)"
fi
exit "$status"
