# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

EGREP_SOURCES := \
	userland/base/egrep/main.c

$(eval $(call USERLAND_PACKAGE,egrep,egrep,*,y,basic,$(EGREP_SOURCES),y,base,))
