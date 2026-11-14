#!/bin/sh
# The QEMU (OVMF) check of the UEFI loader's choice of the boot screen's size from the monitor's EDID.  Two images are
# made in build/x86_64 and copied to OUT: plain.img of config-x86_64-edid.mk (the standard zedbsd-uefi.cfg: no
# video=, no logo) and logo.img of config-x86_64-edid-logo.mk (the same with logo=logo.ppm, for which the loader
# without an EDID wishes for 1920x1080).  plan/tools/boot-test.sh boots each case with the display it names (through
# qemu-display.sh, QEMU named p5-ws339), and the size of the screen at the login prompt (the screenshot's: the GOP
# mode the loader left, on which the kernel draws) is compared with the case's expectation.
#
# The host's OVMF (Debian's edk2, UEFI 2.70) installs neither EDID protocol for any of these displays (p001-shell.sh
# shows it in the UEFI shell): its QemuVideo (std VGA, bochs-display) reads the EDID itself and starts in its preferred
# size, and VirtioGpu (virtio-vga) starts in 1280x800.  So every case here is the loader without an EDID, which must
# do what it did before: keep the firmware's mode, or set the splash's 1920x1080 for a logo.  That the EDID is read and
# its size chosen is shown by the host test (edid-test.c) and on a machine whose GOP installs the protocols.
#
#   plan/ws339/tests/p001-qemu.sh OUT
#
# OUT/summary.txt has one line per case: the case, PASS or FAIL of the login prompt, the screen's size and the
# expected one, and OK when both hold.  The last line is "p001-qemu: PASS" when every case is OK.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p001-qemu.sh OUT" >&2; exit 2; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
summary=$out/summary.txt
: > "$summary"
status=PASS

# Makes one image of a configuration and copies it to OUT.
make_image()
{
	timeout 3600 make -j4 CONFIGMK="$1" disk > "$out/$2.log" 2>&1 < /dev/null
	rc=$?
	echo "image $2 rc=$rc warnings=$(grep -c 'warning:' "$out/$2.log")" >> "$summary"
	[ $rc -eq 0 ] || { cat "$summary"; echo "p001-qemu: FAIL"; exit 1; }
	cp --sparse=always build/x86_64/hdd-image.img "$out/$2.img"
	sync
}

# One boot: case name, image, the expected screen size, the display options.
run_case()
{
	name=$1
	image=$2
	expect=$3
	display=$4
	P001_DISPLAY=$display QEMU="$PWD/plan/ws339/tests/qemu-display.sh" OUTPUT="$out/$name" BOOT_TIMEOUT=240 \
		timeout 400 plan/tools/boot-test.sh "$out/$image" > "$out/$name.out" 2>&1
	rc=$?
	verdict=FAIL
	[ $rc -eq 0 ] && verdict=PASS
	size=$(python3 -I -c 'import struct,sys
d=open(sys.argv[1],"rb").read(24)
print("%dx%d" % struct.unpack(">II", d[16:24]))' "$out/$name/login.png" 2>/dev/null || echo none)
	result=OK
	if [ $verdict != PASS ] || [ "$size" != "$expect" ]; then
		result=NG
		status=FAIL
	fi
	echo "$name login=$verdict screen=$size expect=$expect $result display=$display" >> "$summary"
}

make_image plan/ws339/tests/config-x86_64-edid.mk plain
make_image plan/ws339/tests/config-x86_64-edid-logo.mk logo

# The std VGA with an EDID preferring 1600x900: OVMF starts in it, and it stays.
run_case vga-edid plain.img 1600x900 "-device VGA,edid=on,xres=1600,yres=900"
# The std VGA without an EDID: the firmware's 1280x800 stays.
run_case vga-no-edid plain.img 1280x800 "-device VGA,edid=off"
# virtio-vga: the firmware gives no EDID protocol, its 1280x800 stays.
run_case virtio-vga plain.img 1280x800 "-device virtio-vga,edid=on,xres=1600,yres=900"
# bochs-display with an EDID preferring 1600x900.
run_case bochs-edid plain.img 1600x900 "-device bochs-display,edid=on,xres=1600,yres=900"
# With the logo on the std VGA (OVMF starts in the EDID's 1600x900 but gives no EDID protocol): 1920x1080 as before.
run_case logo-vga-edid logo.img 1920x1080 "-device VGA,edid=on,xres=1600,yres=900"
# With the logo and no EDID: the splash's 1920x1080 as before.
run_case logo-vga-no-edid logo.img 1920x1080 "-device VGA,edid=off"
# With the logo on virtio-vga (no EDID protocol): 1920x1080 as before.
run_case logo-virtio-vga logo.img 1920x1080 "-device virtio-vga,edid=on,xres=1600,yres=900"
sync
cat "$summary"
echo "p001-qemu: $status"
