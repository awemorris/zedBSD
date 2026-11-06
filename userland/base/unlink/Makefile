# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UNLINK_SOURCES := \
	userland/base/unlink/main.c

$(eval $(call USERLAND_PACKAGE,unlink,unlink,*,y,basic,$(UNLINK_SOURCES),y,base,))
