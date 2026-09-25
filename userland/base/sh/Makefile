include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_SH_SOURCES := userland/base/sh/main.c \
	userland/base/sh/alias.c userland/base/sh/arithmetic.c \
	userland/base/sh/builtins.c userland/base/sh/cd.c \
	userland/base/sh/command.c userland/base/sh/exec.c \
	userland/base/sh/expand.c userland/base/sh/glob.c userland/base/sh/history.c \
	userland/base/sh/input.c userland/base/sh/jobs.c \
	userland/base/sh/limits.c userland/base/sh/options.c \
	userland/base/sh/parser.c userland/base/sh/printf.c \
	userland/base/sh/read.c userland/base/sh/redir.c \
	userland/base/sh/show.c userland/base/sh/test.c \
	userland/base/sh/trap.c userland/base/sh/util.c \
	userland/base/sh/vars.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sh,sh,*,y,core,\
	$(ZEDBSD_SH_SOURCES),n,base,base/sh,base/libedit,))
