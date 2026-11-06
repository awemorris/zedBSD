# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LSPCI_SOURCES := \
	userland/base/lspci/main.c

$(eval $(call USERLAND_PACKAGE,lspci,lspci,*,y,basic,$(LSPCI_SOURCES),y,base,))
