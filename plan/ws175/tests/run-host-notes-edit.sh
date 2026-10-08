#!/bin/sh
# ws175-p007, p008: builds Notes' model (document.c, edit.c, encode.c, journal.c, save.c) with the whole of libpdf (and
# libz-compat, libjpeg-compat, libtruetype) with the host's C compiler (plain, ASan and UBSan), writes edit-images.pdf
# (make-edit-samples.py), a JPEG and a PNG, runs host-notes-edit with each build in a folder of its own, and checks the
# notebook it saves with qpdf --check; p008: host-notes-picture (picture-file.c with picture.c, libpng-compat and
# libgif-compat) on image files ImageMagick writes.
#   sh plan/ws175/tests/run-host-notes-edit.sh [OUTPUT]   (default build/ws175-notes)
# Each run gets a new directory behind OUTPUT (plan/tools/fresh-out.sh); nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws175-notes}
cc=${CC:-cc}
. plan/tools/fresh-out.sh
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
ln -sf "$(pwd)/include/libc/pdf.h" "$out/include/pdf.h"
ln -sf "$(pwd)/include/libc/sha2.h" "$out/include/sha2.h"
ln -sf "$(pwd)/include/libc/md5.h" "$out/include/md5.h"
ln -sf "$(pwd)/include/libc/sha1.h" "$out/include/sha1.h"
mkdir -p "$out/include/truetype"
ln -sf "$(pwd)/userland/desktop/include/truetype/truetype.h" "$out/include/truetype/truetype.h"
# ws175-p005: the replacement fonts as the desktop installs them, in a folder of the test's own.
mkdir -p "$out/fonts"
ln -sf "$(pwd)/userland/desktop/fonts/Mahora-Regular.ttf" "$out/fonts/keiland.ttf"
ln -sf "$(pwd)/userland/desktop/fonts/Mahora-Mono.ttf" "$out/fonts/keiland-mono.ttf"
ln -sf "$(pwd)/userland/desktop/fonts/JetBrainsMono-Regular.ttf" "$out/fonts/keiland-fallback-mono.ttf"
ln -sf "$(pwd)/userland/desktop/fonts/DroidSansFallbackFull.ttf" "$out/fonts/keiland-fallback.ttf"
fonts=$(cd "$out/fonts" && pwd)
sources="userland/desktop/notes/document.c userland/desktop/notes/edit.c userland/desktop/notes/encode.c
	userland/desktop/notes/journal.c userland/desktop/notes/save.c
	userland/base/libpdf/writer.c userland/base/libpdf/update.c userland/base/libpdf/clean.c userland/base/libpdf/outline.c userland/base/libpdf/object.c
	userland/base/libpdf/reader.c userland/base/libpdf/filter.c userland/base/libpdf/ccitt.c userland/base/libpdf/crypt.c
	userland/base/libpdf/image.c userland/base/libpdf/display.c userland/base/libpdf/content.c userland/base/libpdf/editor.c
	userland/base/libpdf/tounicode.c userland/base/libpdf/intake.c userland/base/libpdf/replace.c userland/base/libpdf/embed.c userland/base/libpdf/subset.c userland/base/libpdf/stroke.c userland/base/libpdf/raster.c
	userland/base/libpdf/font.c userland/base/libpdf/encoding.c userland/base/libpdf/shading.c userland/base/libpdf/charstrings.c
	userland/base/libpdf/type1.c userland/base/libpdf/cff.c userland/base/libpdf/cffdata.c plan/ws175/tests/host-notes-edit.c"
