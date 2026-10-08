#!/bin/sh
# Builds and runs the host test of the PNG writer (ws189, userland/desktop/picture/png-write.c
# compiled unchanged against the host's zlib through a one-line compat header).
# Usage: plan/ws189/tests/run-host-png-write.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws189-host-png-write}
mkdir -p "$out/shim/compat/zlib"
printf '#include <zlib.h>\n' > "$out/shim/compat/zlib/zlib.h"

cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -I $out/shim -I $root/userland/desktop/picture"
$cc $flags $extra -c "$root/userland/desktop/picture/png-write.c" -o "$out/png-write.o"
$cc $flags $extra -c "$root/plan/ws189/tests/host-png-write.c" -o "$out/host-png-write.o"
$cc $extra "$out/host-png-write.o" "$out/png-write.o" -lz -o "$out/host-png-write"
"$out/host-png-write"
