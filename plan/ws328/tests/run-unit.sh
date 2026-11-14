#!/bin/sh
# The host unit tests of libbrowser's vector drawing and SVG modules (userland/desktop/libbrowser/vector/vec-*.c and
# svg/svg-*.c): each test plan/ws328/tests/unit/test-<name>.c is built with every module (modules not implemented yet
# are skeletons that answer ENOSYS or nothing), with ASan and UBSan, -std=c11 -Wall -Wextra -Werror, and run.
# Each test's last line is "test-<name>: PASS (...)" or "test-<name>: FAIL (...)".
#   sh plan/ws328/tests/run-unit.sh <name>...     one or more tests
#   sh plan/ws328/tests/run-unit.sh all           every test present
#   CC=clang sh plan/ws328/tests/run-unit.sh ...  with another compiler (default cc)
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws328-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws328-unit
fresh_out "$out"
cc=${CC:-cc}
src=userland/desktop/libbrowser

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws328/tests/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws328/tests/run-unit.sh <name>... | all"
	echo "run-unit: FAIL"
	exit 2
fi

modules=""
for file in $src/vector/vec-*.c $src/svg/svg-*.c; do
	[ -f "$file" ] && modules="$modules $file"
done

status=0
for name in $names; do
	source=plan/ws328/tests/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	if ! timeout 300 "$cc" -std=c11 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I$src -Iplan/ws328/tests/unit \
	    "$source" $modules -lm -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
		echo "test-$name: FAIL (build)"
		head -20 "$out/test-$name.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 600 "$out/test-$name" > "$out/test-$name.log" 2>&1; then
		tail -1 "$out/test-$name.log"
	else
		echo "test-$name: FAIL (exit; log $out/test-$name.log)"
		grep -v '^ok ' "$out/test-$name.log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"
