# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBEDIT_SOURCES := \
	userland/base/libedit/readline.c

$(eval $(call USERLAND_PACKAGE,libedit,libedit,*,n,library,$(LIBEDIT_SOURCES),n,base,\
	base/libedit,))
