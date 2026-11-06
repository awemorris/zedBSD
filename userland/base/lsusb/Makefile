# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LSUSB_SOURCES := \
	userland/base/lsusb/main.c

$(eval $(call USERLAND_PACKAGE,lsusb,lsusb,*,y,basic,$(LSUSB_SOURCES),y,base,))
