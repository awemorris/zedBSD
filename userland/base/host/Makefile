# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

HOST_SOURCES := \
	userland/base/host/main.c

$(eval $(call USERLAND_PACKAGE,host,host,*,y,network,$(HOST_SOURCES),y,base,))