status=0
for variant in plain asan ubsan; do
	flags="-std=c99 -pedantic -O1 -g -Wall -Wextra -Werror -Wno-overlength-strings -D_DEFAULT_SOURCE -I$out/include -Iuserland/desktop/notes -DPDF_EDIT_FONT_DIRECTORY=\"$fonts\""
	if [ "$variant" = asan ]; then
		flags="$flags -fsanitize=address -fno-omit-frame-pointer"
	fi
	if [ "$variant" = ubsan ]; then
		flags="$flags -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all"
	fi
	loose=$(echo "$flags" | sed 's/-std=c99 -pedantic/-std=gnu11/; s/-Werror//')
	objects=
	"$cc" $loose -c src/libc/openbsd-sha2.c -o "$out/sha2-$variant.o"
	"$cc" $loose -w -c src/libc/openbsd-digest.c -o "$out/digest-$variant.o"
	objects="$objects $out/sha2-$variant.o $out/digest-$variant.o"
	for file in userland/base/libz-compat/*.c userland/base/libjpeg-compat/*.c userland/desktop/libtruetype/*.c \
		userland/base/libpng-compat/*.c userland/base/libgif-compat/*.c userland/desktop/picture/picture.c userland/desktop/picture/png-write.c; do
		object="$out/$(basename "$(dirname "$file")")-$(basename "$file" .c)-$variant.o"
		"$cc" $loose -w -I"$(dirname "$file")" -c "$file" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086
	"$cc" $flags $sources $objects -lm -o "$out/host-notes-edit-$variant"
	model=$(echo "$sources" | sed 's|plan/ws175/tests/host-notes-edit.c||')
	# shellcheck disable=SC2086
	"$cc" $flags $model userland/desktop/notes/picture-file.c plan/ws175/tests/host-notes-picture.c $objects -lm -o "$out/host-notes-picture-$variant"
	mkdir -p "$out/$variant"
	python3 plan/ws175/tests/make-edit-samples.py "$out/$variant" >/dev/null
	convert -size 8x4 gradient:blue-green -quality 90 "$out/$variant/insert.jpg"
	convert -size 7x5 gradient:red-yellow -depth 8 "PNG24:$out/$variant/insert.png"
	if "$out/host-notes-edit-$variant" "$out/$variant" > "$out/notes-edit-$variant.txt" 2>&1; then
		echo "host-notes-edit $variant: $(tail -1 "$out/notes-edit-$variant.txt")"
	else
		grep -v '^ok' "$out/notes-edit-$variant.txt"
		status=1
	fi
	mkdir -p "$out/$variant/pictures"
	pictures="$out/$variant/pictures"
	convert -size 8x4 gradient:blue-green -quality 90 "$pictures/plain.jpg"
	# An APP1 block of EXIF with the orientation 6 (turned a quarter clockwise) after the JPEG's start.
	python3 - "$pictures/plain.jpg" "$pictures/turned.jpg" <<'PYTHON'
import struct, sys
data = open(sys.argv[1], "rb").read()
tiff = b"MM\x00\x2a" + struct.pack(">I", 8) + struct.pack(">H", 1) + struct.pack(">HHIHH", 0x0112, 3, 1, 6, 0) + struct.pack(">I", 0)
block = b"Exif\x00\x00" + tiff
open(sys.argv[2], "wb").write(data[:2] + b"\xff\xe1" + struct.pack(">H", len(block) + 2) + block + data[2:])
PYTHON
	convert -size 8x4 gradient:blue-green -colorspace CMYK -quality 90 "$pictures/cmyk.jpg"
	convert -size 7x5 gradient:red-yellow -depth 8 "PNG24:$pictures/rgb.png"
	convert -size 6x3 gradient:red-blue -alpha set -channel A -evaluate set 50% +channel "PNG32:$pictures/rgba.png"
	convert -size 5x4 xc:red -fill blue -draw "point 1,1" "PNG8:$pictures/palette.png"
	echo "not an image" > "$pictures/text.txt"
	if "$out/host-notes-picture-$variant" "$pictures" > "$out/notes-picture-$variant.txt" 2>&1; then
		echo "host-notes-picture $variant: $(tail -1 "$out/notes-picture-$variant.txt")"
	else
		grep -v '^ok' "$out/notes-picture-$variant.txt"
		status=1
	fi
	# The only error qpdf may find is the sample's own: page 3's stream of a filter no reader decodes here.
	errors=$(qpdf --check "$out/$variant/notes-edit.pdf" 2>&1 | grep 'ERROR' | grep -v 'page 4: content stream' || true)
	if [ -z "$errors" ]; then
		echo "qpdf --check $variant/notes-edit.pdf: ok (the sample's unreadable stream only)"
	else
		echo "$errors"
		status=1
	fi
done
[ $status -eq 0 ] && echo "run-host-notes-edit: PASS" || echo "run-host-notes-edit: FAIL"
exit $status
