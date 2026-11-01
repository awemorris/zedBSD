#!/bin/sh
# OpenSSL for the Windows build: static libssl.a and libcrypto.a cross-built with llvm-mingw from the
# same release tarball the zedBSD package uses (userland/packages/security/openssl/Makefile: version
# and SHA-256 are read from it).
#   sh plan/ws334/tests/openssl-windows.sh <x86_64|aarch64> <output directory>
# The output directory gets include/openssl/*.h, lib/libssl.a, lib/libcrypto.a and the log of the
# build (openssl-build.log); the work (the extracted source and the build tree) is in
# <output directory>/work.  LLVM_MINGW=<root> chooses llvm-mingw (default
# $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64); OPENSSL_SOURCES=<dir> where the tarball is
# kept or fetched to (default build/distfiles when it is there, else <output directory>/sources).
# The last line is openssl-windows: PASS or FAIL.
#
# The libraries are static (no-shared) and never load code at run time (no-dso, no-engine, no-module).
# (--cross-compile-prefix is given because Configure accepts a prefix that is not an absolute path of
# the host only when it cross compiles; it is put before every tool's name, so the tools are named
# clang, ar, ranlib and windres and found in llvm-mingw's bin as <triple>-clang and so on.)
# Configured paths are ones a user cannot create, so nothing is found in a folder the user can write:
#   --prefix=C:/Windows/System32/OmniGrammaton-no-prefix  (where OpenSSL looks for modules and engines)
#   --openssldir=C:/Windows/System32/OmniGrammaton-no-config  (openssl.cnf and the default CA files)
# and the program reads no configuration file (net/tls.c passes OPENSSL_INIT_NO_LOAD_CONFIG).
# x86_64 is OpenSSL's mingw64 target with its assembly; aarch64 is the target of
# plan/ws334/tests/openssl-conf (OpenSSL 3.5.8 has no mingw arm64 target), without assembly.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
arch=${1:-}
out=${2:-}
case "$arch" in
x86_64) target=mingw64; options="" ;;
aarch64) target=mingw-arm64; options="no-asm" ;;
*) echo "usage: openssl-windows.sh <x86_64|aarch64> <output directory>"; echo "openssl-windows: FAIL"; exit 2 ;;
esac
if [ -z "$out" ]; then
	echo "usage: openssl-windows.sh <x86_64|aarch64> <output directory>"
	echo "openssl-windows: FAIL"
	exit 2
fi
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
triple=$arch-w64-mingw32
PATH=$LLVM_MINGW/bin:$PATH
export PATH
fail() {
	echo "openssl-windows: $1"
	echo "openssl-windows: FAIL"
	exit 1
}
[ -x "$LLVM_MINGW/bin/$triple-clang" ] || fail "no $LLVM_MINGW/bin/$triple-clang"

# The release's identity, from the zedBSD package (one place for the version).
package=userland/packages/security/openssl/Makefile
version=$(sed -n 's/^ZEDBSD_EXT_openssl_VERSION := //p' "$package")
sha256=$(sed -n 's/^ZEDBSD_EXT_openssl_SHA256 := //p' "$package")
url=$(sed -n 's/^ZEDBSD_EXT_openssl_URL := //p' "$package")
[ -n "$version" ] && [ -n "$sha256" ] && [ -n "$url" ] || fail "cannot read the release from $package"
archive=openssl-$version.tar.gz

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$out/work
log=$out/openssl-build.log
mkdir -p "$work"
: > "$log"

# The tarball: the shared distfiles when it is there (read only), else fetched and checked.
sources=${OPENSSL_SOURCES:-}
if [ -z "$sources" ]; then
	if [ -f "build/distfiles/$archive" ]; then sources=build/distfiles; else sources=$out/sources; fi
fi
mkdir -p "$sources"
if [ ! -f "$sources/$archive" ]; then
	curl -fsSL --retry 3 --max-time 600 -o "$sources/$archive.part" "$url" || fail "cannot fetch $url"
	mv -f "$sources/$archive.part" "$sources/$archive"
fi
echo "$sha256  $sources/$archive" | sha256sum -c --quiet - || fail "$sources/$archive is not the release (SHA-256)"

# The source, extracted afresh into the work directory's own new directory.
src=$work/src
[ -d "$src" ] || { mkdir -p "$src" && tar -xzf "$sources/$archive" -C "$src" || fail "cannot extract $archive"; }
tree=$src/openssl-$version
[ -f "$tree/Configure" ] || fail "no $tree/Configure"

# The build tree (out of the source tree), afresh when the configuration is.
build=$work/build-$arch
if [ ! -f "$build/.configured" ]; then
	mkdir -p "$build"
	(cd "$build" && \
	 OPENSSL_LOCAL_CONFIG_DIR="$OLDPWD/plan/ws334/tests/openssl-conf" \
	 perl "$tree/Configure" "$target" $options \
		--cross-compile-prefix="$triple-" \
		--prefix=C:/Windows/System32/OmniGrammaton-no-prefix \
		--openssldir=C:/Windows/System32/OmniGrammaton-no-config \
		no-shared no-tests no-docs no-apps no-engine no-dso no-module threads \
		CC=clang AR=ar RANLIB=ranlib RC=windres) \
		>> "$log" 2>&1 || { tail -20 "$log"; fail "Configure failed (log $log)"; }
	touch "$build/.configured"
fi
timeout 3000 make -C "$build" -j"$(nproc)" build_libs >> "$log" 2>&1 || { tail -20 "$log"; fail "make failed (log $log)"; }
sync

# The headers (the source's, then the generated ones of the build tree over them) and the two libraries.
# make install_dev is not used: with a prefix of a Windows form it writes beside the staging directory.
mkdir -p "$out/lib" "$out/include/openssl"
cp "$tree"/include/openssl/*.h "$out/include/openssl/"
cp "$build"/include/openssl/*.h "$out/include/openssl/"
cp "$build/libssl.a" "$build/libcrypto.a" "$out/lib/"

# Checks: the machine of the objects, no shared library, the one name the program needs, the configured paths.
nm=$LLVM_MINGW/bin/llvm-nm
$nm --defined-only "$out/lib/libssl.a" 2> /dev/null | grep -q ' T SSL_CTX_new$' || fail "libssl.a does not define SSL_CTX_new"
$nm --defined-only "$out/lib/libcrypto.a" 2> /dev/null | grep -q ' T X509_STORE_add_cert$' || fail "libcrypto.a does not define X509_STORE_add_cert"
strings -a "$out/lib/libcrypto.a" | grep -q 'C:/Windows/System32/OmniGrammaton-no-config' || fail "libcrypto.a does not hold the configured openssldir"
case "$arch" in
x86_64) machine='file format coff-x86-64' ;;
aarch64) machine='file format coff-arm64' ;;
esac
"$LLVM_MINGW/bin/llvm-objdump" -f "$out/lib/libcrypto.a" 2> /dev/null | grep -q "$machine" || fail "libcrypto.a is not $machine"
sync
echo "openssl-windows: $arch libssl.a $(wc -c < "$out/lib/libssl.a") bytes, libcrypto.a $(wc -c < "$out/lib/libcrypto.a") bytes in $out/lib"
echo "openssl-windows: PASS"
