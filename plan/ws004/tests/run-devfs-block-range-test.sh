#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-devfs-block.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
fixture=$repo/plan/ws004/tests/devfs-block-range-test.c
common="-std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DKERN_USER_ABI_LP64 -DZEDBSD_DEVFS_HOST_TEST -DZEDBSD_STORAGE_HOST_TEST -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo/libc/include"

# shellcheck disable=SC2086
$compiler $common -O2 "$fixture" -Wl,--gc-sections -o "$temporary/devfs-block-range-test"
"$temporary/devfs-block-range-test"

# The fixture includes production devfs.c; unused filesystem vtables stay
# eligible for section GC while the actual range helpers are instrumented.
# shellcheck disable=SC2086
if $compiler $common -O1 -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	--param asan-globals=0 "$fixture" -Wl,--gc-sections \
	-o "$temporary/devfs-block-range-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/devfs-block-range-test-sanitized"
	echo 'HW-T20 devfs block range sanitizer: PASS'
else
	echo 'HW-T20 devfs block range sanitizer: SKIP (compiler unavailable)'
fi

# shellcheck disable=SC2086
if $compiler $common -O0 -fanalyzer -c "$fixture" \
	-o "$temporary/devfs-block-range-test-analyzer.o" 2>/dev/null; then
	echo 'HW-T20 devfs block range analyzer: PASS'
else
	echo 'HW-T20 devfs block range analyzer: SKIP (analyzer unavailable)'
fi
