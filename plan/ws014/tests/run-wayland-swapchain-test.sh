#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercises production swapchain and shared-image code with independent boundaries.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
output=${1:-build/q309-wayland-swapchain}
mkdir -p "$output/include"
ln -sfn "$repo/include/libc/vulkan" "$output/include/vulkan"
ln -sfn "$repo/include/uapi" "$output/include/uapi"
flags='-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wdeclaration-after-statement -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=pthread_cond_clockwait'
sources='plan/ws014/tests/wayland-wsi-swapchain.c userland/base/libvulkan/objects.c userland/base/libvulkan/wsi.c userland/base/libvulkan/wsi-swapchain.c userland/base/libvulkan/wsi-image.c'
cc $flags -I"$output/include" -Iuserland/base/libvulkan $sources -o "$output/wayland-swapchain"
timeout 30 "$output/wayland-swapchain"
timeout 30 "$output/wayland-swapchain" shared-fence
timeout 30 "$output/wayland-swapchain" fallback
cc $flags -I"$output/include" -Iuserland/base/libvulkan \
 -fsanitize=address,undefined -fno-omit-frame-pointer -g \
 $sources -o "$output/wayland-swapchain-asan"
ASAN_OPTIONS=detect_leaks=1 timeout 30 "$output/wayland-swapchain-asan"
ASAN_OPTIONS=detect_leaks=1 timeout 30 "$output/wayland-swapchain-asan" shared-fence
ASAN_OPTIONS=detect_leaks=1 timeout 30 "$output/wayland-swapchain-asan" fallback
