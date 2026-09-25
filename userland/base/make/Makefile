include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Installed as /usr/bin/make.
ZEDBSD_MAKE_SOURCES := userland/base/make/main.c userland/base/make/read.c \
	userland/base/make/variable.c userland/base/make/expand.c \
	userland/base/make/function.c userland/base/make/rule.c \
	userland/base/make/update.c userland/base/make/job.c \
	userland/base/make/util.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,make,make,*,y,basic,$(ZEDBSD_MAKE_SOURCES),y,base,,,,,,,usr/bin))
