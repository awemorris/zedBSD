# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# x86_64 disk image build rules
#

PLATFORM_X86_64_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT ?= $(abspath $(PLATFORM_X86_64_DIR)/../..)

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

# Variant: bios, uefi (alias: native), hybrid
VARIANT ?= $(if $(ZEDBSD_VARIANT),$(ZEDBSD_VARIANT),uefi)

VARIANT_STAMP := $(BUILD)/.variant-$(VARIANT)
$(VARIANT_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/.variant-*
	@touch $@

SWAP_MIB ?= 1024
BOOT_MIB ?= 256
ROOT_MIB ?= 1024
ROOT_INODES ?= 65536

SWAP_IMAGE := $(BUILD)/native-swap-$(SWAP_MIB)m.img
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct \
		--size-mib $(SWAP_MIB) --label zedBSD-swap --output $@

ifeq ($(VARIANT),bios)
ZEDBSD_CONFIG_SRC := mk/platform-x86_64/bootcfg/zedbsd-bios.cfg
else
ZEDBSD_CONFIG_SRC := mk/platform-x86_64/bootcfg/zedbsd-uefi.cfg
endif

ZEDBSD_CONFIG := $(BUILD)/zedbsd.cfg
$(ZEDBSD_CONFIG): $(ZEDBSD_CONFIG_SRC) $(VARIANT_STAMP)
	@mkdir -p $(dir $@)
	cp $< $@.tmp
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),grep -v '^video=' $< > $@.tmp)
	$(if $(filter y,$(CONFIG_BOOT_SPLASH)),printf '%s\n' logo=logo.ppm >> $@.tmp)
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),printf '%s\n' login=graphical >> $@.tmp)
	$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),printf '%s\n' kmsg=quiet >> $@.tmp)
	mv -f $@.tmp $@

ROOT_IMAGE := $(BUILD)/x86_64-native-root-$(ROOT_MIB)m.ufs
$(ROOT_IMAGE): $(BUILD)/rootfs/.stamp $(IMAGE_HOST)
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-rootfs.noct \
		--backend $(abspath $(IMAGE_HOST)) --force \
		--profile amd64 --output $@ --tree $(BUILD)/rootfs \
		--size-mib $(ROOT_MIB) --min-inodes $(ROOT_INODES)

COMMON_DEPS := $(BUILD)/vmunix $(ROOT_IMAGE) $(SWAP_IMAGE) $(ZEDBSD_CONFIG)
ifeq ($(CONFIG_BOOT_SPLASH),y)
COMMON_DEPS += $(BOOT_LOGO)
endif

ifeq ($(filter-out uefi native,$(VARIANT)),)
VARIANT_DEPS := $(BUILD)/uefi/BOOTX64.EFI
else ifeq ($(VARIANT),bios)
VARIANT_DEPS := $(BUILD)/bootloader/stage1-native.bin \
		$(BUILD)/bootloader/stage2-chain.bin \
		$(BUILD)/bootloader/partition-pbr.bin \
		$(BUILD)/bootloader/BOOTZBSD.EXE
else ifeq ($(VARIANT),hybrid)
VARIANT_DEPS := $(BUILD)/uefi/BOOTX64.EFI \
		$(BUILD)/bootloader/stage1.bin \
		$(BUILD)/bootloader/stage2-chain.bin \
		$(BUILD)/bootloader/partition-pbr.bin \
		$(BUILD)/bootloader/BOOTZBSD.EXE
endif

$(DISK_IMAGE_ARTIFACT): $(COMMON_DEPS) $(VARIANT_DEPS) $(VARIANT_STAMP) \
		mk/platform-x86_64/tools/make-x86_64-hdd-image.py \
		mk/platform-x86_64/tools/check-x86_64-hdd-image.py
	$(PYTHON) mk/platform-x86_64/tools/make-x86_64-hdd-image.py --force \
		--variant $(if $(filter native,$(VARIANT)),uefi,$(VARIANT)) \
		--kernel $(BUILD)/vmunix \
		--zedbsd-config $(ZEDBSD_CONFIG) \
		$(if $(filter y,$(CONFIG_BOOT_SPLASH)),--logo $(BOOT_LOGO)) \
		--ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		$(if $(filter uefi native hybrid,$(VARIANT)),--bootx64 $(BUILD)/uefi/BOOTX64.EFI) \
		$(if $(filter bios,$(VARIANT)),--stage1 $(BUILD)/bootloader/stage1-native.bin) \
		$(if $(filter hybrid,$(VARIANT)),--stage1 $(BUILD)/bootloader/stage1.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--stage2 $(BUILD)/bootloader/stage2-chain.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--partition-pbr $(BUILD)/bootloader/partition-pbr.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE) \
		--fat-size-mib $(BOOT_MIB) $@

.PHONY: image-platform check-disk-image
image-platform: rootfs $(DISK_IMAGE_ARTIFACT)

.PHONY: check-disk-image
check-disk-image: $(DISK_IMAGE_ARTIFACT)
	$(PYTHON) mk/platform-x86_64/tools/check-x86_64-hdd-image.py \
		--variant $(if $(filter native,$(VARIANT)),uefi,$(VARIANT)) \
		--kernel $(BUILD)/vmunix \
		--zedbsd-config $(ZEDBSD_CONFIG) \
		$(if $(filter y,$(CONFIG_BOOT_SPLASH)),--logo $(BOOT_LOGO)) \
		--ufs-root $(ROOT_IMAGE) \
		--swap-image $(SWAP_IMAGE) \
		$(if $(filter uefi native hybrid,$(VARIANT)),--bootx64 $(BUILD)/uefi/BOOTX64.EFI) \
		$(if $(filter bios,$(VARIANT)),--stage1 $(BUILD)/bootloader/stage1-native.bin) \
		$(if $(filter hybrid,$(VARIANT)),--stage1 $(BUILD)/bootloader/stage1.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--stage2 $(BUILD)/bootloader/stage2-chain.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--partition-pbr $(BUILD)/bootloader/partition-pbr.bin) \
		$(if $(filter bios hybrid,$(VARIANT)),--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE) \
		$<
