# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ACCOUNT_ADMIN_SOURCES := \
	main.c \
	table.c

$(eval $(call USERLAND_PACKAGE,diskpart,diskpart,*,y,basic,userland/base/diskpart/main.c userland/base/diskpart/table.c,y,base,,,0755,program,,,sbin))
