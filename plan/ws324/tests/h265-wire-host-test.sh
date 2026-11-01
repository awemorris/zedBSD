#!/bin/sh
# Host test of the H.265 video wire round trip: libvulkan's own video encoders (userland/desktop/libvulkan/video.c)
# write an H.265 parameters create, a decode and profiles, and the nvrtx executor's readers (src/drivers/gpu/nvrtx/vk/
# vk-video.c) read them back, every byte and every field the decode keeps.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws324/tests/h265-wire-host-test.sh   (from anywhere)
# The objects and the binary go in a new directory below build/tmp; nothing is removed.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/ws324-h265-wire.XXXXXX")
mkdir -p "$out/client-headers"
ln -s "$repo/include/libc/vulkan" "$out/client-headers/vulkan"
src=$repo/src/drivers/gpu/nvrtx
flags="-std=gnu11 -O1 -g -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror \
-ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize=alignment \
-I$out/client-headers -I$repo/include -I$repo/src -I$repo"
# The libvulkan side (its static encoders), its wire and codec.
cc $flags -Wno-unused-function -c -o "$out/encode.o" "$repo/plan/ws324/tests/h265-wire-encode.c"
cc $flags -c -o "$out/lv-wire.o" "$repo/userland/desktop/libvulkan/wire.c"
cc $flags -c -o "$out/lv-codec.o" "$repo/userland/desktop/libvulkan/codec.c"
# The executor's side (its static readers) and what they call.
cc $flags -Wno-unused-function -c -o "$out/decode.o" "$repo/plan/ws324/tests/h265-wire-decode.c"
for file in vk/vk-object.c vk/vk-reply.c vk/vk-video-h264.c vk/vk-video-h265.c nvdec-records.c wire.c; do
	cc $flags -c -o "$out/$(echo "$file" | tr / -).o" "$src/$file"
done
cc $flags -c -o "$out/i915-codec.o" "$repo/src/drivers/gpu/i915/vk/vk-codec.c"
cc -fsanitize=address,undefined -Wl,--gc-sections -o "$out/h265-wire-host-test" "$out"/*.o
"$out/h265-wire-host-test"
