# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DU_SOURCES := \
	userland/base/du/main.c

$(eval $(call USERLAND_PACKAGE,du,du,*,y,basic,$(DU_SOURCES),y,base,))
