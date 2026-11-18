# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# fsck (BUG-312): checks and repairs a UFS volume; init runs it on a root the kernel could only mount read-only.
# Linked statically (the class static), so that it runs whatever state the root's libraries are in.  The top
# Makefile adds it to every amd64 and arm64 image.
FSCK_SOURCES := userland/base/fsck/main.c userland/base/fsck/volume.c userland/base/fsck/check.c \
	userland/base/fsck/directory.c userland/base/mkfs/ufs-super.c userland/base/mkfs/ufs-endian.c
$(eval $(call USERLAND_PACKAGE,fsck,fsck,*,y,static,$(FSCK_SOURCES),n,base,,,0755,program,,,sbin))
