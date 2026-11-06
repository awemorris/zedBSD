# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TEE_SOURCES := \
	userland/base/tee/main.c

$(eval $(call USERLAND_PACKAGE,tee,tee,*,y,basic,$(TEE_SOURCES),y,base,))
