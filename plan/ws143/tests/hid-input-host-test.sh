#!/bin/sh
# ws143-p005 i01a: the host test of the HID input glue (src/drivers/generic/hid-input.c).  The kernel's HID files and
# the old usb-hid code the glue is compared with (hid-input-old.c, git a6988363c) are compiled freestanding as the kernel
# compiles them; the test (hid-input-host-test.c) uses the host's C library.  The glue and the old code must register the
# same devices and emit the same events for the fixed descriptors (and the Latitude 5330's touchpad and Logitech's
# receiver from files) and 20000 random reports each; the glue's own rules are checked too.
# usage: sh plan/ws143/tests/hid-input-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws143-hid-input-host}
mkdir -p "$OUT"
cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
kflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -nostdlibinc -fno-builtin -ffunction-sections \
	-fdata-sections -D__ZEDBSD__ -DKERN_USER_ABI_LP64 -Iinclude -Isrc"
hflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement -Iinclude"
for file in hid-report hid-digitizer hid-touch hid-input input-bridge-setup; do
	$cc $kflags $extra -c src/drivers/generic/$file.c -o "$OUT/$file.o"
done
$cc $kflags $extra -c plan/ws143/tests/hid-input-old.c -o "$OUT/hid-input-old.o"
$cc $hflags $extra -c plan/ws143/tests/hid-input-host-test.c -o "$OUT/hid-input-host-test.o"
$cc $extra -Wl,--gc-sections "$OUT/hid-input-host-test.o" "$OUT/hid-input-old.o" "$OUT/hid-report.o" \
	"$OUT/hid-digitizer.o" "$OUT/hid-touch.o" "$OUT/hid-input.o" "$OUT/input-bridge-setup.o" -o "$OUT/hid-input-host-test"
timeout 300 "$OUT/hid-input-host-test" check \
	plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin \
	plan/bugs/bug105/logi-bolt-c548-if0.rdesc plan/bugs/bug105/logi-bolt-c548-if1.rdesc \
	plan/bugs/bug105/logi-bolt-c548-if2.rdesc
