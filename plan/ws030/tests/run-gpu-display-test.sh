#!/bin/sh
# Run native display negative cases through the production GPU and cdev dispatchers.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-gpu-display.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=c11 -O1 -g -Wall -Wextra -Werror \
        -ffunction-sections -fdata-sections $extra \
        -DKERN_USER_ABI_LP64 -I"$root/include" -I"$root/src" \
        -idirafter "$root/libc/include" -include "$root/libc/include/sys/ioctl.h" \
        "$root/plan/ws030/tests/gpu-display.c" \
        "$root/src/drivers/gpu/gpu.c" "$root/src/kern/cdev.c" \
        "$root/src/kern/vm-device.c" "$root/src/drivers/pci/pci.c" \
        -Wl,--gc-sections -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
