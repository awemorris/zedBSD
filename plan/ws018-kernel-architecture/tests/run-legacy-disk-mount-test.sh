#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$repo/plan/ws018-kernel-architecture/temp"
temporary=$(mktemp -d "$repo/plan/ws018-kernel-architecture/temp/mount-regression.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
result=0
for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then
		extra="-fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0"
	fi
	# pthread must see host headers; the production objects use kernel ABI headers.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -pthread $extra \
		-c "$repo/plan/ws018-kernel-architecture/tests/mount-thread-host.c" \
		-o "$temporary/thread-$mode.o"
	# Disable only mount's hightext grouping so unused callbacks can be GC'd.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror \
		-ffunction-sections -fdata-sections -DZEDBSD_USER_ABI_LP64 \
		-DZEDBSD_STORAGE_HOST_TEST $extra -I"$repo/include" \
		-I"$repo/include/uapi" -I"$repo/src" -I"$repo/libc/include" \
		"$repo/plan/ws018-kernel-architecture/tests/legacy-disk-mount-test.c" \
		"$repo/src/kern/mount.c" "$repo/src/kern/inode.c" \
		"$repo/src/kern/namei.c" "$repo/src/kern/namecache.c" \
		"$repo/src/kern/cwdinfo.c" "$temporary/thread-$mode.o" \
		-pthread -Wl,--gc-sections -o "$temporary/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		timeout 45s "$temporary/$mode"
	if ! ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		timeout 45s "$temporary/$mode" --covered-directory; then
		result=1
	fi
done
exit "$result"
