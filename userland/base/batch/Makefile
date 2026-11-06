# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BATCH_SOURCES := \
	userland/base/at/main.c

$(eval $(call USERLAND_PACKAGE,batch,batch,*,y,basic,$(BATCH_SOURCES),y,base,))
