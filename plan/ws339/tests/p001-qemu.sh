#!/bin/sh
# The QEMU (OVMF) check of the UEFI loader's choice of the boot screen's size from the monitor's EDID: the image of
# config-x86_64-edid.mk (the standard zedbsd-uefi.cfg: no video=, no logo) is booted by plan/tools/boot-test.sh once
# for each display below, and the size of the screen at the login prompt (the screenshot's, which is the GOP mode
# the loader left: the kernel draws on that framebuffer) is compared with what the display's EDID prefers.
#
#   plan/ws339/tests/p001-qemu.sh OUT
#
# The image must be made (make CONFIGMK=plan/ws339/tests/config-x86_64-edid.mk disk, in build/x86_64).  Each case
# writes OUT/<case>/login.png; OUT/summary.txt has one line per case: the case, PASS or FAIL of the login prompt, the
# screen's size, and what the case expects.  The last line is "p001-qemu: DONE"; the verdict is read from the lines.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p001-qemu.sh OUT" >&2; exit 2; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
summary=$out/summary.txt
: > "$summary"
image=build/x86_64/hdd-image.img

# One boot: case name, what it expects, the display options.
run_case()
{
	name=$1
	expect=$2
	display=$3
	mkdir -p "$out/$name"
	P001_DISPLAY=$display QEMU="$PWD/plan/ws339/tests/qemu-display.sh" OUTPUT="$out/$name" BOOT_TIMEOUT=240 \
		timeout 400 plan/tools/boot-test.sh "$image" > "$out/$name/boot-test.out" 2>&1
	rc=$?
	verdict=FAIL
	[ $rc -eq 0 ] && verdict=PASS
	size=$(python3 -I -c 'import struct,sys
d=open(sys.argv[1],"rb").read(24)
print("%dx%d" % struct.unpack(">II", d[16:24]))' "$out/$name/login.png" 2>/dev/null || echo none)
	echo "$name login=$verdict screen=$size expect=$expect display=$display" >> "$summary"
}

# The std VGA (bochs) with an EDID preferring 1600x900, a size OVMF's QemuVideo lists.
run_case vga-edid-1600x900 1600x900 "-device VGA,edid=on,xres=1600,yres=900"
# The same with an EDID preferring 1500x1000 (3:2), which OVMF does not list: the largest mode inside it.
run_case vga-edid-1500x1000 fit-inside-1500x1000 "-device VGA,edid=on,xres=1500,yres=1000"
# The std VGA without an EDID: the firmware's mode stays, as before.
run_case vga-no-edid firmware-mode "-device VGA,edid=off"
# virtio-vga with an EDID preferring 1600x900.
run_case virtio-vga-edid-1600x900 1600x900-if-the-firmware-gives-edid "-device virtio-vga,edid=on,xres=1600,yres=900"
# bochs-display with an EDID preferring 1600x900.
run_case bochs-edid-1600x900 1600x900-if-the-firmware-gives-edid "-device bochs-display,edid=on,xres=1600,yres=900"
sync
cat "$summary"
echo "p001-qemu: DONE"
