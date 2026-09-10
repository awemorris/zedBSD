#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
temporary=$(mktemp -d "$repo/plan/ws025-io-memory-cache/temp/uas-command.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
sources=("$repo/src/drivers/usb/usb-uas.c" "$repo/plan/ws025-io-memory-cache/tests/uas-command-host.c")
flags=(-std=c11 -Wall -Wextra -Werror -g -I"$repo/include")
cc "${flags[@]}" "${sources[@]}" -o "$temporary/plain"
"$temporary/plain"
cc "${flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-pie -no-pie "${sources[@]}" -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=1 "$temporary/sanitize"
