# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

HOSTNAME_SOURCES := \
	userland/base/hostname/main.c

$(eval $(call USERLAND_PACKAGE,hostname,hostname,*,y,basic,$(HOSTNAME_SOURCES),y,base,))
