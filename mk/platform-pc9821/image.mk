# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# PC-98 disk image build rules
#

PLATFORM_PC9821_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT ?= $(abspath $(PLATFORM_PC9821_DIR)/../..)

DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

HOST_CC ?= cc
IMAGE_HOST ?= $(BUILD)/zedimage-host

# Host image tool
$(IMAGE_HOST): mk/old-tools/build/zedimage-host.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@

BOOT_LOGO := $(BUILD)/boot-logo.ppm
$(BOOT_LOGO): mk/tools/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png
	@mkdir -p $(dir $@)
	$(PYTHON) mk/tools/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png $@.tmp
	mv -f $@.tmp $@

SWAP_MIB ?= 1024
BOOT_MIB ?= 256
ROOT_MIB ?= 1024
ROOT_INODES ?= 65536

SWAP_IMAGE := $(BUILD)/native-swap-$(SWAP_MIB)m.img
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct \
		--size-mib $(SWAP_MIB) --label zedBSD-swap --output $@

ZEDBSD_CONFIG_SRC := mk/platform-pc9821/zedbsd.cfg

ZEDBSD_CONFIG := $(BUILD)/zedbsd.cfg
$(ZEDBSD_CONFIG): $(ZEDBSD_CONFIG_SRC)
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),grep -v '^video=' $< > $@.tmp)
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),printf '%s\n' logo=logo.ppm >> $@.tmp)
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),printf '%s\n' login=graphical >> $@.tmp)
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),printf '%s\n' kmsg=quiet >> $@.tmp)
	mv -f $@.tmp $@

ROOT_IMAGE := $(BUILD)/pc9821-root-$(ROOT_MIB)m.ufs
$(ROOT_IMAGE): $(BUILD)/rootfs/.stamp $(IMAGE_HOST)
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-rootfs.noct \
		--backend $(abspath $(IMAGE_HOST)) --force \
		--profile i386 --output $@ --tree $(BUILD)/rootfs \
		--size-mib $(ROOT_MIB) --min-inodes $(ROOT_INODES)

COMMON_DEPS := $(BUILD)/vmunix $(ROOT_IMAGE) $(SWAP_IMAGE) $(ZEDBSD_CONFIG)
ifeq ($(CONFIG_BOOT_SPLASH),y)
COMMON_DEPS += $(BOOT_LOGO)
endif

BOOTLOADER_DEPS := $(BUILD)/bootloader/stage1.bin \
		   $(BUILD)/bootloader/stage2.bin \
		   $(BUILD)/bootloader/partition-pbr.bin \
		   $(BUILD)/bootloader/BOOTZBSD.EXE

$(DISK_IMAGE_ARTIFACT): $(COMMON_DEPS) $(BOOTLOADER_DEPS) \
		mk/platform-pc9821/tools/make-pc98-hdd-image.py \
		mk/platform-pc9821/tools/check-pc98-hdd-image.py
	$(PYTHON) mk/platform-pc9821/tools/make-pc98-hdd-image.py --force \
		--kernel $(BUILD)/vmunix \
		--zedbsd-config $(ZEDBSD_CONFIG) \
		$(if $(filter y,$(CONFIG_BOOT_SPLASH)),--logo $(BOOT_LOGO)) \
		--ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--fat-size-mib $(BOOT_MIB) $@

.PHONY: image-platform check-disk-image
image-platform: rootfs $(DISK_IMAGE_ARTIFACT)

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) mk/platform-pc9821/tools/check-pc98-hdd-image.py \
		--kernel $(BUILD)/vmunix \
		--zedbsd-config $(ZEDBSD_CONFIG) \
		$(if $(filter y,$(CONFIG_BOOT_SPLASH)),--logo $(BOOT_LOGO)) \
		--ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		$<
