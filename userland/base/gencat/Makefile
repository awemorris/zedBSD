# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

GENCAT_SOURCES := \
	userland/base/gencat/main.c

$(eval $(call USERLAND_PACKAGE,gencat,gencat,*,y,basic,$(GENCAT_SOURCES),y,base,))
