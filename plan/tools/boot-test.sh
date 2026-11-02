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
#   BOOT_TEST_WORK where the emulator's working files go: the stick's copy, the
#                  firmware variables, the monitor socket and the screen
#                  frames (default a new directory under TMPDIR, else /tmp)
#   BOOT_TIMEOUT   seconds to wait for the login prompt (default 180)
#   QEMU           emulator to run (default depends on BOOT_MODE)
#   BOOT_MODE      uefi-nvme (default), uefi-usb, bios-ide, raspi4b,
#                  riscv64-virt, riscv64-uboot, riscv64-uboot-gop, riscv32-virt, m68k-virt,
#                  ppc32-mac99, ppc64-pseries, mips32-malta, mips64-malta,
#                  armv7-virt or sun4u
#   BOOT_PATTERN   what the screen must show, one regular expression per line;
#                  every one must be seen on some screen (default the login
#                  prompt, (?:^(?:\S+ )?|\S)login: ?, also when other output
#                  runs into it on its line)
#   BOOT_THEN      steps after the patterns were seen, one per line:
#                  "type:TEXT" types TEXT and a return on the guest's serial
#                  line, "keys:TEXT" types TEXT and a return on the emulated
#                  keyboard through QMP send-key (US layout; the guest's own
#                  keyboard such as -device usb-kbd), "mouse:DX,DY[,BUTTON]"
#                  moves the emulated mouse and clicks left, middle or right
#                  through QMP input-send-event, "expect:REGEX" waits for
#                  the screen to show REGEX;
#                  the last screen is OUTPUT/login-after.png
#   BOOT_TEXT_ORIGINS  more "x,y" pixel origins of 8x16 text to read, separated
#                  by spaces, for text off the console's grid (a terminal in
#                  an X window); the reading that recognises most is kept
#   BOOT_INTERVAL  seconds between two looks at the screen (default 2, 0.5 in
#                  the modes that boot a kernel directly)
#   BOOT_NVME_BLOCK_SIZE  in uefi-nvme, the boot disk's logical and physical
#                  block size in bytes (4096 for a 4Kn disk); empty leaves
#                  QEMU's default of 512
#   QEMU_NO_KVM    1 runs the amd64 guest on TCG (-cpu max) even when /dev/kvm
#                  is usable; otherwise it runs under KVM (-cpu host) when the
#                  host offers it (plan/tools/guest/qemu-accel.sh)
#
# The bios-ide and raspi4b guests always run on TCG.
#
# BOOT_MODE=bios-ide boots the image the way i386 machines are booted: the
# firmware is the PC BIOS and the disk is on IDE.  Those kernels put their
# console in the VGA text buffer rather than a linear framebuffer, which the
# screen reader handles; the picture is still what the screen shows.  The
# emulator follows the kernel beside the image (vmunix, or KERNEL): a 64-bit
# ELF kernel (an amd64 image's bios variant) runs on qemu-system-x86_64, which
# qemu-system-i386 cannot (it stops at "Booting from Hard Disk..."), and a
# 32-bit one, or an image without a kernel beside it, on qemu-system-i386.
#
# BOOT_MODE=raspi4b boots a Raspberry Pi 4 image (arm64) in QEMU's raspi4b.
# QEMU does not run the Pi's GPU firmware, so the kernel (vmunix beside the
# image, or KERNEL) and the DTB are given to it directly, and the image is the
# SD card.  The console centres its 80x25 grid on the framebuffer, which the
# screen reader also tries.
#
# BOOT_MODE=riscv64-virt boots a RISC-V kernel directly in QEMU's virt machine:
# the operand (or KERNEL, default build/riscv64-virt/vmunix) is the kernel ELF,
# which the default firmware (OpenSBI) enters in supervisor mode.  The machine
# has a ramfb display, which the kernel's console draws to, so the screen is
# read the same way.  INITRD (an initial ramdisk), BOOT_APPEND (the kernel
# command line), BOOT_SMP (default 4), BOOT_MEMORY (default 2G) and QEMU_EXTRA
# (more emulator options, split on spaces, such as -cpu rv64,sstc=off) shape
# the machine.  BOOT_MODE=riscv64-uboot boots the operand as a disk image on
# virtio-blk through U-Boot (UBOOT, default the Debian u-boot-qemu
# qemu-riscv64_smode build), with the same display.  Both run on TCG.  The
# serial line is a socket only typed into by BOOT_THEN; what the guest writes
# there is drained and not kept.  BOOT_MODE=riscv64-uboot-gop is riscv64-uboot
# with a bochs-display in place of the ramfb: U-Boot drives it and hands it to
# the loader as the graphics output, so a VisionFive 2 kernel (operand default
# build/visionfive2/disk.img), whose console draws on the screen the firmware
# left on, is read from it; the virt kernel draws only on a ramfb.  BOOT_MODE=riscv32-virt is riscv64-virt for
# a 32-bit kernel (default build/riscv32-virt/vmunix) on qemu-system-riscv32,
# with BOOT_MEMORY defaulting to 512M.  BOOT_MODE=m68k-virt boots an m68k
# kernel (default build/m68k-virt/vmunix) directly in qemu-system-m68k's virt
# machine with an MC68040 and 512M (BOOT_MEMORY); its display is a
# virtio-gpu device, its first serial line the same socket.
# BOOT_MODE=mips32-malta boots a little-endian MIPS32 kernel (default
# build/mips32-malta/vmunix) directly on qemu-system-mipsel's malta with the
# std VGA and 256M (BOOT_MEMORY), and mips64-malta a MIPS64 one (default
# build/mips64-malta/vmunix) on qemu-system-mips64el with the MIPS64R2-generic
# CPU; BOOT_APPEND is the kernel's command line.
# BOOT_MODE=ppc32-mac99 boots a flat 32-bit PowerPC kernel image (default
# build/ppc32-mac99/vmunix.bin) on qemu-system-ppc's mac99 through OpenBIOS,
# whose display is set to 800x600 at 32 bits a pixel; 512M (BOOT_MEMORY).
# BOOT_MODE=ppc64-pseries boots a big-endian 64-bit PowerPC kernel ELF
# (default build/ppc64-pseries/vmunix) on qemu-system-ppc64's pseries through
# SLOF, whose VGA display the kernel draws on; 512M (BOOT_MEMORY).
# BOOT_MODE=armv7-virt boots a raw 32-bit ARMv7-A kernel image (default
# build/armv7-virt/vmunix.bin) on qemu-system-arm -M virt with a Cortex-A15,
# a GICv2, 512M (BOOT_MEMORY) and a ramfb display the kernel draws its
# console on.  QEMU enters a raw image the way it boots Linux, with the
# device tree in r2, so INITRD and BOOT_APPEND reach the kernel through
# /chosen; an ELF image (vmunix) would get neither, so it is only gdb's.
#
# BOOT_MODE=sun4u boots a SPARC V9 disk image (default build/sun4u/hdd-image.img)
# in QEMU's sun4u machine from its IDE disk through OpenBIOS, with 256 MiB
# (BOOT_MEMORY) on TCG; BOOT_TIMEOUT defaults to 360.  The kernel draws its console on the machine's
# standard VGA, which the screen reader reads; the serial line is a socket
# only typed into by BOOT_THEN.
#
# It writes OUTPUT/login.png and prints its path.  The exit status is 0 only
# when the login prompt was seen.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
. "$root/plan/tools/guest/qemu-accel.sh"
output=${OUTPUT:-$root/build/boot-test}
boot_timeout=${BOOT_TIMEOUT:-180}
boot_mode=${BOOT_MODE:-uefi-nvme}
boot_interval=${BOOT_INTERVAL:-2}
# The riscv64 modes take a kernel or their own disk as the default operand.
case $boot_mode in
riscv64-virt)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/riscv64-virt/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
riscv64-uboot)
	image=${1:-${IMAGE:-$root/build/riscv64-virt/hdd-image.img}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
riscv64-uboot-gop)
	image=${1:-${IMAGE:-$root/build/visionfive2/disk.img}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
riscv32-virt)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/riscv32-virt/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
m68k-virt)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/m68k-virt/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
mips32-malta)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/mips32-malta/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
mips64-malta)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/mips64-malta/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
armv7-virt)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/armv7-virt/vmunix.bin}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
ppc32-mac99)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/ppc32-mac99/vmunix.bin}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
ppc64-pseries)
	image=${1:-${IMAGE:-${KERNEL:-$root/build/ppc64-pseries/vmunix}}}
	boot_interval=${BOOT_INTERVAL:-0.5}
	;;
