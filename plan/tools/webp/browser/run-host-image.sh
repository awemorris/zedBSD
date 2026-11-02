#!/bin/sh
# libbrowser's image module (image/*.c) on WebP files, on the host with ASan and UBSan: img_sniff says IMG_WEBP and
# img_decode gives dwebp 1.5.0's pixels, for every fixture dwebp decodes (FIXTURES/expect/NAME.pam) and every file
# under EXTRA directories (their expected PAM made here with dwebp); files dwebp refuses (animations, broken
# containers) must give EINVAL.  Only files with a RIFF header and a VP8/VP8L/VP8X first chunk count (the MIME
# Sniffing pattern the browser goes by; bare bitstreams are not images to it).
#   sh plan/tools/webp/browser/run-host-image.sh [FIXTURES] [EXTRA...]   (default build/tmp/webp-fixtures)
# Last line: host-image: PASS (<n> files) or host-image: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
fixtures=${1:-build/tmp/webp-fixtures}
[ $# -gt 0 ] && shift
. plan/tools/fresh-out.sh
fresh_out build/tmp/webp-host-image
out=build/tmp/webp-host-image
mkdir -p "$out/include" "$out/extra"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
lib=userland/desktop/libbrowser
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-I$lib -I"$out/include" -Iuserland/desktop/libwebp-compat plan/tools/webp/browser/host-image.c \
	$lib/image/decode.c $lib/image/webp.c $lib/image/png.c $lib/image/jpeg.c $lib/image/gif.c \
	userland/desktop/libwebp-compat/*.c userland/desktop/libpng-compat/*.c userland/desktop/libjpeg-compat/*.c \
	userland/desktop/libgif-compat/*.c userland/base/libz-compat/*.c -lm -o "$out/host-image" > "$out/build.log" 2>&1 || {
	head -20 "$out/build.log"; echo "host-image: FAIL (build)"; exit 1; }

# The pairs: the fixtures with their PAM, those dwebp refuses with "-"; the extra files with a PAM made here.
set --
riff() { [ "$(head -c 4 "$1")" = RIFF ] && [ "$(head -c 14 "$1" | tail -c 6)" = WEBPVP ]; }
for file in "$fixtures"/files/*.webp; do
	name=$(basename "$file" .webp)
	riff "$file" || continue
	if [ -f "$fixtures/expect/$name.pam" ]; then
		set -- "$@" "$file" "$fixtures/expect/$name.pam"
	else
		set -- "$@" "$file" -
	fi
done
for dir in ${EXTRA:-}; do
	for file in "$dir"/*; do
		riff "$file" || continue
		name=$(basename "$file")
		if dwebp -quiet -nodither "$file" -pam -o "$out/extra/$name.pam" 2>/dev/null; then
			set -- "$@" "$file" "$out/extra/$name.pam"
		else
			set -- "$@" "$file" -
		fi
	done
done
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 1800 "$out/host-image" "$@" > "$out/log" 2>&1
grep '^mismatch' "$out/log" | head -10
tail -1 "$out/log"
