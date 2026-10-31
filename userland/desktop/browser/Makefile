include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Web browser app is the shell over libbrowser.so (desktop/libbrowser owns the engine).
# This package is /bin/browser: main.c (the command line and the headless modes) and
# shell/ (the compositor window, its titlebar and the input), which use the engine through <browser/browser.h>.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The pages the browser brings: a start page that works without the network (opened by giving its path;
# without an argument the browser opens its home page, as App Home starts it) and the pages it links to.
KEILAND_BROWSER_DATA := /usr/share/browser/start.html=$(KEILAND_BROWSER_DIR)/data/start.html \
	/usr/share/browser/about.html=$(KEILAND_BROWSER_DIR)/data/about.html \
	/usr/share/browser/text.html=$(KEILAND_BROWSER_DIR)/data/text.html

$(eval $(call ZEDBSD_USERLAND_PACKAGE,browser,browser - Web browser,amd64,n,basic,$(KEILAND_BROWSER_SOURCES),y,desktop,desktop/browser,desktop/libbrowser desktop/libvulkan desktop/libwayland desktop/libkeiland base/libz-compat,0755,program,$(KEILAND_BROWSER_DATA),,bin))
