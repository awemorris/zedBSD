# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BT_SOURCES := \
	userland/base/bt/main.c

$(eval $(call USERLAND_PACKAGE,bt,bt,*,n,basic,$(BT_SOURCES),y,base,base/bt,,0755,program,,,bin))
