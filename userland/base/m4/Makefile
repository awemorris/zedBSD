include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_M4_SOURCES := userland/base/m4/eval.c userland/base/m4/expr.c \
	userland/base/m4/gnum4.c userland/base/m4/look.c \
	userland/base/m4/main.c userland/base/m4/misc.c \
	userland/base/m4/ohash.c userland/base/m4/parser.c \
	userland/base/m4/tokenizer.c userland/base/m4/trace.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,m4,m4,*,y,basic,$(ZEDBSD_M4_SOURCES),y,base,,))
