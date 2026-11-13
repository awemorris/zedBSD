#!/bin/sh
# Builds and runs the host test of the touch pad's next timer (userland/desktop/wayland/touchpad.c compiled
# unchanged).  Usage: sh plan/ws352/tests/pad-due/run.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:-$root/build/ws352/pad-due}
mkdir -p "$out"
cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I $root/userland/desktop/wayland"
$cc $flags $extra -c "$root/userland/desktop/wayland/touchpad.c" -o "$out/touchpad.o"
$cc $flags $extra -c "$root/plan/ws352/tests/pad-due/host-pad-due.c" -o "$out/host-pad-due.o"
$cc $extra "$out/host-pad-due.o" "$out/touchpad.o" -o "$out/host-pad-due"
"$out/host-pad-due"
