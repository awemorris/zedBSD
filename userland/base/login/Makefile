# zedBSD userland package
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

$(eval $(call USERLAND_PACKAGE,login,login,*,y,basic,userland/base/login/main.c userland/base/login/verify.c,y,base,,))
