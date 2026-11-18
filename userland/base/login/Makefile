# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LOGIN_SOURCES := \
	userland/base/login/main.c \
	userland/base/login/verify.c

$(eval $(call USERLAND_PACKAGE,login,login,*,y,basic,$(LOGIN_SOURCES),y,base,))
