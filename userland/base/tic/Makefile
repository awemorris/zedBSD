# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TIC_SOURCES := \
	userland/base/tic/main.c \
	userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,tic,tic,*,y,basic,$(TIC_SOURCES),y,base,\
	base/terminfo))
