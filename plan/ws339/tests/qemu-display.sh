#!/bin/sh
# The emulator boot-test.sh runs for p001-qemu.sh (QEMU=this file): qemu-system-x86_64 with the display that
# P001_DISPLAY names (space-separated options, such as -device VGA,edid=on,xres=1600,yres=900) in place of the
# std VGA (-vga std) that boot-test.sh gives; with -vga none the firmware sees only that display.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
display=${P001_DISPLAY:?P001_DISPLAY names the display}
count=$#
index=0
while [ "$index" -lt "$count" ]; do
	argument=$1
	shift
	index=$((index + 1))
	if [ "$argument" = -vga ] && [ "$index" -lt "$count" ] && [ "$1" = std ]; then
		shift
		index=$((index + 1))
		# shellcheck disable=SC2086
		set -- "$@" -vga none $display
		continue
	fi
	set -- "$@" "$argument"
done
exec qemu-system-x86_64 -name p5-ws339 "$@"
