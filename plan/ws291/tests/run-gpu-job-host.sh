#!/bin/sh
# Host tests of the GPU core (the ws014 fixtures: framework, fences, jobs, supervision, placement, scanout, sharing,
# and ws291's gpu-job-submit.c for GPU_JOB_SUBMIT and GPU_JOB_RETIRE, each with the real gpu.c, cdev.c, handle and
# descriptor-table cores), plain and with ASan/UBSan.  Prints each
# fixture's PASS lines or stops at the first failure.
#   sh plan/ws291/tests/run-gpu-job-host.sh
# The programs go in a new directory below build/tmp, which is kept (nothing is removed).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
work=$(mktemp -d "$repo/build/tmp/gpu-core.XXXXXX")
cores="$repo/src/kern/cdev.c $repo/src/kern/handle.c $repo/src/drivers/gpu/gpu-fence.c $repo/src/kern/fd-object.c \
$repo/src/kern/filedesc.c $repo/src/kern/vm/device.c $repo/src/drivers/pci/pci.c"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    for fixture in gpu-framework gpu-fence gpu-fence-close gpu-fence-reuse gpu-fence-payload gpu-job gpu-supervision \
        gpu-placement gpu-scanout gpu-sharing gpu-job-submit; do
        # gpu-supervision.c includes gpu.c itself; gpu-fence-payload.c brings its own kernel peers and the socket core,
        # and only links plain (the sanitizer keeps references the plain build's section collection drops).
        test "$fixture-$mode" = gpu-fence-payload-sanitize && continue
        sources="$repo/src/drivers/gpu/gpu.c $repo/plan/ws014/tests/gpu-test-fd.c $cores"
        file="$repo/plan/ws014/tests/$fixture.c"
        test "$fixture" = gpu-job-submit && file="$repo/plan/ws291/tests/$fixture.c"
        case $fixture in
        gpu-supervision) sources="$repo/plan/ws014/tests/gpu-test-fd.c $cores" ;;
        gpu-fence-payload) sources="$repo/src/drivers/gpu/gpu.c $cores $repo/src/kern/poll.c $repo/src/kern/net/socket.c \
$repo/src/kern/net/unix-socket.c $repo/src/kern/net/packet-buf.c" ;;
        esac
        timeout 300 cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function $extra -ffunction-sections -fdata-sections \
            -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" -I"$repo/include/libc" -DKERN_UAPI_NATIVE \
            -include "$repo/include/libc/sys/ioctl.h" -include "$repo/include/hal/hal.h" \
            "$file" $sources -Wl,--gc-sections -o "$work/$fixture-$mode"
        echo "== $fixture $mode"
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 120 "$work/$fixture-$mode"
    done
done
echo "gpu-core-host: PASS"
