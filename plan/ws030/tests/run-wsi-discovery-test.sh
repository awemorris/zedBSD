#!/bin/sh
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wsi.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Use the actual Vulkan ABI headers with host libc and pthread implementations.
mkdir "$work/include"
ln -s "$repo/libc/include/vulkan" "$work/include/vulkan"
flags="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wdeclaration-after-statement -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections"
case "${1:-}" in
    "") ;;
    asan) flags="$flags -fsanitize=address -fno-omit-frame-pointer -g" ;;
    ubsan) flags="$flags -fsanitize=undefined -fno-sanitize-recover=all -g" ;;
    *) echo "usage: $0 [asan|ubsan]" >&2; exit 2 ;;
esac
# Intentional word splitting expands the finite compiler-option list above.
${CC:-cc} $flags -I"$work/include" -I"$repo/userland/base/libvulkan" \
    "$repo/plan/ws030/tests/wsi-discovery.c" \
    "$repo/userland/base/libvulkan/objects.c" \
    "$repo/userland/base/libvulkan/wsi.c" \
    "$repo/userland/base/libvulkan/wsi-swapchain.c" -o "$work/wsi-discovery"
timeout 30 "$work/wsi-discovery"

# Exercise the actual native adapter separately from the mock discovery backend.
${CC:-cc} $flags -I"$work/include" -I"$repo/include" \
    -I"$repo/userland/base/libvulkan" \
    "$repo/plan/ws030/tests/wsi-native.c" \
    "$repo/userland/base/libvulkan/objects.c" \
    "$repo/userland/base/libvulkan/wsi-display.c" -o "$work/wsi-native"
timeout 30 "$work/wsi-native"
