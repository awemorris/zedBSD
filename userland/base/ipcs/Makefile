# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

IPCS_SOURCES := \
	userland/base/ipcs/main.c

$(eval $(call USERLAND_PACKAGE,ipcs,ipcs,*,y,basic,$(IPCS_SOURCES),y,base,))
