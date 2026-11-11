# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NICE_SOURCES := \
	userland/base/nice/main.c

$(eval $(call USERLAND_PACKAGE,nice,nice,*,y,basic,$(NICE_SOURCES),y,base,))
