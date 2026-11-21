# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/rootfs.mk

ARCH_INPUTS ?= $(ROOTFS_INPUTS)
ARCH_FILES  ?= $(ROOTFS_FILES)

$(eval $(call ROOTFS_TREE_RULE,riscv64,$(ARCH_INPUTS),$(ARCH_FILES)))

.PHONY: rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
