# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WC_SOURCES := \
	userland/base/wc/main.c

$(eval $(call USERLAND_PACKAGE,wc,wc,*,y,basic,$(WC_SOURCES),y,base,))
