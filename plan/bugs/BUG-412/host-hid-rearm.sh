#!/bin/sh
# The host test of the USB HID worker's re-arm before publication (src/drivers/usb/usb-hid.c), plain and under ASan
# and UBSan.  usage: sh plan/bugs/BUG-412/host-hid-rearm.sh   (from the repository's top; OUT= the build folder)
# The test includes usb-hid.c whole and stands in for the calls its worker makes; the link ignores the rest.  Warnings are
# off (-w): the kernel and host headers define the same limits; the kernel build checks the file with -Werror.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug412-rearm-test}
mkdir -p "$OUT"
F="-std=gnu11 -w -DKERN_UAPI_NATIVE -Iinclude -Isrc -I. -no-pie -Wl,--unresolved-symbols=ignore-all"
# shellcheck disable=SC2086
cc $F -O1 -o "$OUT/plain" plan/bugs/BUG-412/host-hid-rearm.c
# shellcheck disable=SC2086
cc $F -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/sanitized" plan/bugs/BUG-412/host-hid-rearm.c
timeout 60 "$OUT/plain"
timeout 60 "$OUT/sanitized"
