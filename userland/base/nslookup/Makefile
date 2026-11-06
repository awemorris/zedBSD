# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NSLOOKUP_SOURCES := \
	userland/base/nslookup/main.c

$(eval $(call USERLAND_PACKAGE,nslookup,nslookup,*,y,network,$(NSLOOKUP_SOURCES),y,base,))
