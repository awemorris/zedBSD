#!/bin/sh
# The host comparison of libpasskey's cryptography on OpenSSL and on libcrypto-compat: the same, unchanged
# userland/base/libpasskey/crypto-openssl.c is built twice with the driver passkey-compare.c,
#   openssl  against the host's OpenSSL (its headers, -lcrypto),
#   compat   against libcrypto-compat (userland/base/libcrypto-compat/openssl-names first on the include path, every
#            libcrypto-compat source compiled in), with ASan and UBSan,
# and the two outputs of every pk_crypto_* call on the same inputs must be identical (diff).  Then the compat build's
# ECDH with a peer of known private key is checked by OpenSSL's own derivation (check-ecdh), and the compat
# objects are checked to name no OpenSSL symbol other than through libcrypto-compat.  The vectors are read from
# WS335_VECTORS (default build/ws335-vectors).
#   sh plan/ws343/tests/run-passkey-compare.sh
# Last line: run-passkey-compare: PASS or run-passkey-compare: FAIL.  Output under build/tmp/ws343-compare (a fresh
# run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws343-compare
fresh_out "$out"
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
WS335_VECTORS=${WS335_VECTORS:-$(pwd)/build/ws335-vectors}
export WS335_VECTORS
status=0

modules=""
for file in userland/base/libcrypto-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I. -Iplan/ws335/tests/unit"

# The OpenSSL build.
if ! cc $flags -DPASSKEY_COMPARE_OPENSSL plan/ws343/tests/passkey-compare.c userland/base/libpasskey/crypto-openssl.c \
    -lcrypto -o "$out/openssl" > "$out/openssl.build.log" 2>&1; then
	echo "openssl build: FAIL"
	head -20 "$out/openssl.build.log"
	echo "run-passkey-compare: FAIL"
	exit 1
fi

# The compat build: the include path and the library change, crypto-openssl.c does not.
if ! cc $flags -fsanitize=address,undefined -fno-sanitize-recover=all -Iuserland/base/libcrypto-compat/openssl-names \
    -I"$out/include" plan/ws343/tests/passkey-compare.c userland/base/libpasskey/crypto-openssl.c $modules -lpthread \
    -o "$out/compat" > "$out/compat.build.log" 2>&1; then
	echo "compat build: FAIL"
	head -20 "$out/compat.build.log"
	echo "run-passkey-compare: FAIL"
	exit 1
fi

# The compat build names no OpenSSL header: its crypto-openssl.o's include list.
if cc $flags -Iuserland/base/libcrypto-compat/openssl-names -I"$out/include" -M userland/base/libpasskey/crypto-openssl.c \
    2> /dev/null | tr ' \\' '\n\n' | grep -q '^/usr/include/.*openssl/'; then
	echo "compat build: FAIL (an OpenSSL header of the host is included)"
	status=1
fi

# The two runs.
timeout 600 "$out/openssl" > "$out/openssl.out" 2> "$out/openssl.err"
code_openssl=$?
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 600 "$out/compat" \
    > "$out/compat.out" 2> "$out/compat.err"
code_compat=$?
lines=$(wc -l < "$out/openssl.out")
if [ "$code_openssl" != 0 ] || [ "$code_compat" != 0 ]; then
	echo "runs: FAIL (exit openssl $code_openssl, compat $code_compat; $out/compat.err)"
	head -20 "$out/compat.err"
	status=1
elif grep -q 'FAIL' "$out/openssl.out" "$out/compat.out"; then
	echo "runs: FAIL (vectors missing)"
	grep -h FAIL "$out/openssl.out" "$out/compat.out" | head -5
	status=1
elif ! diff "$out/openssl.out" "$out/compat.out" > "$out/diff.txt"; then
	echo "compare: FAIL ($(grep -c '^<' "$out/diff.txt") lines differ of $lines; $out/diff.txt)"
	head -20 "$out/diff.txt"
	status=1
else
	echo "compare: PASS ($lines calls, identical)"
	cut -d. -f1 "$out/compat.out" | cut -d' ' -f1 | sort | uniq -c | tr '\n' ' '
	echo
fi

# The compat ECDH, checked by OpenSSL.
ASAN_OPTIONS=detect_leaks=1 timeout 600 "$out/compat" ecdh 50 > "$out/ecdh.out" 2> "$out/ecdh.err"
if ! timeout 600 "$out/openssl" check-ecdh < "$out/ecdh.out" > "$out/check-ecdh.out" 2>&1; then
	status=1
fi
tail -1 "$out/check-ecdh.out"

if [ "$status" = 0 ]; then echo "run-passkey-compare: PASS"; else echo "run-passkey-compare: FAIL"; fi
exit "$status"
