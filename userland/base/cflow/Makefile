# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CFLOW_SOURCES := \
	userland/base/cflow/main.c \
	userland/base/common/c_parser.c

$(eval $(call USERLAND_PACKAGE,cflow,cflow,*,y,basic,$(CFLOW_SOURCES),y,base,,,0755,program,,,lib/debug))
