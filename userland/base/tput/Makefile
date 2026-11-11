# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TPUT_SOURCES := \
	userland/base/tput/main.c \
	userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,tput,tput,*,y,basic,$(TPUT_SOURCES),y,base,base/terminfo))
