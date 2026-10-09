#!/bin/sh
# Native MMU/cache/IRQ ownership and reset failures, no physical MMIO or QEMU.
# Usage: sh plan/ws141/tests/v3d-hardware-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-v3d-hardware-host}
mkdir -p "$out"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror \
    -Iinclude -Isrc -I. plan/ws141/tests/v3d-hardware-host-test.c \
    plan/ws141/tests/display-lock-host.c src/drivers/gpu/bcm2711/v3d-hardware.c \
    src/drivers/gpu/bcm2711/mmu.c src/drivers/gpu/bcm2711/v3d-job.c \
    src/drivers/gpu/bcm2711/v3d-diagnostic.c src/drivers/gpu/bcm2711/cl.c \
    src/drivers/gpu/bcm2711/v3d-memory.c src/drivers/gpu/bcm2711/render-device.c \
    src/drivers/gpu/bcm2711/share.c \
    -o "$out/v3d-hardware-host-test"
"$out/v3d-hardware-host-test"
