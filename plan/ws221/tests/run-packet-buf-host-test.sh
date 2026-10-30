#!/bin/sh
# The packet pool's host test (packet-buf-host-test.c): src/kern/net/packet-buf.c built as the kernel builds it
# (freestanding, the kernel's headers), once as a 64-bit machine with jumbo packets and once as a 32-bit board without.
# OUT= chooses the build folder (default build/ws221-host).  The last line is "run-packet-buf-host-test: PASS|FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=${OUT:-build/ws221-host}
mkdir -p "$out"
cc=${CC:-clang}
status=0
for arch in AMD64 I386; do
	bits=; [ $arch = I386 ] && bits="-m32 -DKERN_ATOMIC_SERIALIZED"
	flags="-std=c11 -O1 -g -nostdlibinc -Iinclude -Isrc -I. -DKERN_UAPI_NATIVE -DKERN_KCRT_NATIVE -DHAL_ARCH_$arch
		-ffreestanding -fno-builtin -Wall -Wextra -Werror $bits"
	# shellcheck disable=SC2086
	$cc $flags -c src/kern/net/packet-buf.c -o "$out/packet-buf-$arch.o" &&
	    $cc -std=c11 -O1 -g -Iinclude -Isrc -I. -DKERN_UAPI_NATIVE -DKERN_KCRT_NATIVE -DHAL_ARCH_$arch -Wall -Wextra -Werror $bits \
	    -c plan/ws221/tests/packet-buf-host-test.c -o "$out/packet-buf-host-test-$arch.o" &&
	    $cc $bits -o "$out/packet-buf-host-test-$arch" "$out/packet-buf-$arch.o" "$out/packet-buf-host-test-$arch.o" -lc ||
	    { echo "run-packet-buf-host-test: build for $arch failed"; status=1; continue; }
	echo "== $arch"
	timeout 60 "$out/packet-buf-host-test-$arch" || status=1
done
[ $status = 0 ] && echo "run-packet-buf-host-test: PASS" || echo "run-packet-buf-host-test: FAIL"
exit $status
