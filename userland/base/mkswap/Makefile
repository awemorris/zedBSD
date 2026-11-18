# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MKSWAP_SOURCES := \
	userland/base/mkswap/main.c \
	userland/base/mkswap/swap-format.c \
	userland/base/mkswap/swap-device.c \
	userland/base/common/format-file.c \
	userland/base/mkswap/swap-codec.c

$(eval $(call USERLAND_PACKAGE,mkswap,mkswap,*,y,basic,$(MKSWAP_SOURCES),y,base,,,0755,program,,,sbin))
