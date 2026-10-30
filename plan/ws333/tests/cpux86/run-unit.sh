#!/bin/sh
# The host unit tests of the x86 processor decoders (src/drivers/platform/pcat/cpu-x86/cpux86*.c):
# each test plan/ws333/tests/cpux86/unit/test-<name>.c is built with every cpux86*.c (modules not
# implemented yet are skeletons that answer ENOSYS), with ASan and UBSan, and run.
# Each test's last line is "test-<name>: PASS (...)" or "test-<name>: FAIL (...)".
#   sh plan/ws333/tests/cpux86/run-unit.sh <name>...     one or more tests
#   sh plan/ws333/tests/cpux86/run-unit.sh all           every test present
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws333-cpux86-unit (a fresh run
# directory).  CC picks the compiler (default cc).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws333-cpux86-unit
fresh_out "$out"
compiler=${CC:-cc}
src=src/drivers/platform/pcat/cpu-x86

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws333/tests/cpux86/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws333/tests/cpux86/run-unit.sh <name>... | all"
	exit 2
fi

status=0
for name in $names; do
	source=plan/ws333/tests/cpux86/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	if ! $compiler -std=c89 -O1 -g -Wall -Wextra -Werror -Wpedantic -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer -Iinclude -I"$src" -Iplan/ws333/tests/cpux86/unit \
	    "$source" "$src"/cpux86*.c -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
		echo "test-$name: FAIL (build)"
		head -20 "$out/test-$name.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 300 "$out/test-$name" > "$out/test-$name.log" 2>&1; then
		tail -1 "$out/test-$name.log"
	else
		echo "test-$name: FAIL (exit; log $out/test-$name.log)"
		grep -v '^ok ' "$out/test-$name.log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"
