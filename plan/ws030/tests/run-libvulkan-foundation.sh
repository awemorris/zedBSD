#!/bin/sh
# Compile a focused host test against real library primitives and standard host libc.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vulkan-foundation.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir "$work/include"
ln -s "$root/libc/include/vulkan" "$work/include/vulkan"
cc=${CC:-cc}
"$cc" -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -pthread -I"$work/include" -I"$root/userland/base/libvulkan" \
    "$root/userland/base/libvulkan/objects.c" \
    "$root/userland/base/libvulkan/wire.c" \
    "$root/plan/ws030/tests/libvulkan-foundation.c" -o "$work/foundation"
timeout 30 "$work/foundation"
"$cc" -std=c89 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$work/include" -I"$root/userland/base/libvulkan" \
    "$root/userland/base/libvulkan/objects.c" \
    "$root/userland/base/libvulkan/wire.c" \
    "$root/plan/ws030/tests/libvulkan-foundation.c" -o "$work/foundation-sanitized"
ASAN_OPTIONS=detect_leaks=1 timeout 30 "$work/foundation-sanitized"
