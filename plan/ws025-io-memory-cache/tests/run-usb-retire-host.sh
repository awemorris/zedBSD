#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
temporary=$(mktemp -d "$repo/plan/ws025-io-memory-cache/temp/usb-retire.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
flags=(-std=gnu11 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections
       -I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT)
source="$repo/plan/ws025-io-memory-cache/tests/usb-retire-host.c"
cc "${flags[@]}" "$source" -Wl,--gc-sections -o "$temporary/plain"
"$temporary/plain"
# Keep unrelated, unused class registration sections discardable under ASan.
cc "${flags[@]}" -fsanitize=address,undefined --param asan-globals=0 \
   -fno-omit-frame-pointer -fno-pie -no-pie "$source" -Wl,--gc-sections -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=1 "$temporary/sanitize"
