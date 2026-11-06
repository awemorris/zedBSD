# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UNIQ_SOURCES := \
	userland/base/uniq/main.c

$(eval $(call USERLAND_PACKAGE,uniq,uniq,*,y,basic,$(UNIQ_SOURCES),y,base,))
