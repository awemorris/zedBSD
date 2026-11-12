#!/bin/sh
# Builds and runs the host test of src/drivers/platform/rpi5/rpi5-fan.c: the
# test trees are built by dtc, the driver is compiled into the test, and the
# kernel's FDT reader reads the trees.
# Usage: run.sh <out-dir> [-v]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
shift
mkdir -p "$out"
for tree in full critmap nomap nofan; do
	dtc -@ -q -I dts -O dtb -i "$here" -o "$out/$tree.dtb" "$here/$tree.dts"
done
cc=${CC:-cc}
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-DHAL_ARCH_AMD64 -I"$here/shim" -I"$repo/include" -I"$repo/src" \
	-o "$out/rpi5-fan-host-test" \
	"$repo/src/drivers/generic/fdt.c" "$here/rpi5-fan-host-test.c"
timeout 120 "$out/rpi5-fan-host-test" "$out" "$@"
