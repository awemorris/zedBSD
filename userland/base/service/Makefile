# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SERVICE_SOURCES := \
	userland/base/service/main.c \
	userland/base/service/service-command.c \
	userland/base/service/service-console.c \
	userland/base/service/service-config.c \
	userland/base/service/rcconf.c \
	userland/base/service/zsv1-protocol.c \
	userland/base/service/zsv1-client.c

$(eval $(call USERLAND_PACKAGE,service,service,*,y,basic,$(SERVICE_SOURCES),y,base,,,0755,program,,,sbin))
