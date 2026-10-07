#!/bin/sh
# Builds and runs the host test of the lock screen's clock layout (ws187-p001,
# userland/desktop/wayland/lock-clock.c compiled unchanged).
# Usage: plan/ws187/tests/run-host-lock-clock.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws187-host-lock-clock}
mkdir -p "$out"

cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I $root/userland/desktop/wayland"
$cc $flags $extra -c "$root/userland/desktop/wayland/lock-clock.c" -o "$out/lock-clock.o"
$cc $flags $extra -c "$root/plan/ws187/tests/host-lock-clock.c" -o "$out/host-lock-clock.o"
$cc $extra "$out/host-lock-clock.o" "$out/lock-clock.o" -o "$out/host-lock-clock"
"$out/host-lock-clock"
