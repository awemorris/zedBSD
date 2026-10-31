#!/bin/sh
# The contract ssh expects of the crypto library (crypto-contract.c): builds it with ASan and UBSan against the
# host's OpenSSL 3.5 (through plan/ws336/tests/host-crypto), or against libcrypto-compat with
# SSH_CRYPTO_INCLUDE=<dir whose compat/crypto/ holds its headers> SSH_CRYPTO_LIB=<library to link>, and runs it.
#   sh plan/ws336/tests/crypto-contract.sh
# Last line: crypto-contract: PASS or crypto-contract: FAIL.  Output under build/tmp/ssh-crypto-contract.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ssh-crypto-contract
fresh_out "$out"

crypto_include=${SSH_CRYPTO_INCLUDE:-plan/ws336/tests/host-crypto}
crypto_lib=${SSH_CRYPTO_LIB:--lcrypto}

if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iplan/ws336/tests/unit -I"$crypto_include" \
    plan/ws336/tests/crypto-contract.c $crypto_lib -o "$out/crypto-contract" > "$out/build.log" 2>&1; then
	head -20 "$out/build.log"
	echo "crypto-contract: FAIL (build)"
	exit 1
fi
if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    timeout 300 "$out/crypto-contract" > "$out/run.log" 2>&1; then
	tail -1 "$out/run.log"
	echo "crypto-contract: PASS"
	exit 0
fi
grep '^FAIL' "$out/run.log" | head -20
tail -1 "$out/run.log"
echo "crypto-contract: FAIL"
exit 1
