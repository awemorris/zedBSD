# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PWD_SOURCES := \
	userland/base/pwd/main.c

$(eval $(call USERLAND_PACKAGE,pwd,pwd,*,y,basic,$(PWD_SOURCES),y,base,))
