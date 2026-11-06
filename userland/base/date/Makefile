# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DATE_SOURCES := \
	userland/base/date/main.c

$(eval $(call USERLAND_PACKAGE,date,date,*,y,basic,$(DATE_SOURCES),y,base,))
