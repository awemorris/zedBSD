#!/bin/sh
# The host test of the xHCI Normal TD with a zero-length packet (include/drivers/pci/pci-xhci-normal.h), plain and
# under ASan and UBSan.  usage: sh plan/bugs/BUG-416/host-xhci-zero-packet.sh   (OUT= the build folder, a new one)
# The last line is xhci-zero-packet: PASS.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=${OUT:-build/bug416-xhci-test}
fresh_out "$out"
cc=${HOSTCC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -O2 -Iinclude -o "$out/plain" plan/bugs/BUG-416/host-xhci-zero-packet.c
"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
	-o "$out/sanitized" plan/bugs/BUG-416/host-xhci-zero-packet.c
timeout 60 "$out/plain" | tail -1
timeout 60 "$out/sanitized"
