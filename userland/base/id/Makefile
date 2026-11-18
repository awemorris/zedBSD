# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ID_SOURCES := \
	userland/base/id/main.c

$(eval $(call USERLAND_PACKAGE,id,id,*,y,basic,$(ID_SOURCES),y,base,))
