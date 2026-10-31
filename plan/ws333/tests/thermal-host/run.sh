#!/bin/sh
# Builds and runs the host test of src/kern/thermal.c and src/kern/thermal-guard.c.
# Usage: run.sh <out-dir> [-v]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
shift
mkdir -p "$out"
cc=${CC:-cc}
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
	-DHAL_ARCH_AMD64 -I"$here/shim" -I"$repo/include" -I"$repo/src" \
	-o "$out/thermal-host-test" \
	"$repo/src/kern/thermal.c" "$repo/src/kern/thermal-guard.c" "$here/thermal-host-test.c"
timeout 120 "$out/thermal-host-test" "$@"
