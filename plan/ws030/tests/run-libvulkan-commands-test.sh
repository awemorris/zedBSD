#!/bin/sh
# Compile actual Vulkan recording/pool code against independent byte and lifetime fixtures.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vulkan-commands.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$root/libc/include/vulkan" "$work/include/vulkan"
cc=${CC:-cc}
for mode in normal sanitized; do
    sanitize=
    if test "$mode" = sanitized; then
        sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    # These optional flags are fixed by the test and never contain caller shell code.
    "$cc" -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
        -pthread $sanitize -I"$work/include" -I"$root/include" \
        -I"$root/userland/base/libvulkan" \
        "$root/userland/base/libvulkan/objects.c" \
        "$root/userland/base/libvulkan/wire.c" \
        "$root/userland/base/libvulkan/codec.c" \
        "$root/userland/base/libvulkan/commands.c" \
        "$root/plan/ws030/tests/libvulkan-commands.c" -o "$work/commands-$mode"
    ASAN_OPTIONS=detect_leaks=1 timeout 30 "$work/commands-$mode"
done
