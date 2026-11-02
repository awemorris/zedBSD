#!/bin/sh
# The image check of libcrypto-compat.so on zedBSD (amd64, QEMU, the serial console): builds the library and libc
# with config.mk, builds crypto-smoke (build-smoke.sh), builds the image with the library and the program in it,
# boots it, logs in on the serial line and runs the program.  The last line is run-image: PASS or run-image: FAIL;
# the output of the guest is in OUT/guest.txt.
#   sh plan/ws335/tests/run-image.sh [BUILD [OUT]]      (BUILD default build/ws335-image, OUT default BUILD/run)
# The run is the serial harness of plan/tools/guest/amd64-serial.sh (snapshot disk, nothing is left in the image).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
build=${1:-build/ws335-image}
out=${2:-$build/run}
. plan/tools/fresh-out.sh
fresh_out "$out"
config=plan/ws335/tests/config.mk
fail() {
	echo "run-image: FAIL ($1)"
	exit 1
}

timeout 1800 make ZEDBSD_CONFIG=$config BUILD="$build" "$build/dynamic/libcrypto-compat.so" "$build/dynamic/libc.so" \
    > "$out/make-library.txt" 2>&1 || fail "library build, $out/make-library.txt"
sh plan/ws335/tests/build-smoke.sh "$build" "$build/smoke" > "$out/build-smoke.txt" 2>&1 || fail "smoke build, $out/build-smoke.txt"
timeout 3000 sh plan/tools/guest/test-image.sh --no-harness $config "$build" \
    --file /root/crypto-smoke="$build/smoke/crypto-smoke" --mode /root/crypto-smoke=0755 \
    > "$out/make-image.txt" 2>&1 || fail "image build, $out/make-image.txt"
sync
RUN="$out/serial" TIMEOUT=240 timeout 900 sh plan/tools/guest/amd64-serial.sh "$build/hdd-image.img" \
    'ls -l /lib/libcrypto-compat.so /root/crypto-smoke' '/root/crypto-smoke; echo exit=$?' > "$out/guest.txt" 2>&1
sync
cat "$out/guest.txt"
grep -q '^amd64: login prompt' "$out/guest.txt" || fail "no login prompt"
grep -q 'libcrypto-compat.so' "$out/guest.txt" || fail "library not in the image"
[ "$(grep -c '^crypto-smoke: PASS' "$out/guest.txt")" -ge 1 ] || fail "crypto-smoke did not pass"
grep -q '^exit=0' "$out/guest.txt" || fail "crypto-smoke exit status"
echo "run-image: PASS"
