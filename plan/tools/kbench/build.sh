#!/bin/bash
# Builds kbench for an amd64 zedBSD guest (a PIE against the guest's libc.so).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/tools/kbench/build.sh BUILD OUTPUT
#
#   BUILD    an amd64 build directory with dynamic/libc.so (the guest's)
#   OUTPUT   where the program goes
#
# Put the program in the guest (extra-files of plan/tools/guest/guest.py, or
# over SSH) and run it once to warm up, then several times; compare medians.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=$1
output=$2
sysroot=$root/build/amd64/sysroot
clang=$root/build/llvm/bin/clang
object=$output.o

"$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -nostdinc \
	-I"$root" -I"$root/include" -isystem "$sysroot/usr/include" \
	-DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC -m64 -march=x86-64 -O2 -fPIC \
	-fno-stack-protector -Wall -Wextra -Werror \
	-c "$here/kbench.c" -o "$object"
"$clang" --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -nostdlib -pie \
	-Wl,--no-relax -Wl,--hash-style=sysv,-z,now -Wl,--allow-shlib-undefined \
	-Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" "$object" \
	-L"$build/dynamic" -Wl,-rpath-link,"$build/dynamic" -l:libc.so -o "$output"
rm -f "$object"
