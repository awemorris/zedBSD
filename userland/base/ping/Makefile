# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PING_SOURCES := \
	userland/base/ping/main.c

$(eval $(call USERLAND_PACKAGE,ping,ping,*,y,network,$(PING_SOURCES),y,base,,,4755))
