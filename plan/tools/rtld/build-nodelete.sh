#!/bin/sh
# Builds rtld-nodelete (rtld-nodelete.c) and its four libraries on the host with the target's clang, against BUILD's
# libc.so, and puts them in OUT/../rtld-nodelete.tar (ustar, for the guest's pax):
#   libnodelete-dep.so    nodelete-dep.c, nodelete_dep_value() returns 40
#   libnodelete-keep.so   nodelete-lib.c, -z nodelete (FLAGS_1 NODELETE), needs libnodelete-dep.so, lib_value() 42
#   libnodelete-flag.so   nodelete-lib.c, no flag in the file (the program opens it with RTLD_NODELETE), lib_value() 7
#   libnodelete-plain.so  nodelete-lib.c, the control, lib_value() 5
# rtld-nodelete.sh runs it in the guest.
#
#   plan/tools/rtld/build-nodelete.sh BUILD OUT    (BUILD has dynamic/libc.so and sysroot; OUT is made afresh,
#                                                plan/tools/fresh-out.sh)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
[ $# -eq 2 ] || { echo "usage: build-nodelete.sh BUILD OUT" >&2; exit 2; }
build=$1
out=$2
tests=plan/tools/rtld
sysroot=$PWD/build/amd64/sysroot
[ -d "$build/sysroot" ] && sysroot=$(cd "$build/sysroot" && pwd)
readelf=build/llvm/bin/llvm-readelf
cc="$PWD/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
cflags="-nostdinc -I. -Iinclude -isystem $sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC
 -m64 -march=x86-64 -mno-red-zone -O2 -ffreestanding -fPIC -fno-builtin -fno-stack-protector -Wall -Wextra -Werror"
ldshared="-m64 -nostdlib -shared -Wl,--hash-style=gnu,-z,now,-z,relro"
libc="-L$build/dynamic -Wl,-rpath-link,$build/dynamic -l:libc.so"
[ -f "$build/dynamic/libc.so" ] || { echo "build-nodelete: no $build/dynamic/libc.so" >&2; exit 1; }
grep -q RTLD_NODELETE "$sysroot/usr/include/dlfcn.h" || { echo "build-nodelete: no RTLD_NODELETE in $sysroot" >&2; exit 1; }
. plan/tools/fresh-out.sh
fresh_out "$out"
mkdir -p "$out/obj"

# The dependency, then the three libraries of one source.
$cc $cflags -c $tests/nodelete-dep.c -o "$out/obj/nodelete-dep.o"
$cc $ldshared -Wl,-soname,libnodelete-dep.so "$out/obj/nodelete-dep.o" $libc -o "$out/libnodelete-dep.so"
$cc $cflags -DNODELETE_TAG='"keep"' -DNODELETE_VALUE=2 -DNODELETE_DEP -c $tests/nodelete-lib.c -o "$out/obj/keep.o"
$cc $ldshared -Wl,-soname,libnodelete-keep.so,-z,nodelete "$out/obj/keep.o" -L"$out" -l:libnodelete-dep.so $libc \
    -o "$out/libnodelete-keep.so"
$cc $cflags -DNODELETE_TAG='"flag"' -DNODELETE_VALUE=7 -c $tests/nodelete-lib.c -o "$out/obj/flag.o"
$cc $ldshared -Wl,-soname,libnodelete-flag.so "$out/obj/flag.o" $libc -o "$out/libnodelete-flag.so"
$cc $cflags -DNODELETE_TAG='"plain"' -DNODELETE_VALUE=5 -c $tests/nodelete-lib.c -o "$out/obj/plain.o"
$cc $ldshared -Wl,-soname,libnodelete-plain.so "$out/obj/plain.o" $libc -o "$out/libnodelete-plain.so"

# Only keep carries DF_1_NODELETE in its file.
for name in keep flag plain; do
	flags=$($readelf -d "$out/libnodelete-$name.so" | sed -n 's/.*(FLAGS_1) *//p')
	echo "build-nodelete: libnodelete-$name.so FLAGS_1=${flags:-none}"
	case $name:$flags in
	keep:*NODELETE*) ;;
	keep:*|*:*NODELETE*) echo "build-nodelete: libnodelete-$name.so has the wrong FLAGS_1" >&2; exit 1 ;;
	esac
done
$readelf -d "$out/libnodelete-keep.so" | grep -q 'NEEDED.*libnodelete-dep.so' ||
    { echo "build-nodelete: libnodelete-keep.so does not need libnodelete-dep.so" >&2; exit 1; }

# The program, linked like dyntest (platform/amd64/vmunix.mk); it opens the libraries itself.
$cc $cflags -c $tests/rtld-nodelete.c -o "$out/obj/rtld-nodelete.o"
$cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
    -Wl,-z,stack-size=0x100000,--allow-shlib-undefined -Wl,--dynamic-linker=/lib/ld.so \
    "$sysroot/usr/lib/crt1.o" "$out/obj/rtld-nodelete.o" $libc -o "$out/rtld-nodelete"

# One archive for the guest.
(cd "$out" && tar --format=ustar -cf ../rtld-nodelete.tar ./*.so rtld-nodelete)
echo "build-nodelete: $(dirname -- "$out")/rtld-nodelete.tar"
