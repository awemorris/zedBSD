# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SYSLOGD_SOURCES := \
	userland/base/syslogd/main.c

$(eval $(call USERLAND_PACKAGE,syslogd,syslogd,*,y,basic,$(SYSLOGD_SOURCES),n,base,,,0755,program,,,sbin))
