#!/bin/sh
# Host test of the GPU core's image layouts (GPU_IMAGE_LAYOUTS, layout export and import): the real gpu.c, cdev.c,
# handle and descriptor-table cores with the ws014 sharing fixture, plain and with ASan/UBSan, after the UAPI layout
# checks for ILP32 and LP64.  Prints "gpu-layouts-host: PASS" per build or stops at the first failure.
#   sh plan/ws291/tests/run-gpu-layouts-host.sh
# The programs go in a new directory below build/tmp, which is kept (nothing is removed).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
work=$(mktemp -d "$repo/build/tmp/gpu-layouts.XXXXXX")
for abi in 32 64; do
    timeout 60 cc -std=c89 -m$abi -ffreestanding -Wall -Wextra -Werror -I"$repo/include" -I"$repo/include/libc" \
        -DKERN_UAPI_NATIVE -c "$repo/plan/ws014/tests/gpu-uapi-layout.c" -o "$work/layout$abi.o"
done
echo "GPU UAPI: ILP32/LP64 sizes, offsets and ioctl encoding PASS"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    timeout 300 cc -std=c11 -O1 -g -Wall -Wextra -Werror $extra -ffunction-sections -fdata-sections \
        -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" -I"$repo/include/libc" -DKERN_UAPI_NATIVE \
        -include "$repo/include/libc/sys/ioctl.h" -include "$repo/include/hal/hal.h" \
        "$repo/plan/ws291/tests/gpu-layouts-host.c" \
        "$repo/src/drivers/gpu/gpu.c" "$repo/src/kern/cdev.c" "$repo/plan/ws014/tests/gpu-test-fd.c" \
        "$repo/src/kern/handle.c" "$repo/src/drivers/gpu/gpu-fence.c" "$repo/src/kern/fd-object.c" \
        "$repo/src/kern/filedesc.c" "$repo/src/kern/vm/device.c" "$repo/src/drivers/pci/pci.c" \
        -Wl,--gc-sections -o "$work/$mode"
    echo "== $mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$work/$mode"
done
