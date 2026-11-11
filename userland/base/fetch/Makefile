# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FETCH_SOURCES := \
	userland/base/fetch/main.c

$(eval $(call USERLAND_PACKAGE,fetch,fetch,*,y,network,$(FETCH_SOURCES),y,base,))
