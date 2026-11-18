# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PowerPC Mac (32-bit) disk image build rules
#
# The disk is Apple-partitioned so that Open Firmware boots it from USB or
# ATA: partition 2 is an HFS boot partition with the kernel (\vmunix) and
# the CHRP boot script (\ppc\bootinfo.txt, type tbxi, in the blessed
# folder), partition 3 the UFS root made from the root tree, partition 4
# swap (mk/platform-macppc32/tools/make-macppc32-image.py).
#

DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

MACPPC32_BOOT_MIB ?= 16
ROOT_MIB ?= 1024
SWAP_MIB ?= 64

$(DISK_IMAGE_ARTIFACT): $(BUILD)/vmunix $(BUILD)/rootfs/.stamp \
		mk/platform-macppc32/tools/make-macppc32-image.py \
		mk/platform-macppc32/tools/hfs.py
	$(PYTHON) mk/platform-macppc32/tools/make-macppc32-image.py \
		--kernel $(BUILD)/vmunix \
		--root-tree $(BUILD)/rootfs \
		--boot-mib $(MACPPC32_BOOT_MIB) \
		--root-mib $(ROOT_MIB) \
		--swap-mib $(SWAP_MIB) \
		--output $@

.PHONY: disk-image image-platform image-macppc32

disk-image: $(DISK_IMAGE_ARTIFACT)
image-platform: rootfs $(DISK_IMAGE_ARTIFACT)
image-macppc32: image-platform
