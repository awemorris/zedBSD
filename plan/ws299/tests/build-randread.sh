#!/bin/sh
# Builds randread.c, patwrite.c, directio.c and cpuloop.c twice: OUT/NAME-zedbsd (static, x86_64 zedBSD, against this
# tree's sysroot build/x86_64/sysroot, made by make sysroot) and OUT/NAME-linux (static, the build host's cc), for
# nvme-bench-5330.sh.  The tree's include/ comes before the sysroot's so that the tree's uapi headers are the ones
# used.
#   sh plan/ws299/tests/build-randread.sh OUT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${1:?OUT}
cd "$(dirname "$0")/../../.."
S=build/x86_64/sysroot
[ -f "$S/usr/lib/libc.a" ] || { echo "build-randread: no sysroot at $S (make sysroot)" >&2; exit 1; }
C=build/llvm/bin/clang
mkdir -p "$OUT"
for name in randread patwrite directio cpuloop; do
	"$C" --target=x86_64-unknown-zedbsd --sysroot="$S" -nostdinc -isystem include -isystem "$S/usr/include" -O2 -Wall -Wextra -Werror \
		-c plan/ws299/tests/$name.c -o "$OUT/$name.o"
	"$C" --target=x86_64-unknown-zedbsd --sysroot="$S" -nostdlib -static -Wl,-T,"$S/usr/lib/zedbsd/x86_64/user.ld" \
		"$S/usr/lib/crt0.o" "$OUT/$name.o" -Wl,--start-group "$S/usr/lib/libc.a" "$S/usr/lib/libzedbsd-compiler-rt.a" \
		"$S/usr/lib/libclang_rt.builtins.a" -Wl,--end-group -o "$OUT/$name-zedbsd"
	cc -O2 -Wall -Wextra -Werror -pthread -static plan/ws299/tests/$name.c -o "$OUT/$name-linux"
done
echo "build-randread: $OUT"
