# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CRON_SOURCES := \
	userland/base/cron/main.c

$(eval $(call USERLAND_PACKAGE,cron,cron,*,y,basic,$(CRON_SOURCES),n,base,,,0755,program,,,sbin))
