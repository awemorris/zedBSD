#!/bin/sh
# The constant-time checks of the calls libpasskey adds to libcrypto-compat, built with every compiler and
# optimization level a build may use: plan/ws335/tests/unit/ct-api-passkey.c and ct-aes.c (the AES circuit the
# inverse cipher shares) with cc and clang at -O2 and -O3, -DWS335_CT, run under valgrind's memcheck the way
# plan/ws335/tests/run-ct.sh runs them (any branch or address on a secret is a FAIL).
#   sh plan/ws343/tests/ct-matrix.sh
# Last line: ct-matrix: PASS or ct-matrix: FAIL.  Output under build/tmp/ws343-ct (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws343-ct
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"

modules=""
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done

status=0
for compiler in cc clang; do
	for level in -O2 -O3; do
		for name in api-passkey aes; do
			label=ct-$name-$compiler$level
			if ! $compiler -std=c89 $level -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DWS335_CT -I"$out/include" -I. \
			    -Iplan/ws335/tests/unit plan/ws335/tests/unit/ct-$name.c $modules -lpthread -o "$out/$label" \
			    > "$out/$label.build.log" 2>&1; then
				echo "$label: FAIL (build)"
				head -20 "$out/$label.build.log"
				status=1
				continue
			fi
			if timeout 1800 valgrind --quiet --error-exitcode=99 --track-origins=yes "$out/$label" \
			    > "$out/$label.log" 2>&1; then
				echo "$label: $(tail -1 "$out/$label.log")"
			else
				echo "$label: FAIL (memcheck; log $out/$label.log)"
				grep -m 20 -E 'Conditional|uninitialised|at 0x|by 0x' "$out/$label.log"
				status=1
			fi
		done
	done
done

if [ "$status" = 0 ]; then echo "ct-matrix: PASS"; else echo "ct-matrix: FAIL"; fi
exit "$status"
