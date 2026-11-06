# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NGETTEXT_SOURCES := \
	userland/base/gettext/main.c

$(eval $(call USERLAND_PACKAGE,ngettext,ngettext,*,y,basic,$(NGETTEXT_SOURCES),y,base,))
