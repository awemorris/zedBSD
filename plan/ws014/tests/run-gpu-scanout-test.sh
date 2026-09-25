#!/bin/sh
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercise real GPU/fd/fence cores with independent renderer and display-only peers.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-gpu-scanout.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections $extra \
        -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" -I"$repo/include/libc" -DKERN_UAPI_NATIVE \
        "$repo/plan/ws014/tests/gpu-scanout.c" "$repo/plan/ws014/tests/gpu-test-fd.c" \
        "$repo/src/drivers/gpu/gpu.c" "$repo/src/kern/cdev.c" \
        "$repo/src/drivers/gpu/gpu-fence.c" "$repo/src/kern/handle.c" \
        "$repo/src/kern/fd-object.c" "$repo/src/kern/filedesc.c" \
        "$repo/src/kern/vm-device.c" "$repo/src/drivers/pci/pci.c" \
        -Wl,--gc-sections -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
