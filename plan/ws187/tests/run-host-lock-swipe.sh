#!/bin/sh
# Builds and runs the host test of the lock screen's unlocking moves and grace (ws187-p002,
# userland/desktop/wayland/lock-swipe.c compiled unchanged).
# Usage: plan/ws187/tests/run-host-lock-swipe.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws187-host-lock-swipe}
mkdir -p "$out"

cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I $root/userland/desktop/wayland"
$cc $flags $extra -c "$root/userland/desktop/wayland/lock-swipe.c" -o "$out/lock-swipe.o"
$cc $flags $extra -c "$root/plan/ws187/tests/host-lock-swipe.c" -o "$out/host-lock-swipe.o"
$cc $extra "$out/host-lock-swipe.o" "$out/lock-swipe.o" -o "$out/host-lock-swipe"
"$out/host-lock-swipe"
