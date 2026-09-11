#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
output=${1:?usage: build-tls-fixtures.sh OUTPUT}
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
for arch in amd64 i386; do
	triple=x86_64-unknown-zedbsd
	define=HAL_ARCH_AMD64
	extra="-mno-red-zone"
	if test "$arch" = i386; then triple=i386-unknown-zedbsd; define=HAL_ARCH_I386; extra="-msoft-float -mno-80387 -mno-mmx -mno-sse -mno-sse2"; fi
	"$repo/build/llvm/bin/clang" --target="$triple" -ffreestanding -nostdinc -I"$repo/libc/include" -I"$repo/include/uapi" \
		-fno-pic -fno-pie -ftls-model=local-exec -O2 $extra -c \
		"$repo/plan/ws022/tests/tls-image.c" -o "$output/$arch.o"
	"$repo/build/llvm/bin/clang" --target="$triple" -D"$define" -I"$repo/include" \
		-c "$repo/userland/base/libc/syscall-$arch.S" -o "$output/$arch-syscall.o"
	"$repo/build/llvm/bin/ld.lld" -static \
		-T "$repo/plan/ws022/tests/tls-image.ld" \
		"$output/$arch.o" "$output/$arch-syscall.o" -o "$output/$arch.elf"
	# Exercise zero-only, empty, and nonzero first-byte TLS offsets.
	for kind in empty zero; do
		mode=''
		if test "$kind" = zero; then mode=-DTLS_ZERO_ONLY; fi
		"$repo/build/llvm/bin/clang" --target="$triple" -ffreestanding -nostdinc \
			-I"$repo/libc/include" -I"$repo/include/uapi" -fno-pic -fno-pie \
			-ftls-model=local-exec -O2 $extra $mode -c \
			"$repo/plan/ws022/tests/tls-empty-image.c" -o "$output/$arch-$kind.o"
		"$repo/build/llvm/bin/ld.lld" -static \
			-T "$repo/plan/ws022/tests/tls-image.ld" \
			"$output/$arch-$kind.o" "$output/$arch-syscall.o" -o "$output/$arch-$kind.elf"
	done
	sed 's/ .tdata :/ . += 4; .tdata :/' "$repo/plan/ws022/tests/tls-image.ld" > "$output/offset.ld"
	"$repo/build/llvm/bin/ld.lld" -static -T "$output/offset.ld" \
		"$output/$arch.o" "$output/$arch-syscall.o" -o "$output/$arch-offset.elf"
	"$repo/build/llvm/bin/llvm-readelf" -lWs "$output/$arch.elf" > "$output/$arch.readelf"
	"$repo/build/llvm/bin/llvm-objdump" -d "$output/$arch.elf" > "$output/$arch.disassembly"
done
python3 "$repo/plan/ws022/tests/mutate-tls-fixtures.py" "$output"
