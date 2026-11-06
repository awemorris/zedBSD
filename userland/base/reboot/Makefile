# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

REBOOT_SOURCES := \
	userland/base/shutdown-control/main.c \
	userland/base/service/zsv1-protocol.c \
	userland/base/service/zsv1-client.c

$(eval $(call USERLAND_PACKAGE,reboot,reboot,*,y,basic,$(REBOOT_SOURCES),y,base,,,0755,program,,,sbin))
