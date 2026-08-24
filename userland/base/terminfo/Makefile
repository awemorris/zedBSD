include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_TERMINFO_DATA := userland/base/terminfo/ansi.zti \
	userland/base/terminfo/linux.zti \
	userland/base/terminfo/screen-256color.zti \
	userland/base/terminfo/screen.zti \
	userland/base/terminfo/tmux-256color.zti \
	userland/base/terminfo/tmux.zti userland/base/terminfo/vt100.zti \
	userland/base/terminfo/vt220.zti \
	userland/base/terminfo/xterm-256color.zti \
	userland/base/terminfo/xterm.zti userland/base/terminfo/zed.zti
ZEDBSD_TERMINFO_INSTALL := $(foreach file,$(ZEDBSD_TERMINFO_DATA),\
	/lib/terminfo/$(notdir $(file))=$(file))

$(eval $(call ZEDBSD_USERLAND_PACKAGE,terminfo,major terminal descriptions,\
	*,y,data,,y,base,base/terminfo,,,data,$(ZEDBSD_TERMINFO_INSTALL)))
