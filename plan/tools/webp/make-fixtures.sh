#!/bin/sh
# Makes libwebp-compat's test files and their expected outputs with the host's libwebp 1.5.0 tools (Debian's webp
# and libwebp-dev packages; a test oracle on the host only, never in the tree):
#   src/*.pam            the sources (gen-sources.py)
#   files/NAME.webp      each fixture of fixtures.list (cwebp, fixture-encoder, img2webp, craft.py)
#   expect/NAME.pam      dwebp -nodither -pam (RGBA, fancy upsampling)          when dwebp decodes the file
#   expect/NAME.nofancy.pam   dwebp -nodither -nofancy -pam
#   expect/NAME.yuv      dwebp -nodither -yuv (Y, U, V and, with alpha, A)       for lossy files
#   expect/NAME.nofilter.yuv  dwebp -nodither -nofilter -yuv
#   expect/NAME.ref      the reference driver's report (webp-driver.c built against libwebp)
#   fixtures.sha256      the SHA-256 of every files/*.webp, compared with plan/tools/webp/fixtures.sha256
# into a fresh build/tmp/webp-fixtures (a link to the run directory).  The files are the same on every host with
# the same libwebp; a difference from the recorded list is reported (and is not a failure on its own).
#   sh plan/tools/webp/make-fixtures.sh
# Last line: make-fixtures: PASS (<n> files ...) or make-fixtures: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
tests=plan/tools/webp
out=build/tmp/webp-fixtures
fresh_out "$out"
mkdir -p "$out/src" "$out/files" "$out/expect" "$out/bin"

# The tools must be libwebp 1.5.0's (the version the expected outputs and the compat interface are of).
for tool in cwebp dwebp img2webp; do
	if ! command -v "$tool" > /dev/null 2>&1; then
		echo "make-fixtures: FAIL ($tool missing: sudo apt-get install webp libwebp-dev)"
		exit 1
	fi
done
version=$(dwebp -version 2>/dev/null | head -1)
if [ "$version" != "1.5.0" ]; then
	echo "make-fixtures: FAIL (dwebp $version, not 1.5.0)"
	exit 1
fi

# The sources, the encoder and the reference driver.
timeout 600 python3 -I $tests/gen-sources.py "$out/src" > "$out/gen.log" 2>&1 || { echo "make-fixtures: FAIL (gen-sources)"; exit 1; }
cc -std=c99 -O1 -Wall -Wextra -Werror $tests/fixture-encoder.c -lwebp -o "$out/bin/fixture-encoder" || {
	echo "make-fixtures: FAIL (fixture-encoder build)"; exit 1; }
cc -std=c99 -O1 -Wall -Wextra -Werror -DWEBP_DRIVER_LIBWEBP $tests/webp-driver.c -lwebp -o "$out/bin/webp-driver-libwebp" || {
	echo "make-fixtures: FAIL (reference driver build)"; exit 1; }

# The fixtures, in the list's order (craft lines use fixtures made before them).
failed=0
count=0
grep -v '^#' $tests/fixtures.list | while read -r name tool source args; do
	[ -n "$name" ] || continue
	file=$out/files/$name.webp
	case "$tool" in
	cwebp)
		timeout 300 cwebp -quiet $args "$out/src/$source.pam" -o "$file" ;;
	enc)
		timeout 300 "$out/bin/fixture-encoder" "$out/src/$source.pam" "$file" $args ;;
	img2webp)
		loop=$(echo "$args" | sed -n 's/.*-loop \([0-9]*\).*/\1/p')
		frame=$(echo "$args" | sed 's/-loop [0-9]*//')
		frames=""
		for one in $(echo "$source" | tr ',' ' '); do
			frames="$frames $frame $out/src/$one.pam"
		done
		timeout 300 img2webp -loop "${loop:-0}" $frames -o "$file" > /dev/null 2>&1 ;;
	craft)
		timeout 60 python3 -I $tests/craft.py $args "$out/files/$source.webp" "$file" ;;
	*)
		echo "unknown tool $tool for $name" ;;
	esac
	if [ ! -s "$file" ]; then
		echo "FAILED to make $name"
		echo "$name" >> "$out/failed.txt"
	fi
done
[ -f "$out/failed.txt" ] && failed=$(wc -l < "$out/failed.txt")
count=$(ls "$out/files" | wc -l)

# The expected outputs.
decoded=0
for file in "$out"/files/*.webp; do
	name=$(basename "$file" .webp)
	timeout 300 "$out/bin/webp-driver-libwebp" "$file" > "$out/expect/$name.ref" 2>&1
	if timeout 120 dwebp -quiet -nodither "$file" -pam -o "$out/expect/$name.pam" 2> /dev/null; then
		decoded=$((decoded + 1))
		timeout 120 dwebp -quiet -nodither -nofancy "$file" -pam -o "$out/expect/$name.nofancy.pam" 2> /dev/null
		if grep -q '^features 0 [0-9]* [0-9]* [01] 0 1$' "$out/expect/$name.ref"; then
			timeout 120 dwebp -quiet -nodither "$file" -yuv -o "$out/expect/$name.yuv" 2> /dev/null
			timeout 120 dwebp -quiet -nodither -nofilter "$file" -yuv -o "$out/expect/$name.nofilter.yuv" 2> /dev/null
		fi
	fi
done

# The file list's hashes, against the recorded ones.
(cd "$out/files" && sha256sum *.webp) > "$out/fixtures.sha256"
same=unrecorded
if [ -f $tests/fixtures.sha256 ]; then
	if cmp -s $tests/fixtures.sha256 "$out/fixtures.sha256"; then
		same=same
	else
		same="DIFFERENT ($(diff $tests/fixtures.sha256 "$out/fixtures.sha256" | grep -c '^>') lines)"
	fi
fi
sync
if [ "$failed" = 0 ] && [ "$count" -gt 0 ]; then
	echo "make-fixtures: PASS ($count files, $decoded decoded by dwebp, hashes $same; $out)"
	exit 0
fi
echo "make-fixtures: FAIL ($failed of the list not made; $out/failed.txt)"
exit 1
