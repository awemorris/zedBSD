include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# xdg-decoration, cursor-shape and viewporter of the compositor (WS035 p080).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
EXTRAS_PROBE_SOURCES := userland/tests/extras-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,extras-probe,decoration cursor-shape and viewporter probe,amd64,y,basic,$(EXTRAS_PROBE_SOURCES),y,tests,tests/extras-probe,desktop/libwayland,0755,program,,,bin))
