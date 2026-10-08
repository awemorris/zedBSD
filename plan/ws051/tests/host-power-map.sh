#!/bin/sh
# BUG-256: builds the display's power map (src/drivers/gpu/i915/display/power.c) with the host's C compiler and
# host-power-map.c against it, plainly and under ASan/UBSan, and runs it.
#   sh plan/ws051/tests/host-power-map.sh [OUT_DIR]   (default build/ws051-host)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
dir=${1:-build/ws051-host}
mkdir -p "$dir"
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -DKERN_USER_ABI_LP64 -I. -Iinclude -idirafter include/libc"
for mode in plain sanitize; do
	extra=
	if test "$mode" = sanitize; then
		extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
	fi
	cc $flags $extra src/drivers/gpu/i915/display/power.c plan/ws051/tests/host-power-map.c -o "$dir/host-power-map-$mode"
	"$dir/host-power-map-$mode"
done
