#!/bin/sh
# The shape checks of libbrowser's vector drawing and SVG modules (userland/desktop/libbrowser/vector, svg):
#  1. every header compiles alone (included twice, so its guard works), with cc and clang,
#     -std=c11 -Wall -Wextra -Werror -Wpedantic (libbrowser is built as C11);
#  2. every .c compiles with both;
#  3. the modules include nothing of the engine: only vector/ and svg/ headers and the C library's
#     (no "base/", "dom/", "css/", "page/", "paint/", "layout/" and no <vulkan/...>);
#  4. no source names development records (a WS, Phase, Queue or Bug number, Q1, a date, or plan/);
#  5. with BASE=<commit>, every header vector/vec-*.h and svg/svg-*.h is unchanged since that commit (an
#     implementation Phase must not change them; a header fault goes back to Q1);
#  6. every .c compiles for zedBSD (amd64) against zedBSD's own C library headers with the project's clang
#     (WS328_ZEDBSD_CC, default build/llvm/bin/clang of this tree, --target=x86_64-unknown-zedbsd, the flags of
#     userland/base/package.mk for x86_64) and a sysroot (WS328_ZEDBSD_SYSROOT, default build/x86_64/sysroot of this
#     tree, as the phase.md says how to get them); when either is missing check 6 is not done and the last line is
#     "check-skeleton: INCOMPLETE" (exit 1), which does not pass.
#   sh plan/ws328/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws328/tests/check-skeleton.sh
# Last line: check-skeleton: PASS, FAIL or INCOMPLETE.  Output under build/tmp/ws328-skeleton.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws328-skeleton
fresh_out "$out"
src=userland/desktop/libbrowser
flags="-std=c11 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I$src"
status=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in $src/vector/*.h $src/svg/*.h; do
		printf '#include "%s"\n#include "%s"\n' "${header#$src/}" "${header#$src/}" > "$out/one.c"
		if ! timeout 120 $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in $src/vector/*.c $src/svg/*.c; do
		if ! timeout 120 $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

if grep -n -E '#[[:space:]]*include[[:space:]]*["<](base|dom|css|page|paint|layout|html|bind|js|vm|net|text|image|view|vulkan)/' \
    $src/vector/* $src/svg/*; then
	echo "FAILED a module includes the engine (above)"
	status=1
fi

if grep -n -E '\b(WS|ws)[0-9]{3}\b|\bq[0-9]{3,4}\b|\bp[0-9]{3}\b|\bQ1\b|BUG-[0-9]+|plan/|\b20[0-9]{2}-[01][0-9]-[0-3][0-9]\b' \
    $src/vector/* $src/svg/*; then
	echo "FAILED development records named in the sources (above)"
	status=1
fi

if [ -n "${BASE:-}" ]; then
	frozen=$(git ls-tree --name-only "$BASE" -- $src/vector/ $src/svg/ | grep -E '\.h$')
	if [ -z "$frozen" ]; then
		echo "FAILED no headers at $BASE"
		status=1
	elif ! git diff --quiet "$BASE" -- $frozen; then
		echo "FAILED headers changed since $BASE:"
		git diff --stat "$BASE" -- $frozen
		status=1
	fi
	added=$(ls $src/vector/*.h $src/svg/*.h | while read -r header; do
		git cat-file -e "$BASE:$header" 2> /dev/null || echo "$header"; done)
	if [ -n "$added" ]; then
		echo "FAILED headers added since $BASE: $added"
		status=1
	fi
fi

zedbsd_cc=${WS328_ZEDBSD_CC:-build/llvm/bin/clang}
zedbsd_sysroot=${WS328_ZEDBSD_SYSROOT:-build/x86_64/sysroot}
if [ -x "$zedbsd_cc" ] && [ -d "$zedbsd_sysroot/usr/include" ]; then
	for source in $src/vector/*.c $src/svg/*.c; do
		if ! timeout 120 "$zedbsd_cc" --target=x86_64-unknown-zedbsd -nostdinc -I$src -Iinclude \
		    -isystem "$zedbsd_sysroot/usr/include" -m64 -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 \
		    -std=c11 -Wall -Wextra -Werror -fsyntax-only "$source" > "$out/zedbsd.log" 2>&1; then
			echo "FAILED zedBSD headers $source"
			grep error "$out/zedbsd.log" | head -10
			status=1
		fi
	done
else
	echo "zedBSD toolchain ($zedbsd_cc) or sysroot ($zedbsd_sysroot): not present, check 6 not done"
	if [ "$status" = 0 ]; then
		echo "check-skeleton: INCOMPLETE"
		exit 1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
