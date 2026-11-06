# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

GETTY_SOURCES := \
	userland/base/getty/main.c

$(eval $(call USERLAND_PACKAGE,getty,getty,*,y,basic,$(GETTY_SOURCES),n,base,,,0755,program,,,sbin))
