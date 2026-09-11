#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-native-ufs.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then extra="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
    ${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
        -Wdeclaration-after-statement $extra -I"$repo" \
        "$repo/plan/ws019/tests/native-ufs-codec-test.c" \
        "$repo/userland/base/mkfs/ufs-super.c" "$repo/userland/base/mkfs/ufs-endian.c" \
        -o "$temporary/test"
    for size in 4194304 4294967296; do
        image="$temporary/$mode-$size.img"
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/test" "$image" "$size"
        python3 "$repo/plan/ws019/tests/native-ufs-inspect.py" "$image"
    done
done
