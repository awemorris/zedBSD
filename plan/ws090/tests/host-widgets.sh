#!/bin/sh
# ws090-p005: builds and runs the host test of libkeiland's widgets (host-widgets.c) with the drawing layer,
# the input, libkeiland's scroller and gestures and libtruetype, on Linux.  The gallery (the page of
# widgets, and with a dialog) goes to build/ws090-shots.
#   sh plan/ws090/tests/host-widgets.sh [OUTPUT]   (default build/ws090/host-widgets)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/ws090/host-widgets}
shots=build/ws090-shots
mkdir -p "$(dirname "$out")/inc" "$shots"
mkdir -p "$(dirname "$out")/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$(dirname "$out")/inc/truetype/"
mkdir -p "$(dirname "$out")/inc/keiland"
cp userland/desktop/include/keiland/keiland.h "$(dirname "$out")/inc/keiland/"
# ws090-p008: the text's colour emoji (KUI_VERSION 9) read their PNG pictures through picture/color-glyph.c and
# libpng-compat, whose headers are the C library's compat ones.
ln -sfn "$(pwd)/include/libc/compat" "$(dirname "$out")/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I"$(dirname "$out")/inc" -I. -I$K -I$U/libtruetype \
	plan/ws090/tests/host-widgets.c \
	$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/list.c $K/cards.c \
	$U/libkeiland/gesture.c $U/libkeiland/motion.c $U/libkeiland/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm -o "$out"
F=$U/fonts
"$out" $F/Mahora-Regular.ttf $F/DroidSansFallbackFull.ttf "$shots/host-widgets"
for p in "$shots"/host-widgets-*.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
