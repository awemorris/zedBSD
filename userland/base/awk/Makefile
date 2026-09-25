include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_AWK_SOURCES := userland/base/awk/main.c userland/base/awk/lex.c \
	userland/base/awk/parse.c userland/base/awk/value.c \
	userland/base/awk/record.c userland/base/awk/run.c \
	userland/base/awk/builtin.c userland/base/awk/io.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,awk,awk,*,y,basic,$(ZEDBSD_AWK_SOURCES),y,base,,))
