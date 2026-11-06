# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

GETCONF_SOURCES := \
	userland/base/getconf/main.c

$(eval $(call USERLAND_PACKAGE,getconf,getconf,*,y,basic,$(GETCONF_SOURCES),y,base,))
