#!/bin/sh
# Host test of the x86_64 memcpy of src/libc/string.c: the file is built as the amd64 userland builds it, its memcpy
# renamed zb_memcpy, and linked with memcpy-test.c; then again with the "rep movsb" path cut off, as on a processor
# without Enhanced REP MOVSB.  The last line is
# "memcpy-test: PASS|FAIL".  Output in OUT (default build/tmp/memcpy-test).
#   sh plan/ws299/tests/host/memcpy-test.sh [OUT]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
OUT=${1:-build/tmp/memcpy-test}
S=build/x86_64/sysroot
[ -f "$S/usr/include/string.h" ] || { echo "memcpy-test: no sysroot at $S (make CONFIGMK=config/config.x86_64.mk sysroot)" >&2; exit 1; }
C=build/llvm/bin/clang
mkdir -p "$OUT"
# Builds string.c as the amd64 userland does (FILE), renames its memcpy, links the test, runs it (NAME).
one() {
	"$C" --target=x86_64-unknown-zedbsd -nostdinc -isystem "$S/usr/include" -Iinclude -Isrc -I. -DHAL_ARCH_X86_64 \
		-DKERN_USER_ABI_LP64 -m64 -march=x86-64 -mno-red-zone -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
		-fno-builtin -fno-common -ffunction-sections -fdata-sections -Os -Wall -Wextra -Werror \
		-c "$1" -o "$OUT/$2.o"
	build/llvm/bin/llvm-objcopy --redefine-sym memcpy=zb_memcpy "$OUT/$2.o" "$OUT/$2-zb.o"
	cc -O1 -Wall -Wextra -no-pie plan/ws299/tests/host/memcpy-test.c "$OUT/$2-zb.o" -o "$OUT/$2-test"
	echo "memcpy-test: $2"
	"$OUT/$2-test"
}
one src/libc/string.c string
# The same without "rep movsb", as on a processor without Enhanced REP MOVSB (the copy of the file is the test's).
sed 's/if (count >= STRING_REP_MOVSB_MINIMUM \&\& string_erms_present())/if (0 \&\& string_erms_present())/' src/libc/string.c > "$OUT/string-noerms.c"
grep -q "if (0 " "$OUT/string-noerms.c"
one "$OUT/string-noerms.c" string-noerms
