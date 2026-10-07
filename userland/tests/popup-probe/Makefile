include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# xdg_popup, xdg_positioner and the xdg_toplevel requests of the compositor (WS035 p076).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
POPUP_PROBE_SOURCES := userland/tests/popup-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,popup-probe,xdg_popup probe,amd64,y,basic,$(POPUP_PROBE_SOURCES),y,tests,tests/popup-probe,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
