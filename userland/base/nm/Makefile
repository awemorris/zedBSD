# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NM_SOURCES := \
	userland/base/nm/main.c \
	userland/base/common/archive.c \
	userland/base/common/elf_symbols.c

$(eval $(call USERLAND_PACKAGE,nm,nm,*,y,basic,$(NM_SOURCES),y,base,))
