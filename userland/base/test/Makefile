include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_TEST_SOURCES := userland/base/test/main.c userland/base/sh/test.c \
	userland/base/common/builtin-standalone.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,test,test,*,y,basic,$(ZEDBSD_TEST_SOURCES),y,base,,))
