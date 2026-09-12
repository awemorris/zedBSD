#!/bin/sh
# Verify original memory code against an independent native peer and real host mmap.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vulkan-memory.XXXXXX)
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
        -Dmmap=memory_test_mmap -Dmunmap=memory_test_munmap -Dioctl=memory_test_ioctl \
        -c "$root/userland/base/libvulkan/memory.c" -o "$work/memory.o"
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread $extra \
        -I"$work/include" -I"$root/include" -I"$root/userland/base/libvulkan" \
        "$root/plan/ws030/tests/libvulkan-memory.c" "$work/memory.o" \
        "$root/userland/base/libvulkan/objects.c" "$root/userland/base/libvulkan/wire.c" \
        -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
