# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MAILX_SOURCES := \
	userland/base/deferred-stub/main.c

$(eval $(call USERLAND_PACKAGE,mailx,mailx,*,y,basic,$(MAILX_SOURCES),y,base,))

USERLAND_mailx_PROVIDER := mail
