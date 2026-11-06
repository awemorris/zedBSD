# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UNEXPAND_SOURCES := \
	userland/base/unexpand/main.c \
	userland/base/expand/tabs.c

$(eval $(call USERLAND_PACKAGE,unexpand,unexpand,*,y,basic,$(UNEXPAND_SOURCES),y,base,))
