# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CKSUM_SOURCES := \
	userland/base/cksum/main.c \
	userland/base/common/sha256.c

$(eval $(call USERLAND_PACKAGE,cksum,cksum,*,y,basic,$(CKSUM_SOURCES),y,base,))
