# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ADMIN_SOURCES := \
	userland/base/sccs/main.c \
	userland/base/common/sccs.c

$(eval $(call USERLAND_PACKAGE,admin,admin,*,y,basic,$(ADMIN_SOURCES),y,base,))
