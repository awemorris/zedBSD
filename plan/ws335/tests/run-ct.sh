#!/bin/sh
# The constant-time checks: each test plan/ws335/tests/unit/ct-<name>.c (which uses ct.h's CT_SECRET and
# CT_PUBLIC around the secret-handling calls of one module) is built with -DWS335_CT, without sanitizers, at -O2
# (the optimizer's branches are what runs), and run under valgrind's memcheck; any use of a secret in a branch or an
# address is a memcheck error and a FAIL.
#   sh plan/ws335/tests/run-ct.sh <name>... | all
# Last line: run-ct: PASS or run-ct: FAIL.  Output under build/tmp/ws335-ct (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws335-ct
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"

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
	echo "usage: sh plan/ws335/tests/run-ct.sh <name>... | all"
	exit 2
fi
if ! command -v valgrind > /dev/null 2>&1; then
	echo "run-ct: FAIL (valgrind is not installed)"
	exit 1
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
	source=plan/ws335/tests/unit/ct-$name.c
	if ! cc -std=c89 -O2 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DWS335_CT -I"$out/include" -I. \
	    -Iplan/ws335/tests/unit "$source" $modules -lpthread -o "$out/ct-$name" > "$out/ct-$name.build.log" 2>&1; then
		echo "ct-$name: FAIL (build)"
		head -20 "$out/ct-$name.build.log"
		status=1
		continue
	fi
	if timeout 1800 valgrind --quiet --error-exitcode=99 --track-origins=yes "$out/ct-$name" \
	    > "$out/ct-$name.log" 2>&1; then
		tail -1 "$out/ct-$name.log"
	else
		echo "ct-$name: FAIL (memcheck; log $out/ct-$name.log)"
		grep -m 20 -E 'Conditional|uninitialised|at 0x|by 0x' "$out/ct-$name.log"
		status=1
	fi
done

if [ "$status" = 0 ]; then echo "run-ct: PASS"; else echo "run-ct: FAIL"; fi
exit "$status"
