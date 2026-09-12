#!/bin/sh
# Compile real context and wire code against a bounded fake GPU-kernel peer.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vulkan-context.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$root/libc/include/vulkan" "$work/include/vulkan"
cc=${CC:-cc}
for mode in normal sanitized; do
    sanitize=
    if test "$mode" = sanitized; then
        sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    # The optional flags above contain only this script's fixed compiler arguments.
    "$cc" -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
        -pthread $sanitize -I"$work/include" -I"$root/include" \
        -Dopen=vulkan_test_open -Dclose=vulkan_test_close -Dioctl=vulkan_test_ioctl \
        -Dclock_gettime=vulkan_test_clock_gettime -Dnanosleep=vulkan_test_nanosleep \
        -c "$root/userland/base/libvulkan/context.c" -o "$work/context.o"
    "$cc" -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
        -pthread $sanitize -I"$work/include" -I"$root/include" \
        -I"$root/userland/base/libvulkan" "$work/context.o" \
        "$root/userland/base/libvulkan/wire.c" \
        "$root/userland/base/libvulkan/objects.c" \
        "$root/plan/ws030/tests/libvulkan-context.c" -o "$work/context-$mode"
    ASAN_OPTIONS=detect_leaks=1 timeout 30 "$work/context-$mode"
done
