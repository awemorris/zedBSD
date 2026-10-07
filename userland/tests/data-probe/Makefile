include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The clipboard between clients of the compositor (WS035 p079).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
DATA_PROBE_SOURCES := userland/tests/data-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,data-probe,clipboard probe,amd64,y,basic,$(DATA_PROBE_SOURCES),y,tests,tests/data-probe,desktop/libwayland,0755,program,,,bin))
