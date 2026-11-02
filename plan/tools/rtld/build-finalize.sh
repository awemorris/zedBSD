#!/bin/sh
# Builds rtld-finalize (rtld-finalize.c) and its two libraries on the host with the target's clang, against BUILD's
# libc.so, and puts them in OUT/../rtld-finalize.tar (ustar, for the guest's pax):
#   libfinalize-plain.so  finalize-lib.c and finalize-cxx.cc, which dlclose() unloads
#   libfinalize-keep.so   the same, -z nodelete (FLAGS_1 NODELETE)
# The libraries are compiled with -O2; the script checks that lib_register_tail() reaches atexit() by a tail call
# (a jmp, not a call), that the C++ static object's destructor is registered with __cxa_atexit, and that the
# program exports finalize_record and finalize_count.  rtld-finalize.sh runs it in the guest.
#
#   plan/tools/rtld/build-finalize.sh BUILD OUT    (BUILD has dynamic/libc.so and sysroot; OUT is made afresh,
#                                                plan/tools/fresh-out.sh)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
[ $# -eq 2 ] || { echo "usage: build-finalize.sh BUILD OUT" >&2; exit 2; }
build=$1
out=$2
tests=plan/tools/rtld
sysroot=$PWD/build/amd64/sysroot
[ -d "$build/sysroot" ] && sysroot=$(cd "$build/sysroot" && pwd)
readelf=build/llvm/bin/llvm-readelf
objdump=build/llvm/bin/llvm-objdump
cc="$PWD/build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
cxx="$PWD/build/llvm/bin/clang++ --target=x86_64-unknown-zedbsd --sysroot=$sysroot"
cflags="-nostdinc -I. -Iinclude -isystem $sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC
 -m64 -march=x86-64 -mno-red-zone -O2 -ffreestanding -fPIC -fno-builtin -fno-stack-protector -Wall -Wextra -Werror"
cxxflags="-nostdinc -nostdinc++ -m64 -march=x86-64 -mno-red-zone -O2 -ffreestanding -fPIC -fno-exceptions -fno-rtti
 -fno-stack-protector -Wall -Wextra -Werror"
ldshared="-m64 -nostdlib -shared -Wl,--hash-style=gnu,-z,now,-z,relro"
libc="-L$build/dynamic -Wl,-rpath-link,$build/dynamic -l:libc.so"
[ -f "$build/dynamic/libc.so" ] || { echo "build-finalize: no $build/dynamic/libc.so" >&2; exit 1; }
. plan/tools/fresh-out.sh
fresh_out "$out"
mkdir -p "$out/obj"

# The two libraries, each of the C part and the C++ part.
for name in plain keep; do
	$cc $cflags -DFINALIZE_TAG="\"$name\"" -c $tests/finalize-lib.c -o "$out/obj/$name-c.o"
	$cxx $cxxflags -DFINALIZE_TAG="\"$name\"" -c $tests/finalize-cxx.cc -o "$out/obj/$name-cxx.o"
	extra=
	[ $name = keep ] && extra=-Wl,-z,nodelete
	$cc $ldshared -Wl,-soname,libfinalize-$name.so $extra "$out/obj/$name-c.o" "$out/obj/$name-cxx.o" $libc \
	    -o "$out/libfinalize-$name.so"
done

# keep alone carries DF_1_NODELETE.
for name in plain keep; do
	flags=$($readelf -d "$out/libfinalize-$name.so" | sed -n 's/.*(FLAGS_1) *//p')
	echo "build-finalize: libfinalize-$name.so FLAGS_1=${flags:-none}"
	case $name:$flags in
	keep:*NODELETE*) ;;
	keep:*|*:*NODELETE*) echo "build-finalize: libfinalize-$name.so has the wrong FLAGS_1" >&2; exit 1 ;;
	esac
done

# lib_register_tail ends in a jump to atexit, so atexit's return address is not in the library.
tail_code=$($objdump -d --no-show-raw-insn --disassemble-symbols=lib_register_tail "$out/libfinalize-plain.so")
echo "$tail_code" | grep -Eq 'jmp.*<atexit@plt>' ||
    { echo "build-finalize: lib_register_tail does not tail-call atexit" >&2; echo "$tail_code" >&2; exit 1; }
echo "$tail_code" | grep -Eq 'call.*<atexit@plt>' &&
    { echo "build-finalize: lib_register_tail calls atexit" >&2; exit 1; }
echo "build-finalize: lib_register_tail tail-calls atexit"

# The C++ object's destructor is registered through __cxa_atexit, with the linker's __dso_handle.
$readelf --dyn-syms "$out/libfinalize-plain.so" | grep -q ' UND __cxa_atexit$' ||
    { echo "build-finalize: the library does not import __cxa_atexit" >&2; exit 1; }
$readelf -s "$out/libfinalize-plain.so" | grep -q 'HIDDEN .* __dso_handle$' ||
    { echo "build-finalize: no __dso_handle in the library" >&2; exit 1; }

# The program, linked like dyntest (platform/amd64/vmunix.mk), exporting what the libraries record through.
$cc $cflags -c $tests/rtld-finalize.c -o "$out/obj/rtld-finalize.o"
$cc -m64 -nostdlib -pie -Wl,--no-relax -Wl,--hash-style=sysv,-z,now,-z,relro,-z,separate-code \
    -Wl,-z,stack-size=0x100000,--allow-shlib-undefined -Wl,--dynamic-linker=/lib/ld.so \
    -Wl,--export-dynamic-symbol=finalize_record,--export-dynamic-symbol=finalize_count \
    "$sysroot/usr/lib/crt1.o" "$out/obj/rtld-finalize.o" $libc -o "$out/rtld-finalize"
for symbol in finalize_record finalize_count; do
	$readelf --dyn-syms "$out/rtld-finalize" | grep -Eq "FUNC +GLOBAL .* $symbol\$" ||
	    { echo "build-finalize: the program does not export $symbol" >&2; exit 1; }
done

# One archive for the guest.
(cd "$out" && tar --format=ustar -cf ../rtld-finalize.tar ./*.so rtld-finalize)
echo "build-finalize: $(dirname -- "$out")/rtld-finalize.tar"
