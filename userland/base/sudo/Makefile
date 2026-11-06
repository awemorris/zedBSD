# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# sudo (ws160-p001), set-user-ID root.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SUDO_SOURCES := \
	userland/base/sudo/main.c \
	userland/base/common/account.c \
	userland/base/login/verify.c

$(eval $(call USERLAND_PACKAGE,sudo,sudo,*,y,basic,$(SUDO_SOURCES),y,base,,,4755))
