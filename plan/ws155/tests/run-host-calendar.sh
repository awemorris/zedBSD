#!/bin/sh
# ws155-p000, p003: builds and runs the host test of Calendar's view (host-calendar.c) with its 3D renderer, libkeiland's
# drawing, text and widgets and libtruetype, on Linux, and turns the pictures into PNG files (the frames on glass laid
# on a wallpaper, roughly as zdesktop shows them) and the desk calendar's frames into an animated GIF.
#   sh plan/ws155/tests/run-host-calendar.sh [OUTPUT]   (default build/ws155/host-calendar; pictures beside it)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/ws155/host-calendar}
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
C=$U/calendar
cc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I"$dir/inc" -I. -I$K -I$U/libtruetype \
	plan/ws155/tests/host-calendar.c plan/ws155/tests/host-calendar-data.c $C/view.c $C/store.c $C/date.c $C/scene.c $C/render3d.c \
	$K/canvas.c $K/text.c $K/icons.c $K/icons-line.c $K/theme.c $K/input.c $K/scroll.c $K/scroll-bar.c \
	$K/text-touch.c $K/text-bar.c $K/text-select.c $K/ui.c $K/widgets.c $K/field.c $K/text-area.c $K/list.c $K/cards.c \
	$L/gesture.c $L/motion.c $L/scroll.c \
	$U/libtruetype/*.c $U/picture/color-glyph.c \
	userland/base/libz-compat/inflate.c userland/base/libz-compat/checksum.c userland/base/libpng-compat/read.c -lm -o "$out"
F=$U/fonts
folder=$(mktemp -d "$dir/calendar-data.XXXXXX")
"$out" $F/Mahora-Regular.ttf $F/DroidSansFallbackFull.ttf "$out" "$folder/Calendar"
for p in "$out"-*.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
for p in "$out"-*.pam; do
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
r, g, b, a = frame.split()
inv = a.point(lambda v: 255 - v)
gr, gg, gb = ground.split()
out = Image.merge('RGB', [ImageChops.add(c, ImageChops.multiply(gc, inv)) for c, gc in ((r, gr), (g, gg), (b, gb))])
out.save(sys.argv[4])
PY
done
# The small desk calendar's page turning (the day's card at the panel's bottom) as one animated GIF.
python3 - "$out" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
names = ['flip-%d' % i for i in range(6)]
frames = [Image.open('%s-%s.png' % (out, n)).crop((968, 590, 1268, 788)) for n in names]
frames[0].save(out + '-desk.gif', save_all=True, append_images=frames[1:], duration=[600] + [110] * 4 + [900], loop=0)
PY
