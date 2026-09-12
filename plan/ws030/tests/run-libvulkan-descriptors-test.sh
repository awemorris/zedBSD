#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-libvulkan-descriptors.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/include"
ln -s "$repo/libc/include/vulkan" "$work/include/vulkan"

for mode in ordinary sanitize; do
    extra=
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -g'
    fi
    cc -std=c89 -Wall -Wextra -Werror -D_GNU_SOURCE $extra \
        -I"$work/include" -I"$repo/userland/base/libvulkan" \
        "$repo/userland/base/libvulkan/objects.c" \
        "$repo/userland/base/libvulkan/wire.c" \
        "$repo/userland/base/libvulkan/codec.c" \
        "$repo/userland/base/libvulkan/descriptors.c" \
        "$repo/plan/ws030/tests/libvulkan-descriptors.c" \
        -pthread -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode"
    printf '%s: PASS\n' "$mode"
done
