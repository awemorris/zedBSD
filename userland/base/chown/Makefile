# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CHOWN_SOURCES := \
	userland/base/chown/main.c \
	userland/base/chown/change.c

$(eval $(call USERLAND_PACKAGE,chown,chown,*,y,basic,$(CHOWN_SOURCES),y,base,))
