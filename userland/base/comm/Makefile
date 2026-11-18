# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

COMM_SOURCES := \
	userland/base/comm/main.c

$(eval $(call USERLAND_PACKAGE,comm,comm,*,y,basic,$(COMM_SOURCES),y,base,))
