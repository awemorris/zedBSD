#!/bin/sh
# The host unit tests of libcrypto-compat and libssl-compat: each test plan/ws335/tests/unit/test-<name>.c is built
# with every .c of both libraries except the Windows ones (modules not implemented yet are skeletons that answer
# ENOSYS), with ASan and UBSan, and run.  Each test's last line is "test-<name>: PASS (<n> checks)" or
# "test-<name>: FAIL (...)".  The test vectors are read from WS335_VECTORS (default build/ws335-vectors, made by
# fetch.sh); a test that needs a missing file says FAIL, not PASS.
#   sh plan/ws335/tests/run-unit.sh <name>...     one or more tests (ct-<name> runs unit/ct-<name>.c under ASan too)
#   sh plan/ws335/tests/run-unit.sh all           every test present
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws335-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws335-unit
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
WS335_VECTORS=${WS335_VECTORS:-$(pwd)/build/ws335-vectors}
export WS335_VECTORS

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws335/tests/unit/test-*.c plan/ws335/tests/unit/ct-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		case "$name" in
		test-*) names="$names ${name#test-}" ;;
		*) names="$names $name" ;;
		esac
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws335/tests/run-unit.sh <name>... | all"
	exit 2
fi

modules=""
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done

status=0
for name in $names; do
	case "$name" in
	ct-*) source=plan/ws335/tests/unit/$name.c; name=${name#ct-}; label=ct-$name ;;
	*) source=plan/ws335/tests/unit/test-$name.c; label=test-$name ;;
	esac
	if [ ! -f "$source" ]; then
		echo "$label: FAIL (no $source)"
		status=1
		continue
	fi
	if ! cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. -Iplan/ws335/tests/unit \
	    "$source" $modules -lpthread -o "$out/$label" > "$out/$label.build.log" 2>&1; then
		echo "$label: FAIL (build)"
		head -20 "$out/$label.build.log"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    timeout 600 "$out/$label" > "$out/$label.log" 2>&1; then
		tail -1 "$out/$label.log"
	else
		echo "$label: FAIL (exit; log $out/$label.log)"
		grep -v '^ok ' "$out/$label.log" | head -20
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"
