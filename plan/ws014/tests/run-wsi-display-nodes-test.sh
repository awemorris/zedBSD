#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercise real node pairing with a finite independent device inventory.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-wsi-nodes.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
        -Wdeclaration-after-statement -pthread -ffunction-sections -fdata-sections $extra \
        -I"$work/include" -I"$repo/include" -I"$repo/userland/base/libvulkan" \
        "$repo/plan/ws014/tests/wsi-display-nodes.c" \
        "$repo/userland/base/libvulkan/wsi-display-nodes.c" \
        "$repo/userland/base/libvulkan/objects.c" -Wl,--gc-sections -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
