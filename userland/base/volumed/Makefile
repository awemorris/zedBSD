# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The removable media daemon (ws132-p004, plan/ws132/phase004/phase.md): lists the removable FAT and UFS
# volumes on /run/volumed.sock and mounts one under /media when the seat's user asks (never by itself).
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

VOLUMED_SOURCES := userland/base/volumed/main.c userland/base/volumed/names.c
VOLUMED_DATA := /etc/service.d/volumed=userland/base/volumed/volumed.service

$(eval $(call USERLAND_PACKAGE,volumed,volumed,*,n,basic,$(VOLUMED_SOURCES),y,base,base/volumed,,0755,program,$(VOLUMED_DATA),,sbin))
