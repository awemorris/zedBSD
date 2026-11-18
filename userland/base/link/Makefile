# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LINK_SOURCES := \
	userland/base/link/main.c

$(eval $(call USERLAND_PACKAGE,link,link,*,y,basic,$(LINK_SOURCES),y,base,))
