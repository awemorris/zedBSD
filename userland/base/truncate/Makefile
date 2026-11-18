# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TRUNCATE_SOURCES := \
	userland/base/truncate/main.c

$(eval $(call USERLAND_PACKAGE,truncate,truncate,*,y,basic,$(TRUNCATE_SOURCES),y,base,))
