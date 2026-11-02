#!/bin/sh
# Builds a zedBSD target of this worktree (default build/amd64/dynamic/libwebp-compat.so) against a copy of the main
# checkout's amd64 sysroot with this tree's include/libc/compat/webp headers added, so a new compat header needs no
# rebuild of the shared sysroot (a toolchain step that is Q1's).  The copy is build/amd64/sysroot of this worktree
# (made once; the headers are refreshed on every run).  Prints the make output to build/tmp/ws344-target.log and, last,
#   target-build: PASS (<target>, 0 warnings) or target-build: FAIL (...)
#   sh plan/ws344/tests/target-build.sh [<target>...]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
main=/home/awe/zedBSD-claude1
shared=$main/build/amd64/sysroot
sysroot=$(pwd)/build/amd64/sysroot
targets="$*"
[ -n "$targets" ] || targets=build/amd64/dynamic/libwebp-compat.so
mkdir -p build/tmp build/amd64
log=build/tmp/ws344-target.log

# The copy must be this worktree's own: never the main checkout's shared sysroot through a link.
case "$(realpath -m "$sysroot")" in
/home/awe/zedBSD-claude1/*)
	echo "target-build: FAIL ($sysroot resolves into the main checkout; use a worktree with its own build/amd64)"
	exit 1 ;;
esac

# The sysroot copy, once; then this tree's compat headers over it.
if [ ! -f "$sysroot/.zedbsd-sysroot-complete" ]; then
	if [ -e "$sysroot" ]; then
		echo "target-build: FAIL ($sysroot exists without its stamp; ask Q1 to remove it)"
		exit 1
	fi
	cp -a "$shared" "$sysroot" || { echo "target-build: FAIL (copying $shared)"; exit 1; }
fi
mkdir -p "$sysroot/usr/include/compat/webp"
cp include/libc/compat/webp/*.h "$sysroot/usr/include/compat/webp/"

# The targets, with the shared compiler and the copied sysroot.
status=0
timeout 3600 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=$main/build/llvm/bin \
	ZEDBSD_SYSROOT_AMD64="$sysroot" -o "$sysroot/.zedbsd-sysroot-complete" -j8 $targets > "$log" 2>&1 || status=1
warnings=$(grep -c -i 'warning:' "$log")
sync
if [ "$status" = 0 ] && [ "$warnings" = 0 ]; then
	echo "target-build: PASS ($targets, 0 warnings)"
	exit 0
fi
grep -i -E 'error|warning:' "$log" | head -20
echo "target-build: FAIL (status $status, $warnings warnings; log $log)"
exit 1
