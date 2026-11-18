# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PS_SOURCES := \
	userland/base/ps/main.c

$(eval $(call USERLAND_PACKAGE,ps,ps,*,y,basic,$(PS_SOURCES),y,base,))
