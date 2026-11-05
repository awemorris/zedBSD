include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ifdef TOPLEVEL_BUILD
CURSES_DATA := /lib/libcurses.a=$(BUILD)/lib/libcurses.a
endif

$(eval $(call USERLAND_PACKAGE,curses,libcurses,*,y,\
	library,userland/base/curses/curses.c userland/base/curses/termcap.c userland/base/common/terminfo.c,y,\
	base,base/curses,base/terminfo,,library,$(CURSES_DATA),\
	include/libc/curses.h))
