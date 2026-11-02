#!/bin/sh
# x509-limbo through the public interface: limbo-api.c runs every SERVER case of x509-limbo as a TLS 1.2 handshake
# (the case's chain in the server's Certificate, the case's roots in the client's store, its name through
# SSL_set1_host, its time as the library's clock) and compares each verdict with the case's expectation; the
# differences must be the listed ones with their reasons (test-limbo.c's, which checks cryc_chain_verify directly,
# and the public interface's own).  It is built with the host's cc from the sources of both libraries with ASan and
# UBSan, as run-unit.sh builds the unit tests.
#   sh plan/ws335/tests/run-limbo.sh
# Reads WS335_VECTORS/rec/limbo.vec (default build/ws335-vectors, made by fetch.sh).  WS335_LIMBO_TRACE=1 prints
# every case's verdict; WS335_LIMBO_ONLY=<id> runs one case.
# The summary (counts by id prefix and by reason) is printed; the last line is run-limbo: PASS or run-limbo: FAIL.
# Output under build/tmp/ws335-limbo (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws335-limbo
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
WS335_VECTORS=${WS335_VECTORS:-$(pwd)/build/ws335-vectors}
case "$WS335_VECTORS" in
/*) ;;
*) WS335_VECTORS=$(pwd)/$WS335_VECTORS ;;
esac
export WS335_VECTORS

modules=
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done
if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. -Iplan/ws335/tests/unit \
    plan/ws335/tests/limbo-api.c $modules -lpthread -o "$out/limbo-api" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
	head -20 "$out/build.log"
	echo "run-limbo: FAIL (build; $out/build.log)"
	exit 1
fi
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 1800 "$out/limbo-api" \
    > "$out/limbo-api.log" 2>&1
code=$?
grep -v '^ok ' "$out/limbo-api.log" | tail -40
last=$(tail -1 "$out/limbo-api.log")
case "$code:$last" in
"0:limbo-api: PASS"*) echo "run-limbo: PASS" ;;
*)
	echo "run-limbo: FAIL (exit $code; $out/limbo-api.log)"
	exit 1 ;;
esac
