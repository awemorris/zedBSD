# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FIND_SOURCES := \
	userland/base/find/main.c \
	userland/base/chmod/mode.c

$(eval $(call USERLAND_PACKAGE,find,find,*,y,basic,$(FIND_SOURCES),y,base,))
