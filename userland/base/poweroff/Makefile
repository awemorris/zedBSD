# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

POWEROFF_SOURCES := \
	userland/base/shutdown-control/main.c \
	userland/base/service/zsv1-protocol.c \
	userland/base/service/zsv1-client.c

$(eval $(call USERLAND_PACKAGE,poweroff,poweroff,*,y,basic,$(POWEROFF_SOURCES),y,base,,,0755,program,,,sbin))
