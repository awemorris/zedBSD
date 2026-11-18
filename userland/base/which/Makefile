# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WHICH_SOURCES := \
	userland/base/which/main.c

$(eval $(call USERLAND_PACKAGE,which,which,*,y,basic,$(WHICH_SOURCES),y,base,))
