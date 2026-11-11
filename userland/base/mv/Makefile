# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MV_SOURCES := \
	userland/base/mv/main.c \
	userland/base/cp/copy.c

$(eval $(call USERLAND_PACKAGE,mv,mv,*,y,basic,$(MV_SOURCES),y,base,))
