# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

REALPATH_SOURCES := \
	userland/base/realpath/main.c

$(eval $(call USERLAND_PACKAGE,realpath,realpath,*,y,basic,$(REALPATH_SOURCES),y,base,))
