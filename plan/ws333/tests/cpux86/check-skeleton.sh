#!/bin/sh
# The shape checks of the x86 processor decoders (src/drivers/platform/pcat/cpu-x86/cpux86*):
#  1. every cpux86 header compiles alone (included twice, so its guard works), with cc and clang
#     (both must be installed),
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every cpux86*.c compiles with both;
#  3. with BASE=<commit>, the headers are unchanged since that commit (an implementation Phase
#     must not change them; a header fault goes back to Q1).
#   sh plan/ws333/tests/cpux86/check-skeleton.sh
#   BASE=<sha> sh plan/ws333/tests/cpux86/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.  Output under
# build/tmp/ws333-cpux86-skeleton (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws333-cpux86-skeleton
fresh_out "$out"
src=src/drivers/platform/pcat/cpu-x86
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -Iinclude -I$src"
status=0
compilers=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "FAILED $compiler is not installed (both compilers are required)"
		status=1
		continue
	fi
	compilers=$((compilers + 1))
	for header in "$src"/cpux86*.h; do
		printf '#include "%s"\n#include "%s"\nint check_skeleton_unit;\n' "$(basename "$header")" "$(basename "$header")" > "$out/one.c"
		if ! $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in "$src"/cpux86*.c; do
		if ! $compiler $flags -c "$source" -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done
if [ "$compilers" = 0 ]; then
	echo "FAILED no compiler"
	status=1
fi

if [ -n "${BASE:-}" ]; then
	if ! git rev-parse --verify --quiet "$BASE^{commit}" > /dev/null; then
		echo "FAILED BASE $BASE is not a commit"
		status=1
	elif ! git diff --quiet "$BASE" -- "$src/cpux86*.h"; then
		echo "FAILED headers changed since $BASE:"
		git diff --stat "$BASE" -- "$src/cpux86*.h"
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
