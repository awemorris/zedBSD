#!/bin/sh
# The host unit tests of libwebp-compat: each test plan/ws344/tests/unit/test-<name>.c is built with every
# userland/desktop/libwebp-compat/*.c (modules not implemented yet are skeletons that answer ENOSYS or 0) and the
# compat headers (include/libc/compat/webp, copied under the run directory as compat/webp), with ASan and UBSan, and
# run (rfc-vp8-tables.h, RFC 6386's tables extracted by vp8-tables-extract.py, is on the include path).
# Each test's last line is "test-<name>: PASS (...)" or "test-<name>: FAIL (...)".
#   sh plan/ws344/tests/run-unit.sh <name>...     one or more tests
#   sh plan/ws344/tests/run-unit.sh all           every test present
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws344-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws344-unit
fresh_out "$out"
mkdir -p "$out/include/compat/webp"
cp include/libc/compat/webp/*.h "$out/include/compat/webp/"

# RFC 6386's tables read from its text, for the table test (rfc-vp8-tables.h).
spec=${WEBPC_RFC6386:-/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt}
if [ -f "$spec" ]; then
	python3 -I plan/ws344/tests/vp8-tables-extract.py "$spec" "$out/include/rfc-vp8-tables.h" > /dev/null || {
		echo "run-unit: FAIL (vp8-tables-extract)"; exit 1; }
fi

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws344/tests/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws344/tests/run-unit.sh <name>... | all"
	exit 2
fi

modules=$(ls userland/desktop/libwebp-compat/*.c)
status=0
for name in $names; do
	source=plan/ws344/tests/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I"$out/include" -Iuserland/desktop/libwebp-compat \
	    -Iplan/ws344/tests/unit "$source" $modules -lm -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
		echo "test-$name: FAIL (build)"
		head -20 "$out/test-$name.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    WEBPC_FIXTURES="${WEBPC_FIXTURES:-build/tmp/ws344-fixtures}" \
	    timeout 600 "$out/test-$name" > "$out/test-$name.log" 2>&1; then
		tail -1 "$out/test-$name.log"
	else
		echo "test-$name: FAIL (exit; log $out/test-$name.log)"
		grep -v '^ok ' "$out/test-$name.log" | tail -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"
