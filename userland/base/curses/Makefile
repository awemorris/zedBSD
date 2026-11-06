# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ifdef TOPLEVEL_BUILD
CURSES_DATA := /lib/libcurses.a=$(BUILD)/lib/libcurses.a
endif

CURSES_SOURCES := \
	userland/base/curses/curses.c \
	userland/base/curses/termcap.c \
	userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,curses,libcurses,*,y,\
	library,$(CURSES_SOURCES),y,\
	base,base/curses,base/terminfo,,library,$(CURSES_DATA),\
	include/libc/curses.h))
