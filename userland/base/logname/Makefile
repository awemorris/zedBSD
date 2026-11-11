# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LOGNAME_SOURCES := \
	userland/base/logname/main.c

$(eval $(call USERLAND_PACKAGE,logname,logname,*,y,basic,$(LOGNAME_SOURCES),y,base,))
