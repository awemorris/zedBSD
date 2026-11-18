# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DMESG_SOURCES := \
	userland/base/dmesg/main.c

$(eval $(call USERLAND_PACKAGE,dmesg,dmesg,*,y,basic,$(DMESG_SOURCES),y,base,))
