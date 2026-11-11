# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MOUNT_SOURCES := \
	userland/base/mount/main.c

$(eval $(call USERLAND_PACKAGE,mount,mount,*,y,core,$(MOUNT_SOURCES),n,base,,,0755,program,,,sbin))
