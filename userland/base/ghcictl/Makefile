# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Lists the GPUs of the generic GHCI driver, their capabilities and display modes.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

GHCICTL_SOURCES := \
	userland/base/ghcictl/main.c

$(eval $(call USERLAND_PACKAGE,ghcictl,GHCI GPU status tool,*,n,basic,$(GHCICTL_SOURCES),y,base,))
