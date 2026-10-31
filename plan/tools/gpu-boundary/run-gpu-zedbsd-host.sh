#!/bin/sh
# ws103-p004: host test of libkeiland-backend's check of a GPU buffer's description (userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.c),
# plain and with ASan/UBSan.  Prints each case and "gpu-zedbsd-host: PASS" or FAIL.
#   sh plan/tools/gpu-boundary/run-gpu-zedbsd-host.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
# The work directory stays in build/tmp (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh removes it).
mkdir -p build/tmp
work=$(mktemp -d "$(pwd)/build/tmp/gpu-zedbsd-host.XXXXXX")
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror $extra \
        -I"$work/include" -I"$repo/include" -I"$repo" \
        "$repo/plan/tools/gpu-boundary/gpu-zedbsd-host.c" "$repo/userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.c" \
        -o "$work/$mode"
    echo "== $mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
