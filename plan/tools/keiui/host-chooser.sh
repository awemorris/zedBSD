#!/bin/sh
# libkeiland (ws090-p006, from plan/tools/keiland of ws092-p003): builds and runs the host tests of the file
# chooser's model and view (host-chooser.c) with libkeiland's chooser, widgets, drawing and input, libkeiland's
# recent files, scroller and gestures, and libtruetype, on Linux, and turns the pictures into PNG files.
#   sh plan/tools/keiui/host-chooser.sh [OUTPUT]   (default build/keiui/host-chooser; pictures in build/keiui-shots)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/keiui/host-chooser}
shots=build/keiui-shots
mkdir -p "$(dirname "$out")/inc" "$shots"
mkdir -p "$(dirname "$out")/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$(dirname "$out")/inc/truetype/"
mkdir -p "$(dirname "$out")/inc/keiland"
cp userland/desktop/include/keiland/keiland.h "$(dirname "$out")/inc/keiland/"
ln -sfn "$(pwd)/include/libc/compat" "$(dirname "$out")/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
L=$U/libkeiland
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I"$(dirname "$out")/inc" -I. -I$K -I$U/libtruetype \
	plan/tools/keiui/host-chooser.c $K/chooser-model.c $K/chooser-view.c \
	$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/list.c $K/cards.c \
	$L/recent.c $L/gesture.c $L/motion.c $L/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm -o "$out"
F=$U/fonts
"$out" $F/Mahora-Regular.ttf $F/DroidSansFallbackFull.ttf "$shots/host-chooser"
for p in "$shots"/host-chooser-*.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
