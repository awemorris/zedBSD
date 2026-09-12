#!/bin/sh
# Exercise the actual pipeline implementation, common object registry and typed codec.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vulkan-pipeline.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$root/libc/include/vulkan" "$work/include/vulkan"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wdeclaration-after-statement \
        -pthread $extra -I"$work/include" -I"$root/include" \
        -I"$root/userland/base/libvulkan" \
        "$root/plan/ws030/tests/libvulkan-pipeline.c" \
        "$root/userland/base/libvulkan/pipeline.c" \
        "$root/userland/base/libvulkan/objects.c" \
        "$root/userland/base/libvulkan/wire.c" \
        "$root/userland/base/libvulkan/codec.c" \
        -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
