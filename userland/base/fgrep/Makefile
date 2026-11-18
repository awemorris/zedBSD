# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FGREP_SOURCES := \
	userland/base/fgrep/main.c

$(eval $(call USERLAND_PACKAGE,fgrep,fgrep,*,y,basic,$(FGREP_SOURCES),y,base,))
