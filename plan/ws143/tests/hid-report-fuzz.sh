#!/bin/sh
# ws143-p005 i01a (design §5.2 [F6]): the fuzz of the kernel's HID parser (hid-report.c), its pen and touch state
# machines and the HID input glue, under ASan and UBSan: mutated and generated report descriptors and random reports,
# compared with the old usb-hid code (hid-input-host-test.c's fuzz mode).  Nothing may fault, the two must agree,
# and every key code stays within KEY_MAX.  The count and the seed are fixed, so a run is repeatable (about 30 s for the
# default 1000000 descriptors on the development machine); a regression test.
# usage: sh plan/ws143/tests/hid-report-fuzz.sh [COUNT [SEED]]   (from the repository's top; default 1000000, seed 1)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
total=${1:-1000000}
seed=${2:-1}
OUT=${OUT:-build/ws143-hid-report-fuzz}
mkdir -p "$OUT"
cc=${CC:-clang}
san="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
kflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -nostdlibinc -fno-builtin -D__ZEDBSD__ \
	-DKERN_USER_ABI_LP64 -Iinclude -Isrc $san"
hflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement -Iinclude $san"
for file in hid-report hid-digitizer hid-touch hid-input; do
	$cc $kflags -c src/drivers/generic/$file.c -o "$OUT/$file.o"
done
$cc $kflags -c plan/ws143/tests/hid-input-old.c -o "$OUT/hid-input-old.o"
$cc $hflags -c plan/ws143/tests/hid-input-host-test.c -o "$OUT/hid-input-host-test.o"
$cc $san "$OUT/hid-input-host-test.o" "$OUT/hid-input-old.o" "$OUT/hid-report.o" "$OUT/hid-digitizer.o" \
	"$OUT/hid-touch.o" "$OUT/hid-input.o" -o "$OUT/hid-report-fuzz"
timeout 300 "$OUT/hid-report-fuzz" fuzz "$total" "$seed" \
	plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin \
	plan/bugs/bug105/logi-bolt-c548-if0.rdesc plan/bugs/bug105/logi-bolt-c548-if1.rdesc \
	plan/bugs/bug105/logi-bolt-c548-if2.rdesc
