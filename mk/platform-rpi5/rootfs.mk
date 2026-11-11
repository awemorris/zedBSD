# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Raspberry Pi 5 rootfs Makefile
#

RPI5_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
RPI5_ARCH_FILES  ?= $(ROOTFS_FILES)
AARCH64_ARCH_INPUTS ?= $(RPI5_ARCH_INPUTS)
AARCH64_ARCH_FILES  ?= $(RPI5_ARCH_FILES)

$(eval $(call ROOTFS_TREE_RULE,arm64,$(RPI5_ARCH_INPUTS),$(RPI5_ARCH_FILES)))

.PHONY: rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