sun4u)
	image=${1:-${IMAGE:-$root/build/sun4u/hdd-image.img}}
	# OpenBIOS and the emulated UltraSPARC take a few minutes to the prompt.
	boot_timeout=${BOOT_TIMEOUT:-360}
	;;
*)
	image=${1:-${IMAGE:-$root/build/amd64/hdd-image.img}}
	;;
esac
code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
vars=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

case $boot_mode in
uefi-usb|uefi-nvme)
	qemu=${QEMU:-qemu-system-x86_64}
	required=("$image" "$code" "$vars")
	;;
bios-ide)
	# The ELF class of the kernel beside the image (byte 5: 1 is 32-bit, 2 is 64-bit) chooses the emulator.
	kernel=${KERNEL:-$(dirname -- "$image")/vmunix}
	bios_qemu=qemu-system-i386
	if [[ -f $kernel ]]; then
		elf_class=$(od -An -tu1 -j4 -N1 -- "$kernel" | tr -d ' ')
		if [[ $elf_class == 2 ]]; then
			bios_qemu=qemu-system-x86_64
		fi
	fi
	qemu=${QEMU:-$bios_qemu}
	required=("$image")
	;;
raspi4b)
	qemu=${QEMU:-qemu-system-aarch64}
	kernel=${KERNEL:-$(dirname -- "$image")/vmunix}
	# The build puts the device tree beside the image (ws233-p001); for an image built before that,
	# rpi4-dtb.sh takes the same file from a shallow clone of raspberrypi/firmware.
	dtb=${DTB:-$(sh "$root/plan/tools/rpi4-dtb.sh" "$image")}
	required=("$image" "$kernel" "$dtb")
	;;
