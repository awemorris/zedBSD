#!/bin/sh
# Checks libwebp-compat's inner decoders on the fixtures (make-fixtures.sh first) against dwebp's outputs, with
# fixture-check.c built with every library source, ASan and UBSan:
#   lossless   every lossless fixture: webpc_vp8l_decode's ARGB = dwebp -pam
#   lossy      every lossy fixture: webpc_vp8_decode's planes = dwebp -yuv, and with bypass = dwebp -nofilter -yuv
#   alpha      every lossy fixture with alpha: webpc_alpha_decode = the alpha of dwebp -pam
#   sh plan/tools/webp/run-fixtures.sh lossless|lossy|alpha
# Last line: run-fixtures: PASS (<mode>, <n> checks) or run-fixtures: FAIL (<m> of <n>; log ...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
mode=${1:-}
fixtures=${WEBPC_FIXTURES:-build/tmp/webp-fixtures}
case "$mode" in
lossless|lossy|alpha) ;;
*) echo "usage: sh plan/tools/webp/run-fixtures.sh lossless|lossy|alpha"; exit 2 ;;
esac
if [ ! -d "$fixtures/expect" ]; then
	echo "run-fixtures: FAIL (no fixtures; run sh plan/tools/webp/make-fixtures.sh)"
	exit 1
fi
out=build/tmp/webp-fixture-check
fresh_out "$out"
mkdir -p "$out/include/compat/webp"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"
if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$out/include" -Iuserland/desktop/libwebp-compat plan/tools/webp/fixture-check.c \
    userland/desktop/libwebp-compat/*.c -o "$out/fixture-check" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "run-fixtures: FAIL (build)"
	exit 1
fi

total=0
failed=0
for ref in "$fixtures"/expect/*.ref; do
	name=$(basename "$ref" .ref)
	file=$fixtures/files/$name.webp
	features=$(grep '^features 0 ' "$ref")
	[ -n "$features" ] || continue
	format=$(echo "$features" | awk '{print $7}')
	alpha=$(echo "$features" | awk '{print $5}')
	case "$mode" in
	lossless)
		[ "$format" = 2 ] && [ -f "$fixtures/expect/$name.pam" ] || continue
		set -- "lossless $file $fixtures/expect/$name.pam" ;;
	lossy)
		[ "$format" = 1 ] && [ -f "$fixtures/expect/$name.yuv" ] || continue
		set -- "lossy $file $fixtures/expect/$name.yuv" "lossy $file $fixtures/expect/$name.nofilter.yuv nofilter" ;;
	alpha)
		[ "$format" = 1 ] && [ "$alpha" = 1 ] && [ -f "$fixtures/expect/$name.pam" ] || continue
		set -- "alpha $file $fixtures/expect/$name.pam" ;;
	esac
	for check in "$@"; do
		total=$((total + 1))
		if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		    timeout 120 "$out/fixture-check" $check >> "$out/log" 2>&1; then
			failed=$((failed + 1))
			echo "FAILED $check" >> "$out/log"
		fi
	done
done

grep -E '^(mismatch|FAILED)' "$out/log" | head -20
if [ "$total" = 0 ]; then echo "run-fixtures: FAIL ($mode, no fixture matched)"; exit 1; fi
if [ "$failed" = 0 ]; then echo "run-fixtures: PASS ($mode, $total checks)"; exit 0; fi
echo "run-fixtures: FAIL ($failed of $total; log $out/log)"
exit 1
