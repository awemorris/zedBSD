#!/bin/sh
# Builds and runs the host test of the HID over I2C driver (ws159-p003; the driver is on the HID input glue,
# hid-input.c, since ws143-p005)
# with the Latitude 5330's touchpad descriptors, by its line and by sampling.  The driver and the HID
# files are compiled freestanding like the kernel; the test supplies the
# ACPI, I2C, input and scheduler stand-ins with the host C library.
# Usage: plan/ws159/tests/run-host-i2c-hid.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws159-host-i2c-hid}
mkdir -p "$out"

cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
kflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -nostdlibinc \
	-fno-builtin -ffunction-sections -fdata-sections -D__ZEDBSD__ \
	-DKERN_USER_ABI_LP64 -DCONFIG_DRIVER_ACPI=1 -I $root/include -I $root/src"
$cc $kflags $extra -c "$root/src/drivers/i2c/i2c-hid.c" -o "$out/i2c-hid.o"
$cc $kflags $extra -c "$root/src/drivers/generic/hid-report.c" -o "$out/hid-report.o"
$cc $kflags $extra -c "$root/src/drivers/generic/hid-digitizer.c" -o "$out/hid-digitizer.o"
$cc $kflags $extra -c "$root/src/drivers/generic/hid-touch.c" -o "$out/hid-touch.o"
$cc $kflags $extra -c "$root/src/drivers/generic/hid-input.c" -o "$out/hid-input.o"
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -I "$root/include" $extra \
	-c "$root/plan/ws159/tests/host-i2c-hid.c" -o "$out/host-i2c-hid.o"
$cc -Wl,--gc-sections $extra "$out/host-i2c-hid.o" "$out/i2c-hid.o" "$out/hid-report.o" \
	"$out/hid-digitizer.o" "$out/hid-touch.o" "$out/hid-input.o" -o "$out/host-i2c-hid"
"$out/host-i2c-hid" "$root/plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin" line
"$out/host-i2c-hid" "$root/plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin" sample
"$out/host-i2c-hid" "$root/plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin" irq
