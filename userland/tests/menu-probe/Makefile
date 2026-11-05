include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The System Menu's protocol errors and libkeiland's own checks (WS070 p002).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MENU_PROBE_SOURCES := userland/tests/menu-probe/main.c
$(eval $(call USERLAND_PACKAGE,menu-probe,System Menu probe,amd64,y,basic,$(MENU_PROBE_SOURCES),y,tests,tests/menu-probe,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
