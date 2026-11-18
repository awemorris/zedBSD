# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TAC_SOURCES := \
	userland/base/tac/main.c

$(eval $(call USERLAND_PACKAGE,tac,tac,*,y,basic,$(TAC_SOURCES),y,base,))
