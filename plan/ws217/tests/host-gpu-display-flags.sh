#!/bin/sh
# The display core's flag checks (src/drivers/gpu/gpu-display-flags.h) on the host, under ASan/UBSan.
#   sh plan/ws217/tests/host-gpu-display-flags.sh [OUT]   (default build/ws217-host)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
OUT=${1:-build/ws217-host}
mkdir -p "$OUT"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-DKERN_USER_ABI_LP64 -Iinclude -Isrc -I. -idirafter libc/include \
	-o "$OUT/host-display-flags-test" src/drivers/gpu/tests/host-display-flags-test.c
"$OUT/host-display-flags-test"
