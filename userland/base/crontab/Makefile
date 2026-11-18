# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CRONTAB_SOURCES := \
	userland/base/crontab/main.c

$(eval $(call USERLAND_PACKAGE,crontab,crontab,*,y,basic,$(CRONTAB_SOURCES),y,base,))
