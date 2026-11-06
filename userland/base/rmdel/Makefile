# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

RMDEL_SOURCES := \
	userland/base/sccs/main.c \
	userland/base/common/sccs.c

$(eval $(call USERLAND_PACKAGE,rmdel,rmdel,*,y,basic,$(RMDEL_SOURCES),y,base,))
