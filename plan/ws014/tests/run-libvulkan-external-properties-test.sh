#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-external-properties.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c89 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections -fdata-sections $extra \
        -I"$work/include" -I"$repo/include" -I"$repo/userland/base/libvulkan" \
        "$repo/plan/ws014/tests/libvulkan-external-properties.c" \
        "$repo/userland/base/libvulkan/external-properties.c" \
        "$repo/userland/base/libvulkan/objects.c" "$repo/userland/base/libvulkan/wire.c" \
        -Wl,--gc-sections -pthread -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
done
