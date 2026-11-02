#!/bin/sh
# The host test of the USB HID report ring (src/drivers/usb/usb-hid-ring.c), plain and under ASan and UBSan.
# usage: sh plan/bugs/BUG-412/host-ring.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug412-ring}
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror -O1 -Iinclude -I. -o "$OUT/plain" plan/bugs/BUG-412/host-ring.c src/drivers/usb/usb-hid-ring.c
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I. \
	-o "$OUT/sanitized" plan/bugs/BUG-412/host-ring.c src/drivers/usb/usb-hid-ring.c
timeout 60 "$OUT/plain"
timeout 60 "$OUT/sanitized"
