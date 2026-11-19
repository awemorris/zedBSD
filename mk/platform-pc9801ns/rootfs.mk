# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# pc9801ns rootfs Makefile
#

PC9801NS_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
PC9801NS_ARCH_FILES  ?= $(ROOTFS_FILES)

$(eval $(call ROOTFS_TREE_RULE,pc9821,$(PC9801NS_ARCH_INPUTS),$(PC9801NS_ARCH_FILES)))

.PHONY:rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
