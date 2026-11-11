# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LOGGER_SOURCES := \
	userland/base/logger/main.c

$(eval $(call USERLAND_PACKAGE,logger,logger,*,y,basic,$(LOGGER_SOURCES),y,base,))
