# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LESS_SOURCES := \
	userland/base/less/main.c

$(eval $(call USERLAND_PACKAGE,less,less,*,y,basic,$(LESS_SOURCES),y,base,))
