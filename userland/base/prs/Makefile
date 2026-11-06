# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PRS_SOURCES := \
	userland/base/sccs/main.c \
	userland/base/common/sccs.c

$(eval $(call USERLAND_PACKAGE,prs,prs,*,y,basic,$(PRS_SOURCES),y,base,))
