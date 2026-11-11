# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TIMEOUT_SOURCES := \
	userland/base/timeout/main.c

$(eval $(call USERLAND_PACKAGE,timeout,timeout,*,y,basic,$(TIMEOUT_SOURCES),y,base,))
