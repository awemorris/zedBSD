#!/bin/sh
# netsleep (main.c here, with userland/base/net/protocol.c) linked statically for x86_64, for the 5330's zedBSD,
# the way plan/ws333/tests/hw5330/static-tool.sh links a tool.  Needs build/x86_64/sysroot.
#   sh plan/bugs/BUG-424/netsleep/build.sh OUT       (OUT/netsleep)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
out=${1:?OUT}
mkdir -p "$out"
cc=build/llvm/bin/clang
sr=build/x86_64/sysroot
lib=$sr/usr/lib
flags="--target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64
	-DKERN_USER_ABI_LP64 -nostdinc -isystem $sr/usr/include -I. -ffreestanding -fno-pic -fno-pie -fno-stack-protector
	-Os -Wall -Wextra -Werror"
# shellcheck disable=SC2086
$cc $flags -c plan/bugs/BUG-424/netsleep/main.c -o "$out/netsleep.o"
# shellcheck disable=SC2086
$cc $flags -c userland/base/net/protocol.c -o "$out/netsleep-protocol.o"
$cc --target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -nostdlib -static -Wl,--build-id=none \
	-Wl,-T,$lib/zedbsd/x86_64/user.ld $lib/crt0.o "$out/netsleep.o" "$out/netsleep-protocol.o" \
	-Wl,--start-group $lib/libc.a $lib/libzedbsd-compiler-rt.a $lib/libclang_rt.builtins.a -Wl,--end-group -o "$out/netsleep"
echo "netsleep: $out/netsleep $(sha256sum "$out/netsleep" | cut -c1-16)"
