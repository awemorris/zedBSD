#!/bin/sh
# The host test of usb-hid's vendor-only decision (drv_hidraw_describe's vendor_only), plain and under ASan and UBSan.
# usage: sh plan/bugs/BUG-412/host-vendor-drain.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug412-vendor-drain}
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror -O1 -Iinclude -I. -o "$OUT/plain" plan/bugs/BUG-412/host-vendor-drain.c src/drivers/generic/hidraw-describe.c
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I. \
	-o "$OUT/sanitized" plan/bugs/BUG-412/host-vendor-drain.c src/drivers/generic/hidraw-describe.c
timeout 60 "$OUT/plain"
timeout 60 "$OUT/sanitized"
