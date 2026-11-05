# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

$(eval $(call USERLAND_PACKAGE,mkswap,mkswap,*,y,basic,userland/base/mkswap/main.c userland/base/mkswap/swap-format.c userland/base/mkswap/swap-device.c userland/base/common/format-file.c userland/base/mkswap/swap-codec.c,y,base,,,0755,program,,,sbin))
