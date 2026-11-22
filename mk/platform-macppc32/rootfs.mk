# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# rootfs Makefile
#

include mk/rootfs.mk

MACPPC32_ARCH_INPUTS ?= $(ROOTFS_INPUTS)
MACPPC32_ARCH_FILES  ?= $(ROOTFS_FILES)

# The X session: startx, the session script it runs and the window
# manager's configuration, with the X server (xzed) only.
ifneq ($(filter xzed,$(CONFIG_USERLAND_PROGRAMS)),)
MACPPC32_ARCH_INPUTS += \
	userland/x11/session/startx \
	userland/x11/session/Xzedrc \
	userland/x11/session/zwm.conf
MACPPC32_ARCH_FILES += \
	--file /bin/startx=userland/x11/session/startx \
	--mode /bin/startx=0755 \
	--file /etc/Xzed/Xzedrc=userland/x11/session/Xzedrc \
	--mode /etc/Xzed/Xzedrc=0755 \
	--file /etc/Xzed/zwm.conf=userland/x11/session/zwm.conf \
	--mode /etc/Xzed/zwm.conf=0644
endif

$(eval $(call ROOTFS_TREE_RULE,ppc32,$(MACPPC32_ARCH_INPUTS),$(MACPPC32_ARCH_FILES)))

.PHONY:rootfs-platform
rootfs-platform: $(BUILD)/rootfs/.stamp
