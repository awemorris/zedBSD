# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

AR_SOURCES := \
	userland/base/ar/main.c \
	userland/base/common/archive.c \
	userland/base/common/elf_symbols.c

$(eval $(call USERLAND_PACKAGE,ar,ar,*,y,basic,$(AR_SOURCES),y,base,))
