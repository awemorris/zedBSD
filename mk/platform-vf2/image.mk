# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# vf2 disk image build rules
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
		--profile aarch64 --output $@ --tree $(BUILD)/rootfs \
		--size-mib $(ROOT_MIB) --min-inodes $(ROOT_INODES)

SWAP_MIB ?= 64
SWAP_IMAGE := $(BUILD)/swap.img
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct \
		--size-mib $(SWAP_MIB) --output $@

$(DISK_IMAGE_ARTIFACT): $(BUILD)/vmunix $(ROOT_IMAGE) \
		$(SWAP_IMAGE) $(BUILD)/config.txt $(BUILD)/cmdline.txt \
		$(VF2_BOOT_FIRMWARE_CACHE_FILES) \
		mk/platform-vf2/tools/make-vf2-ufs-root-hdd-image.py \
		mk/platform-vf2/tools/make-vf2-hdd-image.py \
		mk/platform-vf2/tools/check-vf2-hdd-image.py
	$(PYTHON) mk/platform-vf2/tools/make-vf2-ufs-root-hdd-image.py --force \
		--kernel $(BUILD)/vmunix --ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		--config $(BUILD)/config.txt --cmdline $(BUILD)/cmdline.txt \
		--board vf2 --firmware-dir $(VF2_BOOT_FIRMWARE_DIR) $@

.PHONY: disk-image check-disk-image image-vf2

disk-image: $(DISK_IMAGE_ARTIFACT)
image-platform: rootfs $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) mk/platform-vf2/tools/check-vf2-hdd-image.py \
		--kernel $(BUILD)/vmunix --ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		--config $(BUILD)/config.txt --cmdline $(BUILD)/cmdline.txt \
		--board vf2 $<
