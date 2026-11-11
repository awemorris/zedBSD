# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TAIL_SOURCES := \
	userland/base/tail/main.c

$(eval $(call USERLAND_PACKAGE,tail,tail,*,y,basic,$(TAIL_SOURCES),y,base,))
