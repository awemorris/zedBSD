# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

EXPR_SOURCES := \
	userland/base/expr/main.c

$(eval $(call USERLAND_PACKAGE,expr,expr,*,y,basic,$(EXPR_SOURCES),y,base,))
