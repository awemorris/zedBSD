# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

PLATFORM_PC9801NS_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT ?= $(abspath $(PLATFORM_PC9801NS_DIR)/../..)

DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

HOST_CC ?= cc
IMAGE_HOST ?= $(BUILD)/zedimage-host

# Host image tool
$(IMAGE_HOST): mk/tools/zedimage-host.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@

BOOT_LOGO := $(BUILD)/boot-logo.ppm
$(BOOT_LOGO): mk/tools/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png
	@mkdir -p $(dir $@)
	$(PYTHON) mk/tools/make-boot-splash.py userland/desktop/artwork/zedbsd-boot-splash.png $@.tmp
	mv -f $@.tmp $@

# The swap partition holds the swap image exactly, and a PC-98 partition is
# whole cylinders (4 heads x 17 sectors = 68 sectors), so the size in MiB
# is a multiple of 17 (34816 sectors, 512 cylinders).
SWAP_MIB ?= 17
BOOT_MIB ?= 17
ROOT_MIB ?= 34
ROOT_INODES ?= 16384

SWAP_IMAGE := $(BUILD)/native-swap-$(SWAP_MIB)m.img
$(SWAP_IMAGE): mk/tools/make-swapfile.py
	@mkdir -p $(dir $@)
	$(PYTHON) mk/tools/make-swapfile.py --size-mib $(SWAP_MIB) --label zedBSD-swap --output $@

ZEDBSD_CONFIG_SRC := mk/platform-pc9801ns/zedbsd.cfg

BOOT_CONFIG_STAMP := $(BUILD)/.boot-config-splash_$(CONFIG_BOOT_SPLASH)-login_$(CONFIG_GRAPHICAL_LOGIN)-quiet_$(CONFIG_QUIET_BOOT)
$(BOOT_CONFIG_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/.boot-config-*
	@touch $@

ZEDBSD_CONFIG := $(BUILD)/zedbsd.cfg
$(ZEDBSD_CONFIG): $(ZEDBSD_CONFIG_SRC) $(BOOT_CONFIG_STAMP) $(PLATFORM_PC9801NS_DIR)/image.mk
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),grep -v '^video=' $< > $@.tmp)
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),printf '%s\n' logo=logo.ppm >> $@.tmp)
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),printf '%s\n' login=graphical >> $@.tmp)
	$(if $(filter y,$(CONFIG_QUIET_BOOT)),printf '%s\n' kmsg=quiet >> $@.tmp)
	mv -f $@.tmp $@

ROOT_IMAGE := $(BUILD)/pc9801ns-root-$(ROOT_MIB)m.ufs
$(ROOT_IMAGE): $(BUILD)/rootfs/.stamp $(IMAGE_HOST)
	@mkdir -p $(dir $@)
	$(PYTHON) mk/tools/make-rootfs.py --backend $(abspath $(IMAGE_HOST)) --force --profile i386 --output $@ --tree $(BUILD)/rootfs --size-mib $(ROOT_MIB) --min-inodes $(ROOT_INODES)

COMMON_DEPS := $(BUILD)/vmunix $(ROOT_IMAGE) $(SWAP_IMAGE) $(ZEDBSD_CONFIG)
ifeq ($(CONFIG_BOOT_SPLASH),y)
COMMON_DEPS += $(BOOT_LOGO)
endif

BOOTLOADER_DEPS := $(BUILD)/bootloader/stage1.bin \
		   $(BUILD)/bootloader/stage2.bin \
		   $(BUILD)/bootloader/partition-pbr.bin \
		   $(BUILD)/bootloader/BOOTZBSD.EXE

$(DISK_IMAGE_ARTIFACT): $(COMMON_DEPS) $(BOOTLOADER_DEPS) $(BOOT_CONFIG_STAMP) mk/tools/make-pc98-hdd-image.py mk/tools/check-pc98-hdd-image.py
	$(PYTHON) mk/tools/make-pc98-hdd-image.py --force --heads 4 --kernel $(BUILD)/vmunix --zedbsd-config $(ZEDBSD_CONFIG) --ufs-root $(ROOT_IMAGE) --swap-image $(SWAP_IMAGE) --stage1 $(BUILD)/bootloader/stage1.bin --stage2 $(BUILD)/bootloader/stage2.bin --partition-pbr $(BUILD)/bootloader/partition-pbr.bin --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE --fat-size-mib $(BOOT_MIB) $@

.PHONY: image-platform check-disk-image
image-platform: rootfs-platform $(DISK_IMAGE_ARTIFACT)
	@echo 'hdd-image.img completed.'

check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) mk/tools/check-pc98-hdd-image.py --heads 4 --kernel $(BUILD)/vmunix --zedbsd-config $(ZEDBSD_CONFIG) $(if $(filter y,$(CONFIG_BOOT_SPLASH)),--logo $(BOOT_LOGO)) --ufs-root $(ROOT_IMAGE) --swap-image $(SWAP_IMAGE) --stage1 $(BUILD)/bootloader/stage1.bin --stage2 $(BUILD)/bootloader/stage2.bin --partition-pbr $(BUILD)/bootloader/partition-pbr.bin --bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE $<
