# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SCCS_SOURCES := \
	userland/base/sccs/main.c \
	userland/base/common/sccs.c

$(eval $(call USERLAND_PACKAGE,sccs,sccs,*,y,basic,$(SCCS_SOURCES),y,base,))