riscv64-virt)
	qemu=${QEMU:-qemu-system-riscv64}
	required=("$image")
	if [[ -n ${INITRD:-} ]]; then
		required+=("$INITRD")
	fi
	;;
riscv32-virt)
	qemu=${QEMU:-qemu-system-riscv32}
	required=("$image")
	if [[ -n ${INITRD:-} ]]; then
		required+=("$INITRD")
	fi
	;;
m68k-virt)
	qemu=${QEMU:-qemu-system-m68k}
	required=("$image")
	;;
armv7-virt)
	qemu=${QEMU:-qemu-system-arm}
	required=("$image")
	if [[ -n ${INITRD:-} ]]; then
		required+=("$INITRD")
	fi
	;;
mips32-malta)
	qemu=${QEMU:-qemu-system-mipsel}
	required=("$image")
	;;
mips64-malta)
	qemu=${QEMU:-qemu-system-mips64el}
	required=("$image")
	;;
ppc32-mac99)
	qemu=${QEMU:-qemu-system-ppc}
	required=("$image")
	;;
ppc64-pseries)
	qemu=${QEMU:-qemu-system-ppc64}
	required=("$image")
	;;
riscv64-uboot|riscv64-uboot-gop)
	qemu=${QEMU:-qemu-system-riscv64}
	uboot=${UBOOT:-/usr/lib/u-boot/qemu-riscv64_smode/u-boot.bin}
	required=("$image" "$uboot")
	;;
