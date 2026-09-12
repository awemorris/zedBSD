#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Link every actual implementation family; no Vulkan entry point or transport is mocked.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-libvulkan-dispatch.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/include"
ln -s "$repo/libc/include/vulkan" "$work/include/vulkan"
for mode in ordinary sanitize; do
    extra=
    executable_extra=
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
        executable_extra=-no-pie
    fi
    set --
    for source in objects wire context codec dispatch instance device resources pipeline descriptors commands memory sync queue query wsi wsi-display wsi-swapchain; do
        set -- "$@" "$repo/userland/base/libvulkan/$source.c"
    done
    timeout 60 cc -std=c89 -D_GNU_SOURCE -Wall -Wextra -Werror $extra \
        -I"$work/include" -I"$repo/include" -fPIC -shared "$@" \
        -Wl,-z,defs -Wl,-soname,libvulkan-dispatch.so -pthread \
        -o "$work/libvulkan-dispatch.so"
    timeout 30 cc -std=c89 -D_GNU_SOURCE -Wall -Wextra -Werror $extra $executable_extra \
        -I"$work/include" -I"$repo/include" -I"$repo/userland/base/libvulkan" \
        "$repo/plan/ws030/tests/libvulkan-dispatch.c" \
        -L"$work" -Wl,-rpath,"$work" -lvulkan-dispatch -ldl -pthread -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 20 "$work/$mode" "$work/libvulkan-dispatch.so" \
        "$repo/userland/base/libvulkan/api-commands.tsv"
    printf '%s: PASS\n' "$mode"
done
