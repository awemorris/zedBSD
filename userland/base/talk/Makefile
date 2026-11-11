# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TALK_SOURCES := \
	userland/base/deferred-stub/main.c

$(eval $(call USERLAND_PACKAGE,talk,talk,*,y,basic,$(TALK_SOURCES),y,base,))

USERLAND_talk_PROVIDER := talk
