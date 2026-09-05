#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-storage-test.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
common="-std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DZEDBSD_USER_ABI_LP64 -DZEDBSD_STORAGE_HOST_TEST -I$repo -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo/libc/include"
for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then
		# Unused exported filesystem vtables must remain eligible for GC;
		# this fixture instruments stack/heap/accesses, not ASan global roots.
		extra="-fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0"
	fi
	# shellcheck disable=SC2086
	${HOSTCC:-cc} $common $extra \
		"$repo/plan/ws019-installation/tests/storage-foundation-test.c" \
		"$repo/src/drivers/disklabel/mbr.c" \
		-Wl,--gc-sections -o "$temporary/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/$mode"
done
for arch in amd64 i386; do
	case $arch in amd64) triple=x86_64;; i386) triple=i386;; esac
	"$repo/build/llvm/bin/clang" --target="$triple-unknown-zedbsd" \
		--sysroot="$repo/build/$arch/sysroot" -nostdinc \
		-isystem "$repo/build/$arch/sysroot/usr/include" \
		-I"$repo/include/uapi" -x c -fsyntax-only \
		-include zedbsd/block.h -include zedbsd/mountinfo.h /dev/null
done
echo 'storage ABI amd64/i386: PASS'
