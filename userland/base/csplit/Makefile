# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CSPLIT_SOURCES := \
	userland/base/csplit/main.c

$(eval $(call USERLAND_PACKAGE,csplit,csplit,*,y,basic,$(CSPLIT_SOURCES),y,base,))
