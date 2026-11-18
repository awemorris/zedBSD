#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Builds and runs the i915 contract tests on the host, plainly and under
# ASan/UBSan.  Each test links the production sources it checks with a mock
# behind their operations table; see README.md.
#
# Usage: run.sh [test ...]   (default: mmio dma pci rpm pte sync rps memory forget reset)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../../../../../.." && pwd)
driver=$repo/src/drivers/gpu/i915
# A new directory for each run under the checkout's build/tmp, not under /tmp
# (a shared tmpfs); the script removes nothing, build/tmp is cleaned apart.
mkdir -p "$repo/build/tmp"
work=$(mktemp -d "$repo/build/tmp/i915-contracts.XXXXXX")
echo "output: $work"

compiler=${CC:-cc}
tests=${*:-"mmio dma pci rpm pte sync rps memory forget reset"}

warnings="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement"
ordinary="-O2"
sanitized="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

# The driver's own headers come first; the host C library is preferred over
# the zedBSD one, which is only a fallback.
host_headers="-DKERN_USER_ABI_LP64 -I$repo/include -I$repo/src -I$repo -idirafter $repo/include/libc"

# kern/thread.h reaches the zedBSD <sys/types.h>, which clashes with the
# host's; the two files that need the kernel's struct thread are built
# against the zedBSD C library headers instead.
kernel_headers="-DKERN_USER_ABI_LP64 -I$repo/include -I$repo/src -I$repo -I$repo/include/libc -DKERN_UAPI_NATIVE"

# Names the sources one contract test links, besides the shared recorder and stubs.
sources_for() {
	case $1 in
	mmio)
		echo "$here/mmio_contract_test.c $here/mock_mmio.c $driver/mmio.c $driver/trace.c" ;;
	dma)
		echo "$here/dma_contract_test.c $here/mock_dma.c $driver/dma.c $driver/trace.c" ;;
	pci)
		echo "$here/pci_contract_test.c $here/mock_pci.c $driver/pci.c $driver/trace.c" ;;
	rpm)
		echo "$here/rpm_contract_test.c $here/mock_rpm.c $driver/runtime-pm.c $driver/pci.c $driver/trace.c" ;;
	pte)
		echo "$here/pte_contract_test.c $here/host_kernel.c $here/host_thread.c $driver/ggtt.c $driver/ppgtt.c $driver/mmio.c $driver/trace.c" ;;
	sync)
		echo "$here/sync_contract_test.c $here/host_kernel.c $here/host_thread.c $driver/sync.c $driver/workqueue.c $driver/mmio.c $driver/trace.c" ;;
	rps)
		echo "$here/rps_contract_test.c $here/mock_mmio.c $here/host_kernel.c $here/host_thread.c $driver/gt-power.c $driver/workqueue.c $driver/mmio.c $driver/trace.c" ;;
	memory)
		echo "$here/memory_contract_test.c $here/host_render.c $driver/vk/vk-memory.c $driver/vk/vk-object.c $driver/vk/vk-codec.c $driver/vk/vk-reply.c" ;;
	forget)
		echo "$here/forget_contract_test.c $here/host_render.c $driver/vk/vk-forget.c $driver/vk/vk-object.c $driver/vk/vk-descriptor.c $driver/vk/vk-codec.c $driver/vk/vk-reply.c" ;;
	reset)
		echo "$here/reset_contract_test.c $here/mock_mmio.c $driver/reset.c $driver/mmio.c $driver/trace.c" ;;
	*)
		echo "unknown contract test: $1" >&2
		return 1 ;;
	esac
}

# Compiles and links one contract test in one variant; prints the program path.
build() {
	name=$1
	variant=$2
	flags=$3
	directory=$work/$variant/$name
	mkdir -p "$directory"

	sources=$(sources_for "$name")
	objects=
	# The render executor's tests (memory, forget) have the host's heap, locks and log (host_render.c), not the unreached stand-ins.
	unreached=$here/host_unreached.c
	# The engine reset test frees the quarantine it releases, so it brings its own stand-ins as well.
	if [ "$name" = memory ] || [ "$name" = forget ] || [ "$name" = reset ]; then
		unreached=
	fi
	for source in $sources "$here/contract.c" $unreached; do
		base=$(basename "$source" .c)
		case $base in
		workqueue|host_thread)
			headers=$kernel_headers ;;
		*)
			headers=$host_headers ;;
		esac
		"$compiler" $warnings $flags $headers -c "$source" -o "$directory/$base.o"
		objects="$objects $directory/$base.o"
	done

	"$compiler" $flags $objects -o "$directory/$name-contract-test"
	echo "$directory/$name-contract-test"
}

failed=
for name in $tests; do
	echo "=== $name: ordinary ==="
	program=$(build "$name" ordinary "$ordinary")
	if ! "$program"; then
		failed="$failed $name(ordinary)"
	fi

	echo "=== $name: ASan/UBSan ==="
	program=$(build "$name" sanitized "$sanitized")
	if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$program"; then
		failed="$failed $name(sanitized)"
	fi
done

if [ -n "$failed" ]; then
	echo "i915 contract tests FAIL:$failed"
	exit 1
fi
echo "i915 contract tests PASS: $tests (ordinary + ASan/UBSan)"
