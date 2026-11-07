# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Vision Five 2 (RISC-V 64) rootfs Makefile
#

ARCH_INPUTS ?= $(ROOTFS_INPUTS)
ARCH_FILES  ?= $(ROOTFS_FILES)

$(eval $(call ROOTFS_TREE_RULE,riscv64,$(ARCH_INPUTS),$(ARCH_FILES)))

.PHONY: rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
