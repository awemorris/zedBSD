#!/bin/sh
# Builds and runs the host test of the Intel TCO watchdog (src/drivers/platform/pcat/intel-tco.c): the driver's file
# is included by tco-host-test.c, whose stubs model the TCO registers, the hub's ACPI timer switch, the ICH9's
# general control and a millisecond clock.  Built with ACPI (the controller's base from the firmware's name) and
# with ASan and UBSan.
# Usage: run.sh <out-dir>   (TCO_HOST_VERBOSE=1 prints the driver's log lines)
# Last line: tco-host: PASS or tco-host: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
out=${1:?out dir}
mkdir -p "$out"
cc=${CC:-cc}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -DHAL_ARCH_AMD64 -DCONFIG_DRIVER_ACPI=1 -I$here/shim -I$repo/include -I$repo/src"
san="-fsanitize=address,undefined -fno-sanitize-recover=all"
status=0
if ! $cc $flags $san -o "$out/tco-host" "$here/tco-host-test.c" > "$out/build.log" 2>&1; then
	echo "build: FAILED"; head -40 "$out/build.log"; status=1
elif ! timeout 120 "$out/tco-host"; then
	status=1
fi
if [ "$status" = 0 ]; then echo "tco-host: PASS"; else echo "tco-host: FAIL"; fi
exit "$status"
