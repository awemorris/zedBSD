# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CXREF_SOURCES := \
	userland/base/cxref/main.c \
	userland/base/common/c_parser.c

$(eval $(call USERLAND_PACKAGE,cxref,cxref,*,y,basic,$(CXREF_SOURCES),y,base,,,0755,program,,,lib/debug))
