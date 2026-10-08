#!/bin/sh
# ws177-p040 to p043 (case L): builds libpdf (the zedBSD build's C89, plain and ASan+UBSan) with PDF Viewer's core
# (view, draw, document, canvas, text, find) and host-pdf-find-l.c for the host, writes the samples
# (make-find-l.py) and runs the groups: forms (libpdf's page text with the forms' text), find (the matching rules, the
# count, the unreadable characters), select (across pages, words and lines, all, the fingers' handles, the turned
# characters' marks), bar (the find field inside the window).  The frames with the marks are written as PNG.
#   sh plan/ws177/tests/host-pdf-find-l.sh [OUTPUT]   (default build/ws177-pdf-find-l)
# Each run gets a new directory behind OUTPUT (plan/tools/fresh-out.sh); nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws177-pdf-find-l}
cc=${CC:-cc}
font=userland/desktop/fonts/Mahora-Regular.ttf
. plan/tools/fresh-out.sh
fresh_out "$out"
mkdir -p "$out/include/truetype"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
for header in pdf.h sha2.h md5.h sha1.h; do
	ln -sf "$(pwd)/include/libc/$header" "$out/include/$header"
done
ln -sf "$(pwd)/userland/desktop/include/truetype/truetype.h" "$out/include/truetype/truetype.h"
python3 plan/ws177/tests/make-find-l.py "$out/samples"
libpdf=$(ls userland/base/libpdf/*.c)
viewer="userland/desktop/pdfviewer/view.c userland/desktop/pdfviewer/draw.c userland/desktop/pdfviewer/document.c
	userland/desktop/pdfviewer/canvas.c userland/desktop/pdfviewer/text.c userland/desktop/pdfviewer/find.c"
status=0
for variant in plain asan; do
	flags="-std=c89 -pedantic -O1 -g -Wall -Wextra -Werror -Wno-long-long -Wno-overlength-strings -D_DEFAULT_SOURCE -I. -I$out/include"
	if [ "$variant" = asan ]; then
		flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
	fi
	loose=$(echo "$flags" | sed 's/-std=c89 -pedantic/-std=gnu11/; s/-Werror//')
	objects="$out/sha2-$variant.o $out/digest-$variant.o"
	"$cc" $loose -c src/libc/openbsd-sha2.c -o "$out/sha2-$variant.o"
	"$cc" $loose -w -c src/libc/openbsd-digest.c -o "$out/digest-$variant.o"
	for file in userland/base/libz-compat/*.c userland/base/libjpeg-compat/*.c userland/desktop/libtruetype/*.c; do
		object="$out/$(basename "$(dirname "$file")")-$(basename "$file" .c)-$variant.o"
		"$cc" $loose -w -I"$(dirname "$file")" -c "$file" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086
	"$cc" $flags $libpdf $viewer plan/ws177/tests/host-pdf-find-l.c $objects -lm -o "$out/host-pdf-find-l-$variant"
	mkdir -p "$out/frames-$variant"
	if "$out/host-pdf-find-l-$variant" "$font" "$out/samples" "$out/frames-$variant" > "$out/find-l-$variant.log" 2>&1; then
		echo "host-pdf-find-l $variant: $(grep '^host-pdf-find-l' "$out/find-l-$variant.log")"
	else
		grep -E "^(FAILED|host-pdf-find-l)" "$out/find-l-$variant.log" || tail -5 "$out/find-l-$variant.log"
		status=1
	fi
done
for picture in "$out"/frames-plain/*.ppm; do
	[ -e "$picture" ] || continue
	convert "$picture" "${picture%.ppm}.png"
done
[ $status -eq 0 ] && echo "host-pdf-find-l: PASS" || echo "host-pdf-find-l: FAIL"
exit $status
