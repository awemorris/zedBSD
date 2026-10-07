#!/bin/sh
# ws177-p011: builds libpdf (with libz-compat, libjpeg-compat and libtruetype) and host-clean-forms with the host's C
# compiler (plain, then ASan and UBSan) as plan/ws175/tests/run-host-clean.sh does, writes make-clean-forms.py's sample
# and runs host-clean-forms on it; the clean copy is checked with qpdf --check and drawn as the sample is (pdftoppm).
# A new folder each run under build/tmp (Q1's cleaning removes old ones).
#   sh plan/ws177/tests/host-clean-forms.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
cd "$repo"
fresh_out "$repo/build/tmp/ws177-clean-forms"
out=$fresh_dir
cc=${CC:-cc}
mkdir -p "$out/include"
ln -sfn "$repo/include/libc/compat" "$out/include/compat"
for header in pdf.h sha2.h md5.h sha1.h; do ln -sf "$repo/include/libc/$header" "$out/include/$header"; done
ln -sfn "$repo/userland/desktop/include/truetype" "$out/include/truetype"
python3 -I plan/ws177/tests/make-clean-forms.py "$out/sample.pdf"
status=0
for variant in plain sanitized; do
	flags="-std=c89 -pedantic -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I$out/include"
	if [ "$variant" = sanitized ]; then
		flags="$flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
	fi
	loose=$(echo "$flags" | sed 's/-std=c89 -pedantic/-std=gnu11/; s/-Werror//')
	objects=
	"$cc" $loose -c src/libc/openbsd-sha2.c -o "$out/sha2-$variant.o"
	"$cc" $loose -w -c src/libc/openbsd-digest.c -o "$out/digest-$variant.o"
	objects="$objects $out/sha2-$variant.o $out/digest-$variant.o"
	for file in userland/base/libz-compat/*.c userland/base/libjpeg-compat/*.c userland/desktop/libtruetype/*.c; do
		object="$out/$(basename "$(dirname "$file")")-$(basename "$file" .c)-$variant.o"
		"$cc" $loose -w -I"$(dirname "$file")" -c "$file" -o "$object"
		objects="$objects $object"
	done
	# shellcheck disable=SC2086
	"$cc" $flags -Wno-overlength-strings -Iuserland/base/libpdf userland/base/libpdf/*.c plan/ws177/tests/host-clean-forms.c \
		$objects -lm -o "$out/host-clean-forms-$variant"
	if ! "$out/host-clean-forms-$variant" "$out/sample.pdf" "$out/clean-$variant.pdf"; then
		status=1
		continue
	fi
	if ! qpdf --check "$out/clean-$variant.pdf" > "$out/qpdf-$variant.txt" 2>&1; then
		echo "qpdf --check $variant: FAIL"
		cat "$out/qpdf-$variant.txt"
		status=1
	fi
	pdftoppm -r 72 "$out/sample.pdf" "$out/sample-$variant"
	pdftoppm -r 72 "$out/clean-$variant.pdf" "$out/clean-$variant"
	if ! cmp -s "$out/sample-$variant-1.ppm" "$out/clean-$variant-1.ppm"; then
		echo "pdftoppm $variant: the clean copy draws differently"
		status=1
	else
		echo "ok pdftoppm $variant: the clean copy draws as the sample"
	fi
done
[ "$status" = 0 ] && echo "host-clean-forms: PASS" || echo "host-clean-forms: FAIL"
exit "$status"
