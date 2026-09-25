#!/bin/bash
# zedBSD boot test: UEFI boot from a USB stick, photographed at the login prompt.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The machine boots the disk image as a USB mass storage device through OVMF,
# the way the physical machines are booted, and the screen is captured from the
# emulator's framebuffer.  The console draws to the framebuffer, so a picture is
# what a person at the machine would see; nothing here reads a serial port.
#
#   plan/tools/boot-test.sh [IMAGE]
#
#   IMAGE          disk image to boot, as the operand or in the environment
#                  (default build/amd64/hdd-image.img)
#   OUTPUT         where the screenshot and logs go (default build/boot-test)
#   BOOT_TIMEOUT   seconds to wait for the login prompt (default 180)
#   QEMU           emulator to run (default depends on BOOT_MODE)
#   BOOT_MODE      uefi-nvme (default), uefi-usb, bios-ide or raspi4b
#
# BOOT_MODE=bios-ide boots the image the way i386 machines are booted: the
# firmware is the PC BIOS and the disk is on IDE.  Those kernels put their
# console in the VGA text buffer rather than a linear framebuffer, which the
# screen reader handles; the picture is still what the screen shows.
#
# BOOT_MODE=raspi4b boots a Raspberry Pi 4 image (arm64) in QEMU's raspi4b.
# QEMU does not run the Pi's GPU firmware, so the kernel (vmunix beside the
# image, or KERNEL) and the DTB are given to it directly, and the image is the
# SD card.  The console centres its 80x25 grid on the framebuffer, which the
# screen reader also tries.
#
# It writes OUTPUT/login.png and prints its path.  The exit status is 0 only
# when the login prompt was seen.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
image=${1:-${IMAGE:-$root/build/amd64/hdd-image.img}}
output=${OUTPUT:-$root/build/boot-test}
boot_timeout=${BOOT_TIMEOUT:-180}
boot_mode=${BOOT_MODE:-uefi-nvme}
code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

case $boot_mode in
uefi-usb|uefi-nvme)
	qemu=${QEMU:-qemu-system-x86_64}
	required=("$image" "$code" "$vars")
	;;
bios-ide)
	qemu=${QEMU:-qemu-system-i386}
	required=("$image")
	;;
raspi4b)
	qemu=${QEMU:-qemu-system-aarch64}
	kernel=${KERNEL:-$(dirname -- "$image")/vmunix}
	dtb=${DTB:-$root/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb}
	required=("$image" "$kernel" "$dtb")
	;;
*)
	echo "boot-test: unknown BOOT_MODE: $boot_mode" >&2
	exit 1
	;;
esac

for file in "${required[@]}"; do
	if [[ ! -f $file ]]; then
		echo "boot-test: missing file: $file" >&2
		exit 1
	fi
done

rm -rf "$output"
mkdir -p "$output"
disk=$output/stick.img
nvram=$output/uefi-vars.fd
monitor=$output/qmp.sock
screenshot=$output/login.png

# The guest writes to the stick it booted from, so it is given a copy, and the
# firmware gets its own variable store.
cp -- "$image" "$disk"
if [[ $boot_mode == uefi-usb || $boot_mode == uefi-nvme ]]; then
	cp -- "$vars" "$nvram"
fi

cleanup()
{
	if [[ -n ${pid:-} ]] && kill -0 "$pid" 2>/dev/null; then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
	rm -f -- "$disk" "$monitor"
}
trap cleanup EXIT INT TERM

if [[ $boot_mode == uefi-usb || $boot_mode == uefi-nvme ]]; then
	# The boot disk is a USB stick (the way the machines are tried) or an
	# NVMe drive (the way they are installed; 2026-09-25 user direction:
	# the USB model is slow and is not used for measuring).
	if [[ $boot_mode == uefi-nvme ]]; then
		boot_device=(-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1)
	else
		boot_device=(-device usb-storage,bus=xhci.0,port=1,drive=boot,id=rootstick,bootindex=1)
	fi
	# amd64 is tested with 8 GiB (2026-09-24 user decision).  raspi4b
	# below cannot follow: QEMU's model of the board takes only 2 GiB.
	"$qemu" -machine q35 -m 8G -smp 4 -cpu max \
		-drive "if=pflash,format=raw,readonly=on,file=$code" \
		-drive "if=pflash,format=raw,file=$nvram" \
		-device qemu-xhci,id=xhci \
		-drive "if=none,id=boot,file=$disk,format=raw" \
		"${boot_device[@]}" \
		-device usb-kbd,bus=xhci.0,port=2 \
		-vga std -display none -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == raspi4b ]]; then
	"$qemu" -machine raspi4b -m 2G -kernel "$kernel" -dtb "$dtb" \
		-drive "if=sd,format=raw,file=$disk" \
		-display none -serial null -serial null -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
else
	"$qemu" -machine pc -m 256 -cpu max \
		-drive "if=ide,bus=0,unit=0,format=raw,file=$disk" \
		-vga std -display none -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
fi
pid=$!

# The screen is read, not a character stream: the login prompt is the string
# the console draws, so the picture is compared against what OCR would have to
# read.  Instead of reading pixels, this waits for the guest to stop writing to
# the stick and then photographs the screen; the login prompt is the first
# quiet point after the boot, and the picture shows whether that is what it is.
python3 "$root/plan/tools/boot-test.py" \
	--monitor "$monitor" --screenshot "$screenshot" \
	--timeout "$boot_timeout"
status=$?

if [[ $status -eq 0 ]]; then
	echo "boot-test: PASS $screenshot"
fi
exit $status
