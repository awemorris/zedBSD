#!/bin/sh
# ws333: one single-file command of this tree (userland/base/sysctl, userland/tests/sleepctl, ...) linked statically
# for x86_64, for a zedBSD whose userland predates the tree's shared libc or ABI (the 5330's NVMe), the way
# thermal-static.sh links /sbin/thermal.  Needs build/x86_64/sysroot (made again after an include/uapi change).
#   sh plan/ws333/tests/hw5330/static-tool.sh SRCDIR OUT       (OUT/<name of SRCDIR>)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
src=${1:?SRCDIR}
out=${2:?OUT}
name=$(basename "$src")
mkdir -p "$out"
cc=build/llvm/bin/clang
sr=build/x86_64/sysroot
lib=$sr/usr/lib
flags="--target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64
	-DKERN_USER_ABI_LP64 -nostdinc -isystem $sr/usr/include -ffreestanding -fno-pic -fno-pie -fno-stack-protector
	-Os -Wall -Wextra -Werror"
# shellcheck disable=SC2086
$cc $flags -c "$src/main.c" -o "$out/$name.o"
$cc --target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -nostdlib -static -Wl,--build-id=none \
	-Wl,-T,$lib/zedbsd/x86_64/user.ld $lib/crt0.o "$out/$name.o" -Wl,--start-group $lib/libc.a \
	$lib/libzedbsd-compiler-rt.a $lib/libclang_rt.builtins.a -Wl,--end-group -o "$out/$name"
echo "static-tool: $out/$name $(sha256sum "$out/$name" | cut -c1-16)"