sun4u)
	qemu=${QEMU:-qemu-system-sparc64}
	required=("$image")
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

# An earlier output is moved aside, not removed (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh).
if [[ -e $output ]]; then
	mv -- "$output" "$output.old.$$"
fi
mkdir -p "$output"

# The emulator's working files live on a scratch file system, normally the
# tmpfs /tmp, not beside the output.  The emulator writes the guest's disk and
# each screen frame from its main loop; on a full or busy disk those writes
# stall it for tens of seconds and the monitor stops answering (ws129-p009:
# QMP screendump took over 30 s with the stick in build/ and 0.02 s with it
# in /tmp, for the same image).
work_parent=${BOOT_TEST_WORK:-${TMPDIR:-/tmp}}
mkdir -p "$work_parent"
work=$(mktemp -d "$work_parent/boot-test.XXXXXX")
disk=$work/stick.qcow2
nvram=$work/uefi-vars.fd
monitor=$work/qmp.sock
serial=$work/serial.sock
frame=$work/screen.ppm
screenshot=$output/login.png

# The guest writes to the stick it booted from, so it is given a copy-on-write
# overlay of the image (small, so that a work directory left for Q1's cleanup
# costs little), and the firmware gets its own variable store.  A riscv64-virt
# kernel is booted directly and has no disk.
if [[ $boot_mode != riscv64-virt && $boot_mode != riscv32-virt && $boot_mode != m68k-virt &&
	$boot_mode != ppc32-mac99 && $boot_mode != ppc64-pseries &&
	$boot_mode != mips32-malta && $boot_mode != mips64-malta &&
	$boot_mode != armv7-virt ]]; then
	qemu-img create -q -f qcow2 -F raw -b "$(readlink -f -- "$image")" "$disk"
	# QEMU's SD card must be a power of two in size; the overlay grows to the
	# next one and the image itself is left as it is.
	if [[ $boot_mode == raspi4b ]]; then
		size=$(stat -c %s -- "$image")
		card=1048576
		while ((card < size)); do
			card=$((card * 2))
		done
		qemu-img resize -q -f qcow2 "$disk" "$card"
	fi
fi
if [[ $boot_mode == uefi-usb || $boot_mode == uefi-nvme ]]; then
	cp -- "$vars" "$nvram"
fi

cleanup()
{
	if [[ -n ${pid:-} ]] && kill -0 "$pid" 2>/dev/null; then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
	# The work directory stays for Q1's cleanup (plan/tools/q1-clean.sh --tmp).
}
trap cleanup EXIT INT TERM

