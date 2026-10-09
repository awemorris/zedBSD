#!/bin/sh
# Uses actual client wire.c and native transport; descriptor backing is a host fixture, no GPU proof.
# Usage: sh plan/ws141/tests/vulkan-stream-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-vulkan-stream-host}
task_root=$(pwd)
mkdir -p "$out/client-headers"
# Only the actual pinned Vulkan header directory is exposed; host libc/pthread headers remain native.
if [ ! -e "$out/client-headers/vulkan" ]; then
    ln -s "$task_root/include/libc/vulkan" "$out/client-headers/vulkan"
fi
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Iplan/ws141/tests/host -I"$out/client-headers" -Iinclude -Isrc -I. \
    plan/ws141/tests/vulkan-stream-host-test.c src/drivers/gpu/bcm2711/vulkan-stream.c \
    src/drivers/gpu/i915/render/codec.c userland/desktop/libvulkan/wire.c \
    -Wl,--gc-sections -o "$out/vulkan-stream-host-test"
"$out/vulkan-stream-host-test"
