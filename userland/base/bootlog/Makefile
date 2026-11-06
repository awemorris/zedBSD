# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# bootlog keeps each boot's kernel log on the boot FAT.  It is not a
# default: the Raspberry Pi 4 configuration selects it, and rc.conf
# names the service as optional, so an image without it says nothing.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BOOTLOG_SOURCES := \
	userland/base/bootlog/main.c \
	userland/base/bootlog/log-file.c

BOOTLOG_DATA := /etc/service.d/bootlog=userland/base/bootlog/service

$(eval $(call USERLAND_PACKAGE,bootlog,bootlog,*,n,basic,$(BOOTLOG_SOURCES),y,base,,,0755,program,$(BOOTLOG_DATA),,sbin))
