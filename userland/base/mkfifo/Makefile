# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MKFIFO_SOURCES := \
	userland/base/mkfifo/main.c \
	userland/base/chmod/mode.c

$(eval $(call USERLAND_PACKAGE,mkfifo,mkfifo,*,y,basic,$(MKFIFO_SOURCES),y,base,))
