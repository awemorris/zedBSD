# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

GETTEXT_SOURCES := \
	userland/base/gettext/main.c

$(eval $(call USERLAND_PACKAGE,gettext,gettext,*,y,basic,$(GETTEXT_SOURCES),y,base,))
