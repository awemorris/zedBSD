# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CP_SOURCES := \
	userland/base/cp/main.c \
	userland/base/cp/copy.c

$(eval $(call USERLAND_PACKAGE,cp,cp,*,y,basic,$(CP_SOURCES),y,base,))
