# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

INFOCMP_SOURCES := \
	\ \
	userland/base/infocmp/main.c \
	userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,infocmp,infocmp,*,y,basic,$(INFOCMP_SOURCES),y,base,\
	base/terminfo))
