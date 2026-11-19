# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# pc9821 rootfs Makefile
#

PC9821_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
PC9821_ARCH_FILES  ?= $(ROOTFS_FILES)

$(eval $(call ROOTFS_TREE_RULE,pc9821,$(PC9821_ARCH_INPUTS),$(PC9821_ARCH_FILES)))

.PHONY:rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
