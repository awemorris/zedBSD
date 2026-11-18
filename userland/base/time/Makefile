# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TIME_SOURCES := \
	userland/base/time/main.c

$(eval $(call USERLAND_PACKAGE,time,time,*,y,basic,$(TIME_SOURCES),y,base,))
