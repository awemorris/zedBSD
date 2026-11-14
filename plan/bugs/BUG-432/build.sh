#!/bin/sh
# Builds plan/bugs/BUG-432/flush.c for x86_64 zedBSD four ways, against build/x86_64/sysroot (make sysroot first):
#   OUT/flush-static        the static start (crt0.o) with libc.a, as the tests in the tree link a static program
#   OUT/flush-static-driver the compiler driver's own static link (-static: crt1.o, libc.a)
#   OUT/flush-dynamic       crt1.o with libc.so and /lib/ld.so, as the tree's dynamic programs
#   OUT/flush-driver        the compiler driver's default link (crt1.o, -lc)
# and OUT/ptyrun (ptyrun.c, the driver's default link), which runs one on a pseudo-terminal, and
# OUT/tls-check-static (plan/ws073/tests/p038/tls-check.c, linked as flush-static), a static program of the tree's
# tests that returns from main after its last line.
# A link that fails is reported and left out.
#   sh plan/bugs/BUG-432/build.sh OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?OUT}
cd "$(dirname "$0")/../../.."
S=$PWD/build/x86_64/sysroot
L=$S/usr/lib
C=build/llvm/bin/clang
T="--target=x86_64-unknown-zedbsd --sysroot=$S -m64"
mkdir -p "$OUT"
status=0
# shellcheck disable=SC2086
$C $T -march=x86-64 -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -nostdinc -isystem "$S/usr/include" -ffreestanding \
	-fno-stack-protector -fPIC -O2 -Wall -Wextra -Werror -c plan/bugs/BUG-432/flush.c -o "$OUT/flush.o" || exit 1
# shellcheck disable=SC2086
$C $T -nostdlib -static -Wl,--build-id=none -Wl,-T,$L/zedbsd/x86_64/user.ld $L/crt0.o "$OUT/flush.o" \
	-Wl,--start-group $L/libc.a $L/libzedbsd-compiler-rt.a $L/libclang_rt.builtins.a -Wl,--end-group \
	-o "$OUT/flush-static" || { echo "build: flush-static failed"; status=1; }
# shellcheck disable=SC2086
$C $T -static "$OUT/flush.o" -o "$OUT/flush-static-driver" > "$OUT/static-driver.txt" 2>&1 ||
	echo "build: flush-static-driver failed ($(grep -c 'undefined symbol' "$OUT/static-driver.txt") undefined symbols)"
# shellcheck disable=SC2086
$C $T -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=gnu,-z,now,-z,relro -Wl,--allow-shlib-undefined \
	-Wl,--dynamic-linker=/lib/ld.so $L/crt1.o "$OUT/flush.o" -L$L -l:libc.so -o "$OUT/flush-dynamic" ||
	{ echo "build: flush-dynamic failed"; status=1; }
# shellcheck disable=SC2086
$C $T "$OUT/flush.o" -o "$OUT/flush-driver" > "$OUT/driver.txt" 2>&1 || { echo "build: flush-driver failed"; cat "$OUT/driver.txt"; }
# shellcheck disable=SC2086
$C $T -nostdinc -isystem "$S/usr/include" -ffreestanding -fno-stack-protector -O2 -Wall -Wextra -Werror \
	plan/bugs/BUG-432/ptyrun.c -o "$OUT/ptyrun" || { echo "build: ptyrun failed"; status=1; }
# shellcheck disable=SC2086
$C $T -march=x86-64 -mno-red-zone -DHAL_ARCH_X86_64 -DKERN_USER_ABI_LP64 -nostdinc -isystem "$S/usr/include" \
	-ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -Werror \
	-c plan/ws073/tests/p038/tls-check.c -o "$OUT/tls-check-static.o" &&
	$C $T -nostdlib -static -Wl,--build-id=none -Wl,-T,$L/zedbsd/x86_64/user.ld $L/crt0.o "$OUT/tls-check-static.o" \
	-Wl,--start-group $L/libc.a $L/libzedbsd-compiler-rt.a $L/libclang_rt.builtins.a -Wl,--end-group \
	-o "$OUT/tls-check-static" || { echo "build: tls-check-static failed"; status=1; }
ls "$OUT"/flush-* "$OUT/ptyrun" "$OUT/tls-check-static" 2>/dev/null
exit $status