if [[ $boot_mode == uefi-usb || $boot_mode == uefi-nvme ]]; then
	# The boot disk is a USB stick (the way the machines are tried) or an
	# NVMe drive (the way they are installed; 2026-09-25 user direction:
	# the USB model is slow and is not used for measuring).
	if [[ $boot_mode == uefi-nvme ]]; then
		boot_device=(-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1)
		if [[ -n ${BOOT_NVME_BLOCK_SIZE:-} ]]; then
			boot_device=(-device "nvme,serial=zedbsd-boot,drive=boot,bootindex=1,logical_block_size=$BOOT_NVME_BLOCK_SIZE,physical_block_size=$BOOT_NVME_BLOCK_SIZE")
		fi
	else
		boot_device=(-device usb-storage,bus=xhci.0,port=1,drive=boot,id=rootstick,bootindex=1)
	fi
	# amd64 is tested with 8 GiB (2026-09-24 user decision).  raspi4b
	# below cannot follow: QEMU's model of the board takes only 2 GiB.
	# KVM when the host offers it (2026-10-03 user: every test uses KVM).
	read -r -a accel <<<"$(qemu_accel_args max)"
	"$qemu" -machine q35 -m 8G -smp 4 "${accel[@]}" \
		-drive "if=pflash,format=raw,readonly=on,file=$code" \
		-drive "if=pflash,format=raw,file=$nvram" \
		-device qemu-xhci,id=xhci \
		-drive "if=none,id=boot,file=$disk,format=qcow2" \
		"${boot_device[@]}" \
		-device usb-kbd,bus=xhci.0,port=2 \
		-vga std -display none -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == riscv64-virt || $boot_mode == riscv64-uboot || $boot_mode == riscv64-uboot-gop ||
	$boot_mode == riscv32-virt ]]; then
	# The virt machine with a ramfb display, the console the kernel draws,
	# and its first serial line on a socket for typing into.
	riscv_memory=2G
	if [[ $boot_mode == riscv32-virt ]]; then
		riscv_memory=512M
	fi
	riscv_display=(-device ramfb)
	if [[ $boot_mode == riscv64-uboot-gop ]]; then
		riscv_display=(-device bochs-display)
	fi
	riscv_machine=(-machine virt -smp "${BOOT_SMP:-4}" -m "${BOOT_MEMORY:-$riscv_memory}"
		"${riscv_display[@]}" -display none -no-reboot
		-serial "unix:$serial,server,nowait"
		-qmp "unix:$monitor,server,nowait")
	if [[ $boot_mode == riscv64-virt || $boot_mode == riscv32-virt ]]; then
		riscv_boot=(-kernel "$image")
		if [[ -n ${INITRD:-} ]]; then
			riscv_boot+=(-initrd "$INITRD")
		fi
		if [[ -n ${BOOT_APPEND:-} ]]; then
			riscv_boot+=(-append "$BOOT_APPEND")
		fi
	else
		riscv_boot=(-kernel "$uboot"
			-drive "if=none,id=boot,file=$disk,format=qcow2"
			-device virtio-blk-device,drive=boot)
	fi
	read -r -a riscv_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" "${riscv_machine[@]}" "${riscv_boot[@]}" "${riscv_extra[@]}" \
		>"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == m68k-virt ]]; then
	# The virt machine with an MC68040 and a virtio GPU, the console the
	# kernel draws, and its terminal line on a socket for typing into.
	read -r -a m68k_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" -machine virt -cpu m68040 -m "${BOOT_MEMORY:-512M}" \
		-device virtio-gpu-device -display none -no-reboot \
		-serial "unix:$serial,server,nowait" \
		-qmp "unix:$monitor,server,nowait" \
		-kernel "$image" "${m68k_extra[@]}" >"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == mips32-malta || $boot_mode == mips64-malta ]]; then
	# The malta machine with the std VGA the kernel sets up and draws its
	# console on, and the Super I/O COM1 on a socket for typing into.
	mips_machine=(-machine malta -m "${BOOT_MEMORY:-256M}" -vga std
		-display none -no-reboot
		-serial "unix:$serial,server,nowait"
		-qmp "unix:$monitor,server,nowait")
	if [[ $boot_mode == mips64-malta ]]; then
		mips_machine+=(-cpu MIPS64R2-generic)
	fi
	mips_boot=(-kernel "$image")
	if [[ -n ${BOOT_APPEND:-} ]]; then
		mips_boot+=(-append "$BOOT_APPEND")
	fi
	read -r -a mips_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" "${mips_machine[@]}" "${mips_boot[@]}" "${mips_extra[@]}" \
		>"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == armv7-virt ]]; then
	# The virt machine with one Cortex-A15, a GICv2, a ramfb display the
	# kernel draws its console on, and the PL011 on a socket for typing
	# into.  The raw image is booted like Linux: r2 holds the device tree.
	armv7_machine=(-machine virt,gic-version=2 -cpu cortex-a15 -smp "${BOOT_SMP:-1}"
		-m "${BOOT_MEMORY:-512M}" -device ramfb -display none -no-reboot
		-serial "unix:$serial,server,nowait"
		-qmp "unix:$monitor,server,nowait")
	armv7_boot=(-kernel "$image")
	if [[ -n ${INITRD:-} ]]; then
		armv7_boot+=(-initrd "$INITRD")
	fi
	if [[ -n ${BOOT_APPEND:-} ]]; then
		armv7_boot+=(-append "$BOOT_APPEND")
	fi
	read -r -a armv7_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" "${armv7_machine[@]}" "${armv7_boot[@]}" "${armv7_extra[@]}" \
		>"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == ppc32-mac99 ]]; then
	# The mac99 machine through its firmware, with a 32-bit display the
	# kernel draws its console on, and the serial line on a socket.
	read -r -a ppc_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" -machine mac99 -m "${BOOT_MEMORY:-512M}" -g 800x600x32 \
		-display none -no-reboot \
		-serial "unix:$serial,server,nowait" \
		-qmp "unix:$monitor,server,nowait" \
		-kernel "$image" "${ppc_extra[@]}" >"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == ppc64-pseries ]]; then
	# The pseries machine through its firmware, with the VGA display the
	# kernel draws its console on, and the serial line on a socket.
	read -r -a ppc_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" -machine pseries -m "${BOOT_MEMORY:-512M}" \
		-display none -no-reboot \
		-serial "unix:$serial,server,nowait" \
		-qmp "unix:$monitor,server,nowait" \
		-kernel "$image" "${ppc_extra[@]}" >"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == sun4u ]]; then
	# OpenBIOS boots the IDE disk; the standard VGA is the machine's own.
	# QEMU_EXTRA adds options such as -g 1024x768x32 (ws275).
	read -r -a sun4u_extra <<<"${QEMU_EXTRA:-}"
	"$qemu" -machine sun4u -m "${BOOT_MEMORY:-256}" \
		-drive "if=ide,format=qcow2,file=$disk" -boot c \
		-display none -no-reboot \
		-serial "unix:$serial,server,nowait" \
		-qmp "unix:$monitor,server,nowait" "${sun4u_extra[@]}" \
		>"$output/qemu.log" 2>&1 &
