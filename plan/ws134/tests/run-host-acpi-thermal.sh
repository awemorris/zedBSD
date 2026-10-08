#!/bin/sh
# Builds and runs the host test of the ACPI temperature sensors (ws134-p009): src/drivers/acpi/acpi-thermal.c compiled
# freestanding like the kernel, the test (a stand-in namespace) with the host C library and the sanitizers.
#   plan/ws134/tests/run-host-acpi-thermal.sh [OUTPUT]   (default build/ws134-host-thermal)
# Each run gets a new directory behind OUTPUT (plan/tools/fresh-out.sh); nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
out=${1:-build/ws134-host-thermal}
. plan/tools/fresh-out.sh
fresh_out "$out"
cc=${CC:-clang}
san="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
# shellcheck disable=SC2086
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -nostdlibinc \
	-fno-builtin -D__ZEDBSD__ -DKERN_USER_ABI_LP64 -DCONFIG_DRIVER_ACPI=1 $san \
	-I "$root/include" -I "$root/src" \
	-c "$root/src/drivers/acpi/acpi-thermal.c" -o "$out/acpi-thermal.o"
# shellcheck disable=SC2086
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror $san -I "$root/include" -I "$root/src" \
	-c "$root/plan/ws134/tests/host-acpi-thermal.c" -o "$out/host-acpi-thermal.o"
# shellcheck disable=SC2086
$cc $san "$out/host-acpi-thermal.o" "$out/acpi-thermal.o" -o "$out/host-acpi-thermal"
"$out/host-acpi-thermal"
