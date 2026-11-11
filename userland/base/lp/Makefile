# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LP_SOURCES := \
	userland/base/lp/main.c \
	userland/base/lp/lpd-client.c

$(eval $(call USERLAND_PACKAGE,lp,lp,*,y,basic,$(LP_SOURCES),y,base,))
