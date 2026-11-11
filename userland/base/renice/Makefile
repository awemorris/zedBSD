# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

RENICE_SOURCES := \
	userland/base/renice/main.c

$(eval $(call USERLAND_PACKAGE,renice,renice,*,y,basic,$(RENICE_SOURCES),y,base,))
