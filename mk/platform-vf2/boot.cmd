# zedBSD: the U-Boot boot script of the VisionFive 2's card (boot.scr).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# A U-Boot that scans the partition for boot scripts (distro boot) runs this:
# it loads zedBSD's UEFI loader and the mainline device tree named by
# fdtfile from dtb/, and hands both to bootefi.
echo zedBSD: loading from ${devtype} ${devnum}:${distro_bootpart}
if load ${devtype} ${devnum}:${distro_bootpart} ${kernel_addr_r} /EFI/BOOT/BOOTRISCV64.EFI; then
	if load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} /dtb/${fdtfile} || load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} /dtb/jh7110-starfive-visionfive-2-v1.3b.dtb; then
		bootefi ${kernel_addr_r} ${fdt_addr_r}
	fi
fi
