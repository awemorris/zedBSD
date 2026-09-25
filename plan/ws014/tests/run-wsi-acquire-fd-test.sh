#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
output=${1:-/tmp/q312-wsi-acquire-fd}
mkdir -p "$output/include"
ln -sfn "$repo/include/libc/vulkan" "$output/include/vulkan"
flags='-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wdeclaration-after-statement -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=pthread_cond_clockwait -Wl,--wrap=ppoll -Wl,--wrap=pipe2'
cd "$repo"
sources='plan/ws014/tests/wsi-acquire-fd.c userland/base/libvulkan/objects.c userland/base/libvulkan/wsi.c userland/base/libvulkan/wsi-swapchain.c userland/base/libvulkan/wsi-image.c'
cc $flags -I"$output/include" -Iinclude -Iuserland/base/libvulkan $sources -o "$output/test"
timeout 20 "$output/test"
cc $flags -I"$output/include" -Iinclude -Iuserland/base/libvulkan -fsanitize=address,undefined -fno-omit-frame-pointer -g $sources -o "$output/test-asan"
ASAN_OPTIONS=detect_leaks=1 timeout 20 "$output/test-asan"
