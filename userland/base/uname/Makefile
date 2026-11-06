# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UNAME_SOURCES := \
	userland/base/uname/main.c

$(eval $(call USERLAND_PACKAGE,uname,uname,*,y,basic,$(UNAME_SOURCES),y,base,))
