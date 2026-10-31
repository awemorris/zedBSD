#!/bin/sh
# Host tests of the GPU core's jobs and supervision (the ws014 fixtures gpu-supervision.c, gpu-job.c and gpu-fence.c
# with the real gpu.c, cdev.c, handle and descriptor-table cores), plain and with ASan/UBSan.  Prints each fixture's
# PASS line or stops at the first failure.
#   sh plan/ws291/tests/run-gpu-job-host.sh
# The programs go in a new directory below build/tmp, which is kept (nothing is removed).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
work=$(mktemp -d "$repo/build/tmp/gpu-job.XXXXXX")
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    for fixture in gpu-supervision gpu-job gpu-fence; do
        # gpu-supervision.c includes gpu.c itself; the others link it.
        core="$repo/src/drivers/gpu/gpu.c"
        test "$fixture" = gpu-supervision && core=
        timeout 300 cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function $extra -ffunction-sections -fdata-sections \
            -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" -I"$repo/include/libc" -DKERN_UAPI_NATIVE \
            -include "$repo/include/libc/sys/ioctl.h" -include "$repo/include/hal/hal.h" \
            "$repo/plan/ws014/tests/$fixture.c" $core "$repo/src/kern/cdev.c" "$repo/plan/ws014/tests/gpu-test-fd.c" \
            "$repo/src/kern/handle.c" "$repo/src/drivers/gpu/gpu-fence.c" "$repo/src/kern/fd-object.c" \
            "$repo/src/kern/filedesc.c" "$repo/src/kern/vm/device.c" "$repo/src/drivers/pci/pci.c" \
            -Wl,--gc-sections -o "$work/$fixture-$mode"
        echo "== $fixture $mode"
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 120 "$work/$fixture-$mode"
    done
done
