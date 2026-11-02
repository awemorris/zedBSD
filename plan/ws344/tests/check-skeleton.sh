#!/bin/sh
# The shape checks of libwebp-compat (userland/desktop/libwebp-compat and include/libc/compat/webp):
#  1. every header compiles alone (included twice, so its guard works), with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every .c compiles with both, the same flags;
#  3. the library's sources include no libwebp header (<webp/...>) and nothing outside the C library, their own
#     directory and <compat/webp/...>;
#  4. exports.map lists exactly the calls the two compat headers declare;
#  5. no source or header names a development record (WS, Phase, Queue or Bug numbers, plan/);
#  6. with BASE=<commit>, the headers (the library's and the compat ones), common.c, the build files (Makefile,
#     sources.mk, exports.map) and the test tools are unchanged since that commit, and no header or tool was added
#     (an implementation Phase must not change them; a fault goes back to Q1).
#   sh plan/ws344/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws344/tests/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws344-skeleton
fresh_out "$out"
mkdir -p "$out/include/compat/webp"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"
lib=userland/desktop/libwebp-compat
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -I$out/include -I$lib"
status=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in $lib/*.h include/libc/compat/webp/*.h; do
		case "$header" in
		include/libc/*) name="compat/webp/$(basename "$header")"; printf '#include <%s>\n#include <%s>\n' "$name" "$name" > "$out/one.c" ;;
		*) printf '#include "%s"\n#include "%s"\n' "$(basename "$header")" "$(basename "$header")" > "$out/one.c" ;;
		esac
		if ! $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in $lib/*.c; do
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

# 3. The includes: the C library's headers, this directory's, and the compat headers only.
if grep -n '#include' $lib/*.c $lib/*.h include/libc/compat/webp/*.h | \
    grep -v -E '#include (<(stddef|stdint|stdlib|string|errno|limits)\.h>|<compat/webp/(types|decode)\.h>|"[a-z0-9-]+\.h")'; then
	echo "FAILED an include outside the allowed set (above)"
	status=1
fi

# 4. exports.map against the declarations.
grep -h -o -E '\bWebP[A-Za-z]+\(' include/libc/compat/webp/types.h include/libc/compat/webp/decode.h | \
	tr -d '(' | sort -u > "$out/declared.txt"
grep -h -o -E '^#define WebP[A-Za-z]+' include/libc/compat/webp/types.h include/libc/compat/webp/decode.h | \
	sed 's/#define //' | sort -u > "$out/macros.txt"
comm -23 "$out/declared.txt" "$out/macros.txt" > "$out/functions.txt"
grep -o -E '^\s+WebP[A-Za-z]+;' $lib/exports.map | tr -d ' \t;' | sort -u > "$out/exported.txt"
if ! cmp -s "$out/functions.txt" "$out/exported.txt"; then
	echo "FAILED exports.map differs from the declarations (< declared only, > exported only):"
	diff "$out/functions.txt" "$out/exported.txt" | grep '^[<>]'
	status=1
fi

# 5. No development records in the code.
if grep -n -i -E '\b(ws[0-9]{3}|phase[ ]?[0-9]+|q[0-9]{4}|bug-[0-9]+)\b|plan/' $lib/*.c $lib/*.h include/libc/compat/webp/*.h; then
	echo "FAILED a development record named in the code (above)"
	status=1
fi

# 6. Frozen since BASE.
if [ -n "${BASE:-}" ]; then
	frozen="$lib/*.h $lib/common.c $lib/Makefile $lib/sources.mk $lib/exports.map include/libc/compat/webp
		plan/ws344/tests/*.sh plan/ws344/tests/*.py plan/ws344/tests/*.c plan/ws344/tests/unit/*.h
		plan/ws344/tests/unit/test-helpers.c plan/ws344/tests/unit/test-common.c plan/ws344/tests/fixtures.list
		plan/ws344/tests/fixtures.sha256"
	if ! git diff --quiet "$BASE" -- $frozen; then
		echo "FAILED frozen files changed since $BASE:"
		git diff --stat "$BASE" -- $frozen
		status=1
	fi
	added=$(git status --porcelain --untracked-files=all -- "$lib" include/libc/compat/webp plan/ws344/tests | \
		awk '{print $2}' | grep -E '\.h$|^plan/ws344/tests/[^/]+$' )
	if [ -n "$added" ]; then
		echo "FAILED headers or tools added since $BASE (not committed):"
		echo "$added"
		status=1
	fi
	added=$(git diff --name-only --diff-filter=A "$BASE" -- "$lib" include/libc/compat/webp plan/ws344/tests | \
		grep -E '\.h$|^plan/ws344/tests/[^/]+$')
	if [ -n "$added" ]; then
		echo "FAILED headers or tools added since $BASE:"
		echo "$added"
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
