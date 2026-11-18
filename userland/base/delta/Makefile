# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DELTA_SOURCES := \
	userland/base/sccs/main.c \
	userland/base/common/sccs.c

$(eval $(call USERLAND_PACKAGE,delta,delta,*,y,basic,$(DELTA_SOURCES),y,base,))
