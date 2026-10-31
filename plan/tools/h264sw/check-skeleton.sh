#!/bin/sh
# The shape checks of the software H.264 decoder (libmedia's h264sw-* and the codec-independent vcodec-*):
#  1. every h264sw and vcodec header compiles alone (included twice, so its guard works), with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every h264sw-*.c and vcodec-*.c compiles with both;
#  3. no vcodec file names an h264sw header (the codec-independent part does not depend on a codec);
#  4. with BASE=<commit>, the headers are unchanged since that commit (an implementation Phase
#     must not change them; a header fault goes back to Q1).
#   sh plan/tools/h264sw/check-skeleton.sh
#   BASE=<sha> sh plan/tools/h264sw/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/h264sw-skeleton
fresh_out "$out"
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I."
status=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in userland/desktop/libmedia/h264sw*.h userland/desktop/libmedia/vcodec*.h; do
		printf '#include "%s"\n#include "%s"\n' "$header" "$header" > "$out/one.c"
		if ! $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in userland/desktop/libmedia/h264sw-*.c userland/desktop/libmedia/vcodec-*.c; do
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

if grep -n 'h264sw' userland/desktop/libmedia/vcodec*.h userland/desktop/libmedia/vcodec-*.c; then
	echo "FAILED a vcodec file names the H.264 decoder (above)"
	status=1
fi

if [ -n "${BASE:-}" ]; then
	if ! git diff --quiet "$BASE" -- 'userland/desktop/libmedia/h264sw*.h' 'userland/desktop/libmedia/vcodec*.h'; then
		echo "FAILED headers changed since $BASE:"
		git diff --stat "$BASE" -- 'userland/desktop/libmedia/h264sw*.h' 'userland/desktop/libmedia/vcodec*.h'
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
