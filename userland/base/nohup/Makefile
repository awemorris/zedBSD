# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NOHUP_SOURCES := \
	userland/base/nohup/main.c

$(eval $(call USERLAND_PACKAGE,nohup,nohup,*,y,basic,$(NOHUP_SOURCES),y,base,))
