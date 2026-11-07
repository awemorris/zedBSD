# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Raspberry Pi 5 bootloader / boot partition files
#

PLATFORM_RPI5_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT         ?= $(abspath $(PLATFORM_RPI5_DIR)/../..)

RPI5_BOOT_COMMAND_LINE := login=$(if $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),graphical,console)$(if $(or $(filter y,$(CONFIG_GRAPHICAL_LOGIN)),$(filter n,$(CONFIG_BOOT_KERNEL_MESSAGES))), kmsg=quiet)

$(BUILD)/cmdline.txt:
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(RPI5_BOOT_COMMAND_LINE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

RPI5_FIRMWARE_FILES := $(RPI5_BOOT_FIRMWARE_CACHE_FILES) \
	$(BUILD)/bcm2712-rpi-5-b.dtb \
	$(BUILD)/bcm2712d0-rpi-5-b.dtb \
	$(BUILD)/config.txt \
	$(BUILD)/cmdline.txt

$(BUILD)/config.txt: mk/platform-rpi5/config.txt
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/bcm2712-rpi-5-b.dtb: $(RPI5_BOOT_FIRMWARE_DTB)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/bcm2712d0-rpi-5-b.dtb: $(RPI5_BOOT_FIRMWARE_D0_DTB)
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: bootloader bootloader-rpi5 bootloader-platform
bootloader-platform: $(RPI5_FIRMWARE_FILES)
bootloader-rpi5: bootloader-platform
