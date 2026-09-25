#!/bin/sh
# ws035-p001: private toolchain for the pre-refactor baseline builds.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# This checkout has no build/ tree.  The project LLVM (23.1.0-zedbsd6) and the
# host Noct interpreter are copied from a completed tree (default ~/zedBSD/build)
# into build/ws035-p001/toolchain/ so the baseline never writes into that tree.
# Only the parts a kernel/rootfs build reads are copied; every stamp is then
# touched in its dependency order so make accepts the copy without rebuilding.
#
# usage: sh plan/ws035/tests/baseline-toolchain.sh [source-build-dir]
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
src=${1:-$HOME/zedBSD/build}
srctree=$(cd "$src/.." && pwd)
dst=$repo/build/ws035-p001/toolchain
llvm_patch=zedbsd6
noct_patch=zedbsd12

test -f "$src/llvm/.zedbsd-install-identity"
grep -qx "version=23.1.0 patch=$llvm_patch" "$src/llvm/.zedbsd-install-identity"
mkdir -p "$dst"

# LLVM install (accepted by its identity file; no rebuild rule runs).
if ! test -d "$dst/llvm"; then
	cp -a "$src/llvm" "$dst/llvm.tmp"
	mv "$dst/llvm.tmp" "$dst/llvm"
fi

# LLVM distfile: a hard link (same file system, never written) plus a fresh
# verification record, so the order-only archive check is satisfied.
mkdir -p "$dst/llvm-distfiles"
archive=llvm-project-23.1.0.src.tar.xz
test -f "$dst/llvm-distfiles/$archive" ||
	ln "$srctree/toolchain/llvm/distfiles/$archive" "$dst/llvm-distfiles/$archive"

# LLVM source: only compiler-rt builtins are read (sysroot.mk).
mkdir -p "$dst/llvm-source/compiler-rt/lib"
if ! test -d "$dst/llvm-source/compiler-rt/lib/builtins"; then
	cp -a "$src/llvm-source/compiler-rt/lib/builtins" \
		"$dst/llvm-source/compiler-rt/lib/builtins"
fi
cp "$src/llvm-source/LICENSE.TXT" "$dst/llvm-source/LICENSE.TXT"
cp "$src/llvm-source/.zedbsd-source-identity" "$dst/llvm-source/"

# Host Noct interpreter.
if ! test -d "$dst/NoctLang"; then
	cp -a "$src/NoctLang" "$dst/NoctLang.tmp"
	mv "$dst/NoctLang.tmp" "$dst/NoctLang"
fi
mkdir -p "$dst/noct-distfiles" "$dst/host-noct-state"
for f in "$srctree"/userland/base/noct/distfiles/NoctLang-*.tar.gz; do
	test -f "$dst/noct-distfiles/${f##*/}" || ln "$f" "$dst/noct-distfiles/${f##*/}"
done

# Stamps, oldest first (each later one depends on the earlier ones).
touch "$dst/llvm-distfiles/.zedbsd-archive-verified-$archive"
sleep 1
touch "$dst/llvm-source/.zedbsd-source-23.1.0-$llvm_patch"
sleep 1
touch "$dst/llvm-source/.zedbsd-source-verified-23.1.0-$llvm_patch"
touch "$dst/llvm/.zedbsd-install-23.1.0-$llvm_patch"
for f in "$dst"/noct-distfiles/NoctLang-*.tar.gz; do
	touch "$dst/noct-distfiles/.zedbsd-archive-verified-${f##*/}"
done
sleep 1
touch "$dst/NoctLang/.zedbsd-source-2.0.1-$noct_patch"
sleep 1
touch "$dst/NoctLang/.zedbsd-source-verified-2.0.1-$noct_patch"
sleep 1
touch "$dst/NoctLang/build-static/noct"
sleep 1
touch "$dst/host-noct-state/built-2.0.1-$noct_patch-process"
echo "toolchain ready: $dst"
