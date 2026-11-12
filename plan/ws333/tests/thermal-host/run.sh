#!/bin/sh
# Builds and runs the host test of src/kern/thermal.c, src/kern/thermal-guard.c
# and src/kern/thermal-device.c (with the test injection node).
# Usage: run.sh <out-dir> [-v]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
shift
mkdir -p "$out"
cc=${CC:-cc}
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
	-DHAL_ARCH_AMD64 -DCONFIG_THERMAL_TEST_INJECT=1 -I"$here/shim" -I"$repo/include" -I"$repo/src" \
	-o "$out/thermal-host-test" \
	"$repo/src/kern/thermal.c" "$repo/src/kern/thermal-guard.c" "$repo/src/kern/thermal-device.c" "$here/thermal-host-test.c"
timeout 120 "$out/thermal-host-test" "$@"
