# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DD_SOURCES := \
	userland/base/dd/main.c \
	userland/base/dd/tables.c

$(eval $(call USERLAND_PACKAGE,dd,dd,*,y,basic,$(DD_SOURCES),y,base,))
