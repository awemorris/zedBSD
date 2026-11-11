# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

KILL_SOURCES := \
	userland/base/kill/main.c

$(eval $(call USERLAND_PACKAGE,kill,kill,*,y,basic,$(KILL_SOURCES),y,base,))
