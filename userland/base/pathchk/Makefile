# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PATHCHK_SOURCES := \
	userland/base/pathchk/main.c

$(eval $(call USERLAND_PACKAGE,pathchk,pathchk,*,y,basic,$(PATHCHK_SOURCES),y,base,))
