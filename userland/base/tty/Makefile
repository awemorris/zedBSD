# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TTY_SOURCES := \
	userland/base/tty/main.c

$(eval $(call USERLAND_PACKAGE,tty,tty,*,y,basic,$(TTY_SOURCES),y,base,))
