#!/bin/sh
# The constant-time checks of run-ct.sh over several compilers and optimization levels: each test
# plan/ws335/tests/unit/ct-<name>.c is built with -DWS335_CT and no sanitizer, once per compiler (cc and clang by
# default) and level (-O2 and -O3 by default), and run under valgrind's memcheck; a secret used in a branch or an
# address is a memcheck error and a FAIL.  run-ct.sh stays the single-build check (cc, -O2) of each Phase.
#   sh plan/ws335/tests/run-ct-matrix.sh <name>... | all
#   CT_COMPILERS="cc clang" CT_LEVELS="-O2 -O3" sh plan/ws335/tests/run-ct-matrix.sh all
# Each run prints "ct-<name> <compiler> <level>: PASS (...)" or "...: FAIL (...)"; the last line is
# run-ct-matrix: PASS (<n> builds) or run-ct-matrix: FAIL (<failed> of <n> builds).  Output under
# build/tmp/ws335-ct-matrix (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws335-ct-matrix
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
compilers=${CT_COMPILERS:-cc clang}
levels=${CT_LEVELS:--O2 -O3}

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws335/tests/unit/ct-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#ct-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws335/tests/run-ct-matrix.sh <name>... | all"
	exit 2
fi
if ! command -v valgrind > /dev/null 2>&1; then
	echo "run-ct-matrix: FAIL (valgrind is not installed)"
	exit 1
fi

modules=""
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done

builds=0
failed=0
for compiler in $compilers; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "run-ct-matrix: FAIL (no $compiler)"
		exit 1
	fi
	for level in $levels; do
		for name in $names; do
			builds=$((builds + 1))
			label="ct-$name $compiler $level"
			program=$out/ct-$name-$compiler$level
			if ! timeout 600 "$compiler" -std=c89 "$level" -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DWS335_CT \
			    -I"$out/include" -I. -Iplan/ws335/tests/unit plan/ws335/tests/unit/ct-$name.c $modules -lpthread \
			    -o "$program" > "$program.build.log" 2>&1; then
				echo "$label: FAIL (build; $program.build.log)"
				head -10 "$program.build.log"
				failed=$((failed + 1))
				continue
			fi
			if timeout 3600 valgrind --quiet --error-exitcode=99 --track-origins=yes "$program" > "$program.log" 2>&1; then
				echo "$label: $(tail -1 "$program.log" | sed 's/^ct-[a-z0-9-]*: //')"
			else
				echo "$label: FAIL (memcheck; $program.log)"
				grep -m 12 -E 'Conditional|uninitialised|at 0x|by 0x' "$program.log"
				failed=$((failed + 1))
			fi
		done
	done
done

if [ "$failed" -eq 0 ]; then
	echo "run-ct-matrix: PASS ($builds builds)"
else
	echo "run-ct-matrix: FAIL ($failed of $builds builds)"
fi
[ "$failed" -eq 0 ]
