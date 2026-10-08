#!/bin/sh
# ws190-p002: builds and runs the host tests of the fingers' selection's bar (host-text-bar.c with libkeiland's
# text-bar.c, text-touch.c, ui.c and the widgets, as plan/ws177/tests/host-text-edit.sh links them), plainly and under
# ASan/UBSan, in a new directory under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws190/tests/run-host.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
cd "$repo"
fresh_out "$repo/build/tmp/ws190-host"
work=$fresh_dir
mkdir -p "$work/inc/truetype" "$work/inc/keiland"
cp userland/desktop/include/truetype/truetype.h "$work/inc/truetype/"
cp userland/desktop/include/keiland/keiland.h "$work/inc/keiland/"
ln -sfn "$repo/include/libc/compat" "$work/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
for test in host-text-bar host-touch-select; do
	for mode in plain sanitize; do
		extra=
		if test "$mode" = sanitize; then
			extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
		fi
		# shellcheck disable=SC2086
		cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $extra -I"$work/inc" -I. -I$K -I$U/libtruetype \
			plan/ws190/tests/$test.c \
			$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
			$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/text-area.c $K/list.c $K/cards.c \
			$U/libkeiland/gesture.c $U/libkeiland/motion.c $U/libkeiland/scroll.c \
			$U/libtruetype/*.c $U/picture/color-glyph.c \
			userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm \
			-o "$work/$test-$mode"
		timeout 60 "$work/$test-$mode" $U/fonts/Mahora-Regular.ttf
	done
done
echo "ws190-host: PASS"
