# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SLEEP_SOURCES := \
	userland/base/sleep/main.c

$(eval $(call USERLAND_PACKAGE,sleep,sleep,*,y,basic,$(SLEEP_SOURCES),y,base,))
