#!/bin/sh
# Builds and runs the host test of src/kern/cpuperf.c.
# Usage: run.sh <out-dir> [-v]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
shift
mkdir -p "$out"
cc=${CC:-cc}
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-DHAL_ARCH_AMD64 -I"$here/shim" -I"$repo/include" -I"$repo/src" \
	-o "$out/cpuperf-host-test" \
	"$repo/src/kern/cpuperf.c" "$here/cpuperf-host-test.c"
timeout 60 "$out/cpuperf-host-test" "$@"
