include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,infocmp,infocmp,*,y,basic,\
	userland/base/infocmp/main.c userland/base/common/terminfo.c,y,base,\
	base/terminfo,))
