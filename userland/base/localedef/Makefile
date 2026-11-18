# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LOCALEDEF_SOURCES := \
	userland/base/localedef/main.c

$(eval $(call USERLAND_PACKAGE,localedef,localedef,*,y,basic,$(LOCALEDEF_SOURCES),y,base,))
