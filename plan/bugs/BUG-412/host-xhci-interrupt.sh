#!/bin/sh
# The host test of the xHCI interrupt endpoint context (include/drivers/pci/pci-xhci-control.h), plain and under ASan
# and UBSan.  usage: sh plan/bugs/BUG-412/host-xhci-interrupt.sh   (from the repository's top; OUT= the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug412-xhci-test}
mkdir -p "$OUT"
cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -I. -o "$OUT/plain" plan/bugs/BUG-412/host-xhci-interrupt.c
cc -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I. \
	-o "$OUT/sanitized" plan/bugs/BUG-412/host-xhci-interrupt.c
timeout 60 "$OUT/plain"
timeout 60 "$OUT/sanitized"
