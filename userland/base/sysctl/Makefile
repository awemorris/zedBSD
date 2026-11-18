# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SYSCTL_SOURCES := \
	userland/base/sysctl/main.c

$(eval $(call USERLAND_PACKAGE,sysctl,sysctl,*,y,core,$(SYSCTL_SOURCES),n,base,,,0755,program,,,sbin))
