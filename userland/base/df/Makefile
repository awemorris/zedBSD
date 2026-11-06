# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DF_SOURCES := \
	userland/base/df/main.c

$(eval $(call USERLAND_PACKAGE,df,df,*,y,basic,$(DF_SOURCES),y,base,))
