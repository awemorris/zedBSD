include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# What the compositor tells a client about a pen: the tablet protocol, or the pointer (WS079 p003).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TABLET_PROBE_SOURCES := userland/tests/tablet-probe/main.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,tablet-probe,pen tablet probe,amd64,n,basic,$(TABLET_PROBE_SOURCES),y,tests,tests/tablet-probe,desktop/libwayland,0755,program,,,bin))
