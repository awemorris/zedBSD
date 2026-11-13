#!/bin/sh
# Host test of the kernel clock with a variable tick: src/kern/clock.c compiled freestanding against
# the kernel headers and linked with driver.c's fake HAL and scheduler, and process_itimer_tick() cut
# out of src/kern/process.c.  Prints PASS or FAIL per program; the exit status is 0 only when both pass.
#   sh plan/ws351/tests/clock-host/run.sh [OUT]      (OUT defaults to build/ws351/clock-host)
# Uses the tree's build/llvm/bin/clang for the freestanding object and cc for the rest.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
here=plan/ws351/tests/clock-host
out=${1:-build/ws351/clock-host}
mkdir -p "$out"
clang=build/llvm/bin/clang
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -DKERN_UAPI_NATIVE -ffreestanding -nostdinc -isystem $($clang -print-resource-dir)/include -I include -I src"
status=0
# shellcheck disable=SC2086
$clang $flags -c src/kern/clock.c -o "$out/clock.o" || exit 1
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -o "$out/clock-host" "$here/driver.c" "$out/clock.o" || exit 1
"$out/clock-host" || status=1
awk '$0 ~ /^process_itimer_tick\(/ {p=1; print previous} p{print} p&&/^}/{exit} {previous=$0}' src/kern/process.c > "$out/itimer-function.inc"
# shellcheck disable=SC2086
$clang $flags -I "$out" -c "$here/itimer.c" -o "$out/itimer.o" -DKERN_ITIMER_HOST -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu 2> "$out/itimer.err" || { cat "$out/itimer.err"; exit 1; }
cc -o "$out/itimer-host" "$out/itimer.o" || exit 1
"$out/itimer-host" || status=1
exit $status
