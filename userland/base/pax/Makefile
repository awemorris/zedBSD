# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PAX_SOURCES := \
	userland/base/pax/main.c

$(eval $(call USERLAND_PACKAGE,pax,pax,*,y,basic,$(PAX_SOURCES),y,base,))
