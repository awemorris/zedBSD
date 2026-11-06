# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

INSTALL_SOURCES := \
	userland/base/install/main.c \
	userland/base/chmod/mode.c

$(eval $(call USERLAND_PACKAGE,install,install,*,y,basic,$(INSTALL_SOURCES),y,base,))
