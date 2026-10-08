#!/bin/sh
# ws179-p001: builds and runs host-accent-widgets.c (libkeiland's widgets in the eight accents, light and dark) with
# libkeiland's drawing layer, widgets and libtruetype, and writes the pictures as PNG beside the output.
#   sh plan/ws179/tests/run-host-accent-widgets.sh [OUTPUT]   (default build/ws179/host-accent-widgets)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws179/host-accent-widgets}
inc="$(dirname "$out")/inc"
mkdir -p "$inc/truetype" "$inc/keiland"
cp userland/desktop/include/truetype/truetype.h "$inc/truetype/"
cp userland/desktop/include/keiland/keiland.h "$inc/keiland/"
ln -sfn "$(pwd)/include/libc/compat" "$inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I"$inc" -I. -I$K -I$U/libtruetype \
	plan/ws179/tests/host-accent-widgets.c \
	$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/list.c $K/cards.c \
	$U/libkeiland/gesture.c $U/libkeiland/motion.c $U/libkeiland/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm -o "$out"
timeout 60 "$out" $U/fonts/Mahora-Regular.ttf $U/fonts/DroidSansFallbackFull.ttf "$out"
for p in "$out"-light.ppm "$out"-dark.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
