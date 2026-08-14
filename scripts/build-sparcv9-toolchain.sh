#!/usr/bin/env bash
# Build the freestanding SPARC V9 toolchain used by zedBSD.
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
set -euo pipefail

binutils_version=2.45
gcc_version=15.2.0
target=sparc64-unknown-elf
prefix="$HOME/opt/sparc64"
work="$HOME/.cache/zedbsd-sparcv9-toolchain"
jobs=$(nproc)

usage()
{
	cat <<EOF
usage: $0 [--prefix DIR] [--work DIR] [--jobs N]

Builds binutils $binutils_version and GCC $gcc_version for $target.
The default prefix is $prefix.
EOF
}

while test "$#" -gt 0; do
	case "$1" in
	--prefix)
		test "$#" -ge 2 || { usage >&2; exit 2; }
		prefix="$2"
		shift 2
		;;
	--work)
		test "$#" -ge 2 || { usage >&2; exit 2; }
		work="$2"
		shift 2
		;;
	--jobs)
		test "$#" -ge 2 || { usage >&2; exit 2; }
		jobs="$2"
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		echo "unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

case "$prefix" in
/*) ;;
*)
	echo "prefix must be an absolute path: $prefix" >&2
	exit 2
	;;
esac

case "$work" in
/*) ;;
*)
	echo "work directory must be an absolute path: $work" >&2
	exit 2
	;;
esac

case "$jobs" in
''|*[!0-9]*|0)
	echo "jobs must be a positive integer: $jobs" >&2
	exit 2
	;;
esac

downloads="$work/downloads"
sources="$work/sources"
builds="$work/build"
binutils_archive="$downloads/binutils-$binutils_version.tar.xz"
gcc_archive="$downloads/gcc-$gcc_version.tar.xz"
binutils_source="$sources/binutils-$binutils_version"
gcc_source="$sources/gcc-$gcc_version"

mkdir -p "$downloads" "$sources" "$builds" "$prefix"

fetch()
{
	url="$1"
	output="$2"
	if test -f "$output"; then
		return
	fi
	echo "Downloading $url"
	curl --fail --location --retry 3 --output "$output.tmp" "$url"
	mv "$output.tmp" "$output"
}

fetch "https://ftp.gnu.org/gnu/binutils/binutils-$binutils_version.tar.xz" \
	"$binutils_archive"
echo "c50c0e7f9cb188980e2cc97e4537626b1672441815587f1eab69d2a1bfbef5d2  $binutils_archive" |
	sha256sum -c -

fetch "https://gcc.gnu.org/pub/gcc/releases/gcc-$gcc_version/gcc-$gcc_version.tar.xz" \
	"$gcc_archive"
echo "89047a2e07bd9da265b507b516ed3635adb17491c7f4f67cf090f0bd5b3fc7f2ee6e4cc4008beef7ca884b6b71dffe2bb652b21f01a702e17b468cca2d10b2de  $gcc_archive" |
	sha512sum -c -

if ! test -d "$binutils_source"; then
	tar -C "$sources" -xf "$binutils_archive"
fi
if ! test -d "$gcc_source"; then
	tar -C "$sources" -xf "$gcc_archive"
	(
		cd "$gcc_source"
		./contrib/download_prerequisites
	)
fi

binutils_build="$builds/binutils-$binutils_version"
mkdir -p "$binutils_build"
if ! test -f "$binutils_build/Makefile"; then
	(
		cd "$binutils_build"
		"$binutils_source/configure" \
			--target="$target" \
			--prefix="$prefix" \
			--disable-nls \
			--disable-werror \
			--disable-gdb \
			--disable-sim
	)
fi
make -C "$binutils_build" -j"$jobs" MAKEINFO=true
make -C "$binutils_build" install MAKEINFO=true

export PATH="$prefix/bin:$PATH"

gcc_build="$builds/gcc-$gcc_version-$target-soft"
mkdir -p "$gcc_build"
if ! test -f "$gcc_build/Makefile"; then
	(
		cd "$gcc_build"
		"$gcc_source/configure" \
			--target="$target" \
			--prefix="$prefix" \
			--with-cpu-64=ultrasparc \
			--with-float=soft \
			--enable-languages=c \
			--without-headers \
			--disable-bootstrap \
			--disable-nls \
			--disable-shared \
			--disable-threads \
			--disable-multilib \
			--disable-libatomic \
			--disable-libgomp \
			--disable-libquadmath \
			--disable-libsanitizer \
			--disable-libssp
	)
fi
make -C "$gcc_build" -j"$jobs" all-gcc all-target-libgcc MAKEINFO=true
make -C "$gcc_build" install-gcc install-target-libgcc MAKEINFO=true

repo=$(cd "$(dirname "$0")/.." && pwd)
probe_build="$work/abi-probe"
mkdir -p "$probe_build"
common_flags="-m64 -mcpu=ultrasparc -mstack-bias -mcmodel=medany -msoft-float"
"$prefix/bin/$target-gcc" $common_flags -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -mno-app-regs -c \
	"$repo/tests/sparcv9/abi-probe.c" -o "$probe_build/abi-probe.o"
"$prefix/bin/$target-gcc" $common_flags -ffreestanding -fno-pic -fno-pie \
	-c "$repo/tests/sparcv9/abi-start.S" -o "$probe_build/abi-start.o"
"$prefix/bin/$target-ld" -m elf64_sparc -nostdlib -e _start \
	-Ttext=0x400000 "$probe_build/abi-start.o" "$probe_build/abi-probe.o" \
	-o "$probe_build/abi-probe.elf"

"$prefix/bin/$target-readelf" -h "$probe_build/abi-probe.elf" |
	grep -q 'Class:.*ELF64'
"$prefix/bin/$target-readelf" -h "$probe_build/abi-probe.elf" |
	grep -q 'Data:.*big endian'
"$prefix/bin/$target-readelf" -h "$probe_build/abi-probe.elf" |
	grep -q 'Machine:.*Sparc v9'
test -z "$("$prefix/bin/$target-nm" -u "$probe_build/abi-probe.elf")"

echo "SPARC V9 toolchain: PASS"
echo "  prefix: $prefix"
echo "  target: $target"
