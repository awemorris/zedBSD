include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ifdef ZEDBSD_TOPLEVEL_BUILD
ZEDBSD_CURSES_DATA := /lib/libcurses.a=$(BUILD)/lib/libcurses.a
endif
$(eval $(call ZEDBSD_USERLAND_PACKAGE,curses,curses terminal library,*,y,\
	library,userland/base/curses/curses.c userland/base/common/terminfo.c,y,\
	base,base/curses,base/terminfo,,library,$(ZEDBSD_CURSES_DATA),\
	userland/base/curses/curses.h))
