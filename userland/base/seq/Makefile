# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SEQ_SOURCES := \
	userland/base/seq/main.c

$(eval $(call USERLAND_PACKAGE,seq,seq,*,y,basic,$(SEQ_SOURCES),y,base,))
