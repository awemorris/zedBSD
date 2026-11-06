# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NTPDATE_SOURCES := \
	userland/base/ntpdate/main.c \
	userland/base/service/service-config.c \
	userland/base/service/rcconf.c

$(eval $(call USERLAND_PACKAGE,ntpdate,ntpdate,*,y,basic,$(NTPDATE_SOURCES),y,base,,,0755,program,,,sbin))
