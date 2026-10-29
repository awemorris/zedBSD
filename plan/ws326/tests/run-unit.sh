#!/bin/sh
# The host unit tests of the software H.264 decoder (libmedia's h264sw-*): each test
# plan/ws326/tests/unit/test-<name>.c is built with every h264sw-*.c except the back end (modules
# not implemented yet are skeletons that answer ENOSYS) and bits.c, with ASan and UBSan, and run.
# Each test's last line is "test-<name>: PASS (...)" or "test-<name>: FAIL (...)".
#   sh plan/ws326/tests/run-unit.sh <name>...     one or more tests
#   sh plan/ws326/tests/run-unit.sh all           every test present
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws326-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws326-unit
fresh_out "$out"

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws326/tests/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws326/tests/run-unit.sh <name>... | all"
	exit 2
fi

modules=""
for file in userland/desktop/libmedia/h264sw-*.c; do
	case "$file" in
	*/h264sw-backend.c) ;;
	*) modules="$modules $file" ;;
	esac
done

status=0
for name in $names; do
	source=plan/ws326/tests/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -I. -Iplan/ws326/tests/unit "$source" $modules userland/desktop/libmedia/bits.c \
	    -lm -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
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
