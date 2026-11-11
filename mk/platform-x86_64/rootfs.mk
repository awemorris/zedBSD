# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# x86_64 rootfs Makefile
#

X86_64_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
X86_64_ARCH_FILES  ?= $(ROOTFS_FILES)
AMD64_ARCH_INPUTS  ?= $(X86_64_ARCH_INPUTS)
AMD64_ARCH_FILES   ?= $(X86_64_ARCH_FILES)

$(eval $(call ROOTFS_TREE_RULE,x86_64,$(X86_64_ARCH_INPUTS),$(X86_64_ARCH_FILES)))

.PHONY:rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
