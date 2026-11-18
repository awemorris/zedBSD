# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MSGFMT_SOURCES := \
	userland/base/msgfmt/main.c

$(eval $(call USERLAND_PACKAGE,msgfmt,msgfmt,*,y,basic,$(MSGFMT_SOURCES),y,base,))
