# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

EFIBOOT_SOURCES := \
	userland/base/efiboot/main.c \
	userland/base/diskpart/table.c

$(eval $(call USERLAND_PACKAGE,efiboot,efiboot,*,y,basic,userland/base/efiboot/main.c userland/base/diskpart/table.c,y,base,,,0755,program,,,sbin))
