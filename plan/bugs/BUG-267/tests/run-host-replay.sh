#!/bin/sh
# BUG-267: builds and runs the host test of the 5330's USB touch screen (host-replay.c) against the kernel's HID files
# (compiled freestanding as the kernel compiles them), with the descriptors and the reports the 5330 dumped.
# usage: sh plan/bugs/BUG-267/tests/run-host-replay.sh   (from the repository's top; a new folder under build/tmp)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
. plan/tools/fresh-out.sh
fresh_out build/tmp/bug267-replay
out=$fresh_dir
cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
kflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -nostdlibinc -fno-builtin -ffunction-sections \
	-fdata-sections -D__ZEDBSD__ -DKERN_USER_ABI_LP64 -Iinclude -Isrc"
for file in hid-report hid-digitizer hid-touch; do
	$cc $kflags $extra -c src/drivers/generic/$file.c -o "$out/$file.o"
done
$cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude $extra -c plan/bugs/BUG-267/tests/host-replay.c -o "$out/host-replay.o"
$cc -Wl,--gc-sections $extra "$out/host-replay.o" "$out/hid-report.o" "$out/hid-digitizer.o" "$out/hid-touch.o" \
	-o "$out/host-replay"
"$out/host-replay" plan/bugs/BUG-267/touchscreen-if0.rdesc plan/bugs/BUG-267/touchscreen-if1.rdesc \
	plan/bugs/BUG-267/kernel.log
