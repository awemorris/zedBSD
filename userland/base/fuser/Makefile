# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FUSER_SOURCES := \
	userland/base/fuser/main.c

$(eval $(call USERLAND_PACKAGE,fuser,fuser,*,y,basic,$(FUSER_SOURCES),y,base,))
