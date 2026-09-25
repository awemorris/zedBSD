#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Links the actual Wayland WSI and client against an independent socket peer.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
output=${1:-build/q309-wayland-wsi}
mkdir -p "$output/include"
ln -sfn "$repo/include/libc/vulkan" "$output/include/vulkan"
ln -sfn "$repo/include/libc/wayland" "$output/include/wayland"
ln -sfn "$repo/include/uapi" "$output/include/uapi"
sources='userland/base/libwayland/client.c userland/base/libwayland/proxy.c userland/base/libwayland/wire.c userland/base/libwayland/event.c userland/base/libwayland/protocol.c userland/base/libwayland/utility.c userland/base/libvulkan/objects.c'
flags="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -Wno-cast-function-type -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections"
cc $flags -I"$output/include" -Iinclude/libc/wayland -Iuserland/base/libvulkan \
 $sources plan/ws014/tests/wayland-wsi.c -o "$output/wayland-wsi"
timeout 30 "$output/wayland-wsi"
cc $flags -I"$output/include" -Iinclude/libc/wayland -Iuserland/base/libvulkan \
 -fsanitize=address,undefined -fno-omit-frame-pointer -g \
 $sources plan/ws014/tests/wayland-wsi.c -o "$output/wayland-wsi-asan"
ASAN_OPTIONS=detect_leaks=1 timeout 30 "$output/wayland-wsi-asan"
