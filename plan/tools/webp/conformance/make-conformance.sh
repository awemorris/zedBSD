#!/bin/sh
# Makes libwebp-compat's second set of test files, from outside libwebp's encoder, with the same layout as
# make-fixtures.sh (files/, expect/NAME.ref from the driver built against libwebp, expect/NAME.pam, .nofancy.pam and
# for lossy files .yuv and .nofilter.yuv from dwebp 1.5.0), so that run-fixtures.sh and compare.sh run on it with
# WEBPC_FIXTURES=build/tmp/webp-conformance:
#   - libwebp-test-data (https://chromium.googlesource.com/webm/libwebp-test-data, its *.webp);
#   - every shown key frame of libvpx's VP8 test vectors vp80-00-comprehensive-001..017
#     (https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx/), each as a still WebP;
#   - frames with 2, 4 and 8 token partitions from fixture-encoder.c at methods 0 to 2 (libwebp's encoder writes one
#     partition at method 3 and above, which is why fixtures.list's partition lines have one).
# The downloads live in /home/awe/zedBSD-claude1/build/sources/webp-conformance (not in git); their SHA-256 are
# compared with plan/tools/webp/conformance/sha256.txt (written on the first run when absent).  make-fixtures.sh
# must have run (its sources and tools are reused).
#   sh plan/tools/webp/conformance/make-conformance.sh [fetch]
# fetch: download what is missing first (parameterless GETs and a shallow git clone).
# Last line: make-conformance: PASS (<n> files ...) or make-conformance: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
here=plan/tools/webp/conformance
store=/home/awe/zedBSD-claude1/build/sources/webp-conformance
fixtures=build/tmp/webp-fixtures
out=build/tmp/webp-conformance

# The downloads.
if [ "${1:-}" = fetch ]; then
	mkdir -p "$store/vp80"
	if [ ! -d "$store/libwebp-test-data" ]; then
		timeout 600 git clone -q --depth 1 https://chromium.googlesource.com/webm/libwebp-test-data \
			"$store/libwebp-test-data" || { echo "make-conformance: FAIL (clone)"; exit 1; }
	fi
	for n in 001 002 003 004 005 006 007 008 009 010 011 012 013 014 015 016 017; do
		file=$store/vp80/vp80-00-comprehensive-$n.ivf
		[ -s "$file" ] && continue
		timeout 300 curl -fsSL -o "$file" \
			"https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx/vp80-00-comprehensive-$n.ivf" || {
			echo "make-conformance: FAIL (download $n)"; exit 1; }
	done
fi
if [ ! -d "$store/libwebp-test-data" ] || [ ! -d "$store/vp80" ]; then
	echo "make-conformance: FAIL (no downloads in $store; run with fetch)"
	exit 1
fi
if [ ! -x "$fixtures/bin/webp-driver-libwebp" ] || [ ! -x "$fixtures/bin/fixture-encoder" ]; then
	echo "make-conformance: FAIL (run sh plan/tools/webp/make-fixtures.sh first)"
	exit 1
fi

# The downloads' hashes against the recorded ones.
fresh_out "$out"
mkdir -p "$out/files" "$out/expect"
(cd "$store" && sha256sum libwebp-test-data/*.webp vp80/*.ivf) > "$out/sha256.txt"
if [ -f $here/sha256.txt ]; then
	if ! cmp -s $here/sha256.txt "$out/sha256.txt"; then
		diff $here/sha256.txt "$out/sha256.txt" | head -10
		echo "make-conformance: FAIL (the downloads differ from $here/sha256.txt)"
		exit 1
	fi
	hashes=same
else
	cp "$out/sha256.txt" $here/sha256.txt
	hashes=recorded
fi

# The files: libwebp-test-data as it is, the key frames of the vectors, the partition frames.
for file in "$store"/libwebp-test-data/*.webp; do
	cp "$file" "$out/files/ltd-$(basename "$file")"
done
for file in "$store"/vp80/*.ivf; do
	timeout 120 python3 -I $here/ivf-key-frames.py "$file" "$out/files" "$(basename "$file" .ivf)" >> "$out/ivf.log" ||
		{ echo "make-conformance: FAIL (ivf-key-frames $file)"; exit 1; }
done
for parts in 1 2 3; do
	for method in 0 1 2; do
		for source in photo-31x33 photo-256x256 photo-333x211 noise-100x75 text-1024x683 alpha-cutout-64x48; do
			timeout 300 "$fixtures/bin/fixture-encoder" "$fixtures/src/$source.pam" \
				"$out/files/part$parts-m$method-$source.webp" partitions=$parts method=$method q=60 ||
				{ echo "make-conformance: FAIL (fixture-encoder)"; exit 1; }
		done
	done
done

# The expected outputs, as make-fixtures.sh makes them.
decoded=0
for file in "$out"/files/*.webp; do
	name=$(basename "$file" .webp)
	timeout 300 "$fixtures/bin/webp-driver-libwebp" "$file" > "$out/expect/$name.ref" 2>&1
	if timeout 120 dwebp -quiet -nodither "$file" -pam -o "$out/expect/$name.pam" 2> /dev/null; then
		decoded=$((decoded + 1))
		timeout 120 dwebp -quiet -nodither -nofancy "$file" -pam -o "$out/expect/$name.nofancy.pam" 2> /dev/null
		if grep -q '^features 0 [0-9]* [0-9]* [01] 0 1$' "$out/expect/$name.ref"; then
			timeout 120 dwebp -quiet -nodither "$file" -yuv -o "$out/expect/$name.yuv" 2> /dev/null
			timeout 120 dwebp -quiet -nodither -nofilter "$file" -yuv -o "$out/expect/$name.nofilter.yuv" 2> /dev/null
		fi
	fi
done
count=$(ls "$out/files" | wc -l)
sync
echo "make-conformance: PASS ($count files, $decoded decoded by dwebp, downloads $hashes; $out)"
