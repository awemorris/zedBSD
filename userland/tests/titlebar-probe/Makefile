include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Titlebar Presentation's protocol errors and libkeiland's own checks (WS070 p008).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TITLEBAR_PROBE_SOURCES := userland/tests/titlebar-probe/main.c
$(eval $(call USERLAND_PACKAGE,titlebar-probe,Titlebar Presentation probe,amd64,y,basic,$(TITLEBAR_PROBE_SOURCES),y,tests,tests/titlebar-probe,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
