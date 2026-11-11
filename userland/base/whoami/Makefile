# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WHOAMI_SOURCES := \
	userland/base/whoami/main.c

$(eval $(call USERLAND_PACKAGE,whoami,whoami,*,y,basic,$(WHOAMI_SOURCES),y,base,))
