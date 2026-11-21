# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include mk/rootfs.mk

I386_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
I386_ARCH_FILES  ?= $(ROOTFS_FILES)

$(eval $(call ROOTFS_TREE_RULE,i386,$(I386_ARCH_INPUTS),$(I386_ARCH_FILES)))

.PHONY:rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
