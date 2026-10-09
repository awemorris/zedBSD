#!/bin/sh
# Tests actual native Vulkan logical ownership, without a GPU or production test switch.
# Usage: sh plan/ws141/tests/vulkan-object-host-test.sh [output-directory]
set -eu
out=${1:-build/ws141-vulkan-object-host}
mkdir -p "$out"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -include time.h -Wall -Wextra -Werror \
    -Iplan/ws141/tests/host -Iinclude -Isrc \
    plan/ws141/tests/vulkan-object-host-test.c src/drivers/gpu/bcm2711/vulkan-object.c \
    src/drivers/gpu/bcm2711/vulkan-session.c \
    -o "$out/vulkan-object-host-test"
"$out/vulkan-object-host-test"
