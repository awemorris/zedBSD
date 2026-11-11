# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MESG_SOURCES := \
	userland/base/mesg/main.c

$(eval $(call USERLAND_PACKAGE,mesg,mesg,*,y,basic,$(MESG_SOURCES),y,base,))
