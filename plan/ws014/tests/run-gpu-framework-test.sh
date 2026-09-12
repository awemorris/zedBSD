#!/bin/sh
# Compile real GPU, cdev and PCI cores against a bounded host fixture.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws014-gpu-framework.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

# Compile the same public layout against each userspace word size.
cc -std=c89 -m32 -ffreestanding -Wall -Wextra -Werror \
    -DKERN_USER_ABI_ILP32 -I"$repo/include" -I"$repo/libc/include" \
    -c "$repo/plan/ws014/tests/gpu-uapi-layout.c" -o "$work/layout32.o"
cc -std=c89 -m64 -ffreestanding -Wall -Wextra -Werror \
    -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/libc/include" \
    -c "$repo/plan/ws014/tests/gpu-uapi-layout.c" -o "$work/layout64.o"
echo "GPU UAPI: ILP32/LP64 sizes, offsets and ioctl encoding PASS"

# Force the zedBSD ioctl encoding before any host ABI declaration is included.
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections \
    -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" \
    -idirafter "$repo/libc/include" \
    -include "$repo/libc/include/sys/ioctl.h" \
    "$repo/plan/ws014/tests/gpu-framework.c" \
    "$repo/src/drivers/gpu/gpu.c" "$repo/src/kern/cdev.c" \
    "$repo/src/kern/vm-device.c" \
    "$repo/src/drivers/pci/pci.c" -Wl,--gc-sections \
    -o "$work/ordinary"
"$work/ordinary"

cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/src" \
    -idirafter "$repo/libc/include" \
    -include "$repo/libc/include/sys/ioctl.h" \
    "$repo/plan/ws014/tests/gpu-framework.c" \
    "$repo/src/drivers/gpu/gpu.c" "$repo/src/kern/cdev.c" \
    "$repo/src/kern/vm-device.c" \
    "$repo/src/drivers/pci/pci.c" -Wl,--gc-sections \
    -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"
