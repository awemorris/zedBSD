include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The second program over libbrowser (ws074-p057): a page drawn into a PPM file through <browser/browser.h> only,
# with the CPU or the engine's offscreen GPU image, and no window.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
BROWSER_PROBE_SOURCES := userland/tests/browser-probe/main.c userland/tests/browser-threads/threads-posix.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,browser-probe,libbrowser probe,amd64,n,basic,$(BROWSER_PROBE_SOURCES),y,tests,tests/browser-probe,desktop/libbrowser,0755,program,,,bin))
