#!/bin/sh
# The host test of IPv6 fragment reassembly (frag-host-test.c): src/kern/net/ipv6-frag.c, in6.c and packet-buf.c built
# as the kernel builds them (freestanding, the kernel's headers), with ASan and UBSan, once as a 64-bit machine with
# jumbo packets and once as a 32-bit board without.  OUT= chooses the build folder (default build/ws312-host).
# The last line is "run-frag-host-test: PASS|FAIL".
#   plan/tools/ipv6-frag/run-frag-host-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=${OUT:-build/ws312-host}
mkdir -p "$out"
cc=${CC:-clang}
sanitize="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
status=0
for arch in AMD64 I386; do
	bits=; [ $arch = I386 ] && bits="-m32 -DKERN_ATOMIC_SERIALIZED"
	common="-std=c11 -O1 -g -Iinclude -Isrc -I. -DKERN_UAPI_NATIVE -DKERN_KCRT_NATIVE -DHAL_ARCH_$arch -Wall -Wextra -Werror $bits"
	kernel="$common -nostdlibinc -ffreestanding -fno-builtin"
	built=y
	for source in src/kern/net/ipv6-frag.c src/kern/net/in6.c src/kern/net/packet-buf.c; do
		name=$(basename "$source" .c)
		# shellcheck disable=SC2086
		$cc $kernel $sanitize -c "$source" -o "$out/$name-$arch.o" || built=n
	done
	# shellcheck disable=SC2086
	$cc $common $sanitize -c plan/tools/ipv6-frag/frag-host-test.c -o "$out/frag-host-test-$arch.o" || built=n
	# shellcheck disable=SC2086
	[ $built = y ] && $cc $bits $sanitize -o "$out/frag-host-test-$arch" "$out/ipv6-frag-$arch.o" "$out/in6-$arch.o" \
	    "$out/packet-buf-$arch.o" "$out/frag-host-test-$arch.o" || built=n
	if [ $built = n ]; then
		echo "run-frag-host-test: build for $arch failed"
		status=1
		continue
	fi
	echo "== $arch"
	timeout 120 "$out/frag-host-test-$arch" || status=1
done
[ $status = 0 ] && echo "run-frag-host-test: PASS" || echo "run-frag-host-test: FAIL"
exit $status
