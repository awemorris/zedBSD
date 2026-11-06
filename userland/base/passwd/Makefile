# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# passwd (ws160-p001), set-user-ID root.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PASSWD_SOURCES := \
	userland/base/passwd/main.c \
	userland/base/common/account.c \
	userland/base/login/verify.c

$(eval $(call USERLAND_PACKAGE,passwd,passwd,*,y,basic,$(PASSWD_SOURCES),y,base,,,4755))
