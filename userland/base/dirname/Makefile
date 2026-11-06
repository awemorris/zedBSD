# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DIRNAME_SOURCES := \
	userland/base/dirname/main.c

$(eval $(call USERLAND_PACKAGE,dirname,dirname,*,y,basic,$(DIRNAME_SOURCES),y,base,))
