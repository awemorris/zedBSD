# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ED_SOURCES := \
	userland/base/ed/main.c \
	userland/base/ed/buffer.c

$(eval $(call USERLAND_PACKAGE,ed,ed,*,y,basic,$(ED_SOURCES),y,base,,))
