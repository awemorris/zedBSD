#!/bin/sh
# ws169-p000: builds and runs the host test of Mail's view (host-mailer.c) with libkeiland's drawing, text and
# widgets and libtruetype, on Linux, and turns the pictures into PNG files.
#   sh plan/ws169/tests/run-host-mailer.sh [OUTPUT]   (default build/ws169/host-mailer; pictures beside it)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/ws169/host-mailer}
dir=$(dirname "$out")
mkdir -p "$dir/inc"
mkdir -p "$dir/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$dir/inc/truetype/"
mkdir -p "$dir/inc/keiland"
cp userland/desktop/include/keiland/keiland.h "$dir/inc/keiland/"
ln -sfn "$(pwd)/include/libc/compat" "$dir/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
L=$U/libkeiland
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I"$dir/inc" -I. -I$K -I$U/libtruetype \
	plan/ws169/tests/host-mailer.c plan/ws169/tests/host-mailer-data.c $U/mailer/view.c $U/mailer/store.c \
	$K/canvas.c $K/drop-look.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/text-area.c $K/list.c $K/cards.c \
	$L/gesture.c $L/motion.c $L/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm -o "$out"
F=$U/fonts
"$out" $F/Mahora-Regular.ttf $F/DroidSansFallbackFull.ttf "$out"
for p in "$out"-*.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
# The frames on glass laid on a wallpaper as zdesktop would, roughly: under each panel the wallpaper blurred and
# lightened, the frame over it by its alpha (premultiplied).
for p in "$out"-glass*.pam; do
	python3 - "$p" "${p%.pam}.panels" userland/desktop/wallpapers/Birch-Lake.png "${p%.pam}.png" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
data = open(sys.argv[1], 'rb').read()
head, body = data.split(b'ENDHDR\n', 1)
fields = dict(line.split(' ', 1) for line in head.decode().splitlines()[1:])
w, h = int(fields['WIDTH']), int(fields['HEIGHT'])
frame = Image.frombytes('RGBA', (w, h), body)
wall = Image.open(sys.argv[3]).convert('RGB').resize((w + 160, h + 100)).crop((80, 50, 80 + w, 50 + h))
frosted = Image.blend(wall.filter(ImageFilter.GaussianBlur(18)), Image.new('RGB', (w, h), (255, 255, 255)), 0.45)
mask = Image.new('L', (w, h), 0)
draw = ImageDraw.Draw(mask)
for line in open(sys.argv[2]):
    x, y, pw, ph, r = map(int, line.split())
    draw.rounded_rectangle((x, y, x + pw - 1, y + ph - 1), radius=r, fill=255)
ground = Image.composite(frosted, wall, mask)
# The frame is premultiplied: out = frame + ground * (1 - alpha).
r, g, b, a = frame.split()
inv = a.point(lambda v: 255 - v)
gr, gg, gb = ground.split()
out = Image.merge('RGB', [ImageChops.add(c, ImageChops.multiply(gc, inv)) for c, gc in ((r, gr), (g, gg), (b, gb))])
out.save(sys.argv[4])
PY
done
