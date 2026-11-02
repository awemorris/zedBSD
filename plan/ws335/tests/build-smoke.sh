#!/bin/sh
# Builds crypto-smoke (crypto-smoke.c) for amd64 zedBSD with the shared clang, linked against BUILD's
# libcrypto-compat.so and libc.so (the dynamic program of the test image).
#   sh plan/ws335/tests/build-smoke.sh BUILD OUT      (BUILD has dynamic/libcrypto-compat.so and dynamic/libc.so;
#                                                     OUT is a link to a fresh directory, plan/tools/fresh-out.sh)
# The program is OUT/crypto-smoke.  Last line: build-smoke: PASS or build-smoke: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
[ $# -eq 2 ] || { echo "usage: build-smoke.sh BUILD OUT" >&2; exit 2; }
build=$1
out=$2
sysroot=$PWD/build/amd64/sysroot
cc="$PWD/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
for file in "$build/dynamic/libcrypto-compat.so" "$build/dynamic/libc.so" "$sysroot/usr/lib/crt1.o"; do
	[ -f "$file" ] || { echo "build-smoke: no $file"; echo "build-smoke: FAIL"; exit 1; }
done
. plan/tools/fresh-out.sh
fresh_out "$out"
$cc -nostdinc -I. -Iinclude -isystem "$sysroot/usr/include" -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC \
    -m64 -march=x86-64 -mno-red-zone -O2 -ffreestanding -fPIE -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
    -c plan/ws335/tests/crypto-smoke.c -o "$out/crypto-smoke.o"
$cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
    -Wl,-z,stack-size=0x100000 -Wl,--dynamic-linker=/lib/ld.so \
    "$sysroot/usr/lib/crt1.o" "$out/crypto-smoke.o" -L"$build/dynamic" -Wl,-rpath-link,"$build/dynamic" \
    -l:libcrypto-compat.so -l:libc.so -o "$out/crypto-smoke"
python3 tools/build/check-dynamic-elf.py --machine amd64 --role application \
    --needed libcrypto-compat.so --needed libc.so "$out/crypto-smoke"
echo "build-smoke: PASS"
