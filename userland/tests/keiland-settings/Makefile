include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A client of libkeiland's settings for the tests of WS135 (ws135-p003): gets, sets, resets and watches the desktop's
# settings and prints each answer and change on a line of its own.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_SETTINGS_PROBE_SOURCES := userland/tests/keiland-settings/main.c
$(eval $(call USERLAND_PACKAGE,keiland-settings,Settings probe,x86_64,n,basic,$(KEILAND_SETTINGS_PROBE_SOURCES),y,tests,tests/keiland-settings,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
