# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

IPCRM_SOURCES := \
	userland/base/ipcrm/main.c

$(eval $(call USERLAND_PACKAGE,ipcrm,ipcrm,*,y,basic,$(IPCRM_SOURCES),y,base,))
