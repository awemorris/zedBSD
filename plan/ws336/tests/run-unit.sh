#!/bin/sh
# The host unit tests of zedBSD's ssh, sshd and ssh-keygen modules (userland/base/ssh/ssh-*.c): each test
# plan/ws336/tests/unit/test-<name>.c is built with every module (modules not implemented yet are skeletons that
# answer ENOSYS), with ASan and UBSan, and run.  The crypto library is the host's OpenSSL 3.5 through the shims in
# plan/ws336/tests/host-crypto (libcrypto-compat has OpenSSL 3.5's interface); SSH_CRYPTO_INCLUDE=<dir whose
# compat/crypto/ holds libcrypto-compat's headers> and SSH_CRYPTO_LIB=<libcrypto-compat to link> use it instead.
# A test of a program's internal part (sshd's monitor, ssh's client) names the program files it needs in a comment
# line of its own " * run-unit-sources: userland/base/ssh/<file>.c ..." (files under userland/base/ssh/, and the
# console login's password check userland/base/login/verify.c that sshd shares).
# Each test's last line is "test-<name>: PASS (...)" or "test-<name>: FAIL (...)".
#   sh plan/ws336/tests/run-unit.sh <name>...     one or more tests
#   sh plan/ws336/tests/run-unit.sh all           every test present
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ssh-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ssh-unit
fresh_out "$out"

crypto_include=${SSH_CRYPTO_INCLUDE:-plan/ws336/tests/host-crypto}
crypto_lib=${SSH_CRYPTO_LIB:--lcrypto}

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws336/tests/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws336/tests/run-unit.sh <name>... | all"
	echo "run-unit: FAIL"
	exit 2
fi

# The library modules; the programs' files (sshd-*.c, ssh-client-*.c, *-main.c) are linked only when a test names them.
modules=""
for file in userland/base/ssh/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) modules="$modules $file" ;;
	esac
done

status=0
for name in $names; do
	source=plan/ws336/tests/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	extra=$(sed -n 's/^ \* run-unit-sources: *//p' "$source" | tr '\n' ' ')
	for file in $extra; do
		case "$file" in
		userland/base/ssh/*.c|userland/base/login/verify.c) ;;
		*) echo "test-$name: FAIL (run-unit-sources names $file, which it may not)"; status=1; continue 2 ;;
		esac
	done
	if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I. -Iuserland/base/ssh -Iplan/ws336/tests/unit \
	    -I"$crypto_include" "$source" $modules $extra $crypto_lib -o "$out/test-$name" > "$out/test-$name.build.log" 2>&1; then
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
