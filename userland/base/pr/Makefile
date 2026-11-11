# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PR_SOURCES := \
	userland/base/pr/main.c

$(eval $(call USERLAND_PACKAGE,pr,pr,*,y,basic,$(PR_SOURCES),y,base,))
