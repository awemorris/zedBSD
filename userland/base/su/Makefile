# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# su (ws160-p001), set-user-ID root.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SU_SOURCES := \
	userland/base/su/main.c \
	userland/base/common/account.c \
	userland/base/login/verify.c

$(eval $(call USERLAND_PACKAGE,su,su,*,y,basic,$(SU_SOURCES),y,base,,,4755))
