# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MKFS_SOURCES := \
	userland/base/mkfs/main.c \
	userland/base/mkfs/ufs-format.c \
	userland/base/mkfs/fat32-format.c \
	userland/base/mkfs/block-command.c \
	userland/base/common/format-file.c \
	userland/base/mkfs/ufs-super.c \
	userland/base/mkfs/ufs-endian.c

$(eval $(call USERLAND_PACKAGE,mkfs,mkfs,*,y,basic,$(MKFS_SOURCES),y,base,,,0755,program,,,sbin))
