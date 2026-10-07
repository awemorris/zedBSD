include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# wl_subcompositor and wl_subsurface of the compositor (WS035 p077).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
SUBSURFACE_PROBE_SOURCES := userland/tests/subsurface-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,subsurface-probe,wl_subsurface probe,amd64,y,basic,$(SUBSURFACE_PROBE_SOURCES),y,tests,tests/subsurface-probe,desktop/libwayland,0755,program,,,bin))
