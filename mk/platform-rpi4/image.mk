# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# rpi4 disk image build rules
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

# Native layout
SWAP_MIB ?= 1024
BOOT_MIB ?= 256
ROOT_MIB ?= 1024
ROOT_INODES ?= 65536

SWAP_IMAGE := $(BUILD)/native-swap-$(SWAP_MIB)m.img
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct \
		--size-mib $(SWAP_MIB) --output $@

UEFI_ZEDBSD_CONFIG := $(BUILD)/zedbsd-uefi.cfg
$(UEFI_ZEDBSD_CONFIG): mk/platform-x86_64/bootcfg/zedbsd-uefi.cfg
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

ifeq ($(VARIANT),native)

$(DISK_IMAGE_ARTIFACT): $(BUILD)/vmunix $(BUILD)/uefi/BOOTX64.EFI \
		$(UEFI_ZEDBSD_CONFIG) $(ROOT_IMAGE) \
		$(SWAP_IMAGE) $(IMAGE_HOST) $(BOOT_LOGO)
	$(IMAGE_HOST) disk --machine pcat --layout native \
		--kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config $(UEFI_ZEDBSD_CONFIG) --logo $(BOOT_LOGO) \
		--ufs-root $(ROOT_IMAGE) --swapfile $(SWAP_IMAGE) \
		--fat-size-mib $(BOOT_MIB) $@

else

DATA_IMAGE := $(BUILD)/data.img
$(DATA_IMAGE): mk/tools/make-data-image.noct $(IMAGE_HOST)
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-data-image.noct \
		--backend $(abspath $(IMAGE_HOST)) --output $@

SWAP_IMAGE := $(BUILD)/swapfile
$(SWAP_IMAGE): mk/tools/make-swapfile.noct
	@mkdir -p $(dir $@)
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-swapfile.noct --size-mib 64 --output $@

$(DISK_IMAGE_ARTIFACT): $(BUILD)/bootloader/stage1.bin \
		$(BUILD)/bootloader/stage1-native.bin \
		$(BUILD)/bootloader/stage2-chain.bin $(BUILD)/bootloader/partition-pbr.bin \
		$(BUILD)/bootloader/BOOTZBSD.EXE $(BUILD)/vmunix $(ROOT_IMAGE) \
		$(DATA_IMAGE) $(SWAP_IMAGE) $(BUILD)/uefi/BOOTX64.EFI \
		mk/platform-x86_64/bootcfg/zedbsd-bios.cfg $(IMAGE_HOST) \
		mk/tools/make-bios-hdd-image.noct \
		mk/tools/zedbuild.noct \
		mk/tools/journal-format.noct
	$(NOCT) --path=mk/tools:mk/old-tools/build mk/tools/make-bios-hdd-image.noct \
		--backend $(abspath $(IMAGE_HOST)) --force --machine pcat \
		--layout $(VARIANT) \
		--checker mk/platform-x86_64/tools/check-amd64-gpt-image.noct \
		--checker-runner $(NOCT) \
		--stage1 $(BUILD)/bootloader/stage1.bin \
		--stage2 $(BUILD)/bootloader/stage2-chain.bin \
		--partition-pbr $(BUILD)/bootloader/partition-pbr.bin \
		--bootzbsd $(BUILD)/bootloader/BOOTZBSD.EXE \
		--kernel $(BUILD)/vmunix --bootx64 $(BUILD)/uefi/BOOTX64.EFI \
		--zedbsd-config mk/platform-x86_64/bootcfg/zedbsd-bios.cfg \
		--arch-profile amd64 --arch-image $(ROOT_IMAGE) \
		--arch-format ufs --data-image $(DATA_IMAGE) \
		--swapfile $(SWAP_IMAGE) $@

endif

.PHONY: image disk-image image-x86_64 check-disk-image

image-x86_64: rootfs $(DISK_IMAGE_ARTIFACT)
