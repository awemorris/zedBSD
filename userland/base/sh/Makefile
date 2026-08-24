include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_SH_SOURCES := userland/base/sh/main.c \
	userland/base/sh/builtins.c userland/base/sh/lexer.c \
	userland/base/sh/expand.c userland/base/sh/glob.c \
	userland/base/sh/vars.c userland/base/sh/arithmetic.c \
	userland/base/sh/alias.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sh,sh,*,y,core,\
	$(ZEDBSD_SH_SOURCES),n,base,base/sh,base/libedit,))