elif [[ $boot_mode == raspi4b ]]; then
	"$qemu" -machine raspi4b -m 2G -kernel "$kernel" -dtb "$dtb" \
		-drive "if=sd,format=qcow2,file=$disk" \
		-display none -serial null -serial null -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
else
	"$qemu" -machine pc -m 256 -cpu max \
		-drive "if=ide,bus=0,unit=0,format=qcow2,file=$disk" \
		-vga std -display none -no-reboot \
		-qmp "unix:$monitor,server,nowait" >"$output/qemu.log" 2>&1 &
fi
pid=$!

# The screen is read, not a character stream: the login prompt is the string
# the console draws, so the picture is compared against what OCR would have to
# read.  Instead of reading pixels, this waits for the guest to stop writing to
# the stick and then photographs the screen; the login prompt is the first
# quiet point after the boot, and the picture shows whether that is what it is.
watch=(--monitor "$monitor" --screenshot "$screenshot" --frame "$frame"
	--timeout "$boot_timeout" --interval "$boot_interval")
if [[ -n ${BOOT_PATTERN:-} ]]; then
	while IFS= read -r line; do
		if [[ -n $line ]]; then
			watch+=(--pattern "$line")
		fi
	done <<<"$BOOT_PATTERN"
fi
if [[ -n ${BOOT_THEN:-} ]]; then
	watch+=(--serial "$serial")
	while IFS= read -r line; do
		case $line in
		type:*) watch+=("--then-type=${line#type:}") ;;
		keys:*) watch+=("--then-keys=${line#keys:}") ;;
		mouse:*) watch+=("--then-mouse=${line#mouse:}") ;;
		expect:*) watch+=("--then-expect=${line#expect:}") ;;
		'') ;;
		*)
			echo "boot-test: unknown BOOT_THEN step: $line" >&2
			exit 1
			;;
		esac
	done <<<"$BOOT_THEN"
fi
status=0
python3 "$root/plan/tools/boot-test.py" "${watch[@]}" || status=$?

if [[ $status -eq 0 ]]; then
	echo "boot-test: PASS $screenshot"
fi
sync
exit $status
