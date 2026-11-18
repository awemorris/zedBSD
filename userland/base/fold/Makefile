# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FOLD_SOURCES := \
	userland/base/fold/main.c

$(eval $(call USERLAND_PACKAGE,fold,fold,*,y,basic,$(FOLD_SOURCES),y,base,))
