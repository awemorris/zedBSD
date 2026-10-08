#!/bin/sh
# Builds and runs the host test of a drag's mark (ws189, userland/desktop/wayland/dnd-state.c
# compiled unchanged).
# Usage: plan/ws189/tests/run-host-dnd-state.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws189-host-dnd-state}
mkdir -p "$out"

cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I $root/userland/desktop/wayland"
$cc $flags $extra -c "$root/userland/desktop/wayland/dnd-state.c" -o "$out/dnd-state.o"
$cc $flags $extra -c "$root/plan/ws189/tests/host-dnd-state.c" -o "$out/host-dnd-state.o"
$cc $extra "$out/host-dnd-state.o" "$out/dnd-state.o" -o "$out/host-dnd-state"
"$out/host-dnd-state"
