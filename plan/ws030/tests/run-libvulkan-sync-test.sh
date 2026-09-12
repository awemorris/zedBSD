#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-libvulkan-sync.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/include"
ln -s "$repo/libc/include/vulkan" "$work/include/vulkan"

cc -std=c89 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
    -I"$work/include" -I"$repo/userland/base/libvulkan" \
    "$repo/userland/base/libvulkan/objects.c" \
    "$repo/userland/base/libvulkan/wire.c" \
    "$repo/userland/base/libvulkan/sync.c" \
    "$repo/userland/base/libvulkan/queue.c" \
    "$repo/userland/base/libvulkan/query.c" \
    "$repo/plan/ws030/tests/libvulkan-sync.c" \
    -pthread -o "$work/sync-test"
timeout 20 "$work/sync-test"

cc -std=c89 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$work/include" -I"$repo/userland/base/libvulkan" \
    "$repo/userland/base/libvulkan/objects.c" \
    "$repo/userland/base/libvulkan/wire.c" \
    "$repo/userland/base/libvulkan/sync.c" \
    "$repo/userland/base/libvulkan/queue.c" \
    "$repo/userland/base/libvulkan/query.c" \
    "$repo/plan/ws030/tests/libvulkan-sync.c" \
    -pthread -o "$work/sync-sanitized"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$work/sync-sanitized"
