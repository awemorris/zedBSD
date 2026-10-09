#!/bin/sh
# Actual client wire/record codec and native Vulkan ownership/query modules, no physical GPU or QEMU.
# Usage: sh plan/ws141/tests/vulkan-device-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-vulkan-device-host}
task_root=$(pwd)
mkdir -p "$out/client-headers"
if [ ! -e "$out/client-headers/vulkan" ]; then
    ln -s "$task_root/include/libc/vulkan" "$out/client-headers/vulkan"
fi
cc -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -include time.h -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Iplan/ws141/tests/host -I"$out/client-headers" -Iinclude -Isrc -I. \
    plan/ws141/tests/vulkan-device-host-test.c plan/ws141/tests/display-lock-host.c \
    src/drivers/gpu/bcm2711/vulkan-object.c src/drivers/gpu/bcm2711/vulkan-session.c \
    src/drivers/gpu/bcm2711/vulkan-stream.c src/drivers/gpu/bcm2711/vulkan-device.c \
    src/drivers/gpu/bcm2711/vulkan-query.c src/drivers/gpu/bcm2711/vulkan-memory.c src/drivers/gpu/bcm2711/vulkan-resource.c src/drivers/gpu/bcm2711/vulkan-input.c \
    src/drivers/gpu/bcm2711/v3d-memory.c src/drivers/gpu/bcm2711/mmu.c src/drivers/gpu/i915/render/codec.c \
    userland/desktop/libvulkan/wire.c userland/desktop/libvulkan/codec.c userland/desktop/libvulkan/objects.c \
    -Wl,--gc-sections -o "$out/vulkan-device-host-test"
"$out/vulkan-device-host-test"
