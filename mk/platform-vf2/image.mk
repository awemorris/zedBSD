# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# VisionFive 2 (riscv64) disk image build rules
#

DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

HOST_CC ?= cc
IMAGE_HOST ?= $(BUILD)/zedimage-host

# Host image tool
$(IMAGE_HOST): mk/old-tools/build/zedimage-host.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@

ROOT_MIB ?= 1024
ROOT_INODES ?= 65536

ROOT_IMAGE := $(BUILD)/ufs-root.img
$(ROOT_IMAGE): $(BUILD)/rootfs/.stamp $(IMAGE_HOST)
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-rootfs.noct \
		--backend $(abspath $(IMAGE_HOST)) --force \
		--profile riscv64 --output $@ --tree $(BUILD)/rootfs \
		--size-mib $(ROOT_MIB) --min-inodes $(ROOT_INODES)

SWAP_MIB ?= 64
SWAP_IMAGE := $(BUILD)/swap.img
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct \
		--size-mib $(SWAP_MIB) --output $@

# The boot files follow the configuration: they are written every time and
# replaced only when their contents change.
.PHONY: FORCE_BOOT_FILES
FORCE_BOOT_FILES:

$(BUILD)/zedbsd.cfg: FORCE_BOOT_FILES
	@mkdir -p $(dir $@)
	@printf '%s\n' 'kernel=vmunix' 'rootpart=PARTLABEL=zedBSD-root' > $@.tmp
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),@printf '%s\n' 'login=graphical' >> $@.tmp)
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

#
# The files the vendor U-Boot reads from the EFI system partition (partition
# 3).  StarFive's U-Boot 2021.10 imports vf2_uEnv.txt and runs boot2, and its
# distro step imports uEnv.txt and runs distro_bootenv_nvme; both load the
# loader and a device tree from dtb/.  boot.scr serves a U-Boot that scans for
# boot scripts.
#
VF2_ESP_FILES := \
	$(foreach dtb,$(VF2_DTBS),dtb/$(notdir $(dtb))=$(dtb) dtb/starfive/$(notdir $(dtb))=$(dtb)) \
	dtb/LICENSE=mk/platform-vf2/dtb.LICENSE \
	vf2_uEnv.txt=mk/platform-vf2/uEnv.txt \
	uEnv.txt=mk/platform-vf2/uEnv.txt \
	boot.scr=$(BUILD)/boot.scr

VF2_ESP_INPUTS := $(VF2_DTBS) mk/platform-vf2/dtb.LICENSE mk/platform-vf2/uEnv.txt $(BUILD)/boot.scr

$(DISK_IMAGE_ARTIFACT): $(BUILD)/uefi/BOOTRISCV64.EFI $(BUILD)/vmunix $(BUILD)/zedbsd.cfg \
		$(VF2_ESP_INPUTS) \
		$(ROOT_IMAGE) $(SWAP_IMAGE) \
		mk/platform-vf2/tools/make-vf2-hdd-image.py \
		mk/platform-vf2/tools/check-vf2-hdd-image.py
	$(PYTHON) mk/platform-vf2/tools/make-vf2-hdd-image.py --force \
		--efi $(BUILD)/uefi/BOOTRISCV64.EFI \
		--kernel $(BUILD)/vmunix \
		--config $(BUILD)/zedbsd.cfg \
		--ufs-root $(ROOT_IMAGE) \
		--swap $(SWAP_IMAGE) \
		$(addprefix --esp-file ,$(VF2_ESP_FILES)) \
		$@

.PHONY: disk-image check-disk-image image-platform image-vf2

disk-image: $(DISK_IMAGE_ARTIFACT)
image-platform: rootfs $(DISK_IMAGE_ARTIFACT)
image-vf2: image-platform

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) mk/platform-vf2/tools/check-vf2-hdd-image.py \
		--efi $(BUILD)/uefi/BOOTRISCV64.EFI \
		--kernel $(BUILD)/vmunix \
		--config $(BUILD)/zedbsd.cfg \
		--ufs-root $(ROOT_IMAGE) \
		--swap $(SWAP_IMAGE) \
		$<
