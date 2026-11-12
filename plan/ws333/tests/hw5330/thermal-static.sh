#!/bin/sh
# ws333: /sbin/thermal of this tree linked statically for x86_64, for a zedBSD whose userland predates the
# tree's shared libc (the 5330's NVMe), the way the sysroot's smoke link is made, with a main that flushes stdio
# (the static start does not).  Needs build/x86_64/sysroot.
#   sh plan/ws333/tests/hw5330/thermal-static.sh OUT       (OUT/thermal)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
out=${1:?OUT}
mkdir -p "$out"
cc=build/llvm/bin/clang
sr=build/x86_64/sysroot
lib=$sr/usr/lib
$cc --target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64 \
	-DKERN_USER_ABI_LP64 -nostdinc -isystem $sr/usr/include -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-Os -Wall -Wextra -Werror -Dmain=thermal_main -c userland/base/thermal/main.c -o "$out/thermal.o"
# The static start returns from main without flushing stdio; this main flushes before it returns.
printf '%s\n' '#include <stdio.h>' 'int thermal_main(int, char **);' \
	'int main(int argc, char **argv) { int status = thermal_main(argc, argv); fflush(NULL); return status; }' > "$out/flush.c"
$cc --target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64 \
	-DKERN_USER_ABI_LP64 -nostdinc -isystem $sr/usr/include -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
	-Os -Wall -Wextra -Werror -c "$out/flush.c" -o "$out/flush.o"
$cc --target=x86_64-unknown-zedbsd --sysroot=$sr -m64 -nostdlib -static -Wl,--build-id=none \
	-Wl,-T,$lib/zedbsd/x86_64/user.ld $lib/crt0.o "$out/thermal.o" "$out/flush.o" -Wl,--start-group $lib/libc.a \
	$lib/libzedbsd-compiler-rt.a $lib/libclang_rt.builtins.a -Wl,--end-group -o "$out/thermal"
echo "thermal-static: $out/thermal $(sha256sum "$out/thermal" | cut -c1-16)"
