include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libbrowser (ws074-p057): the Web browser engine as a component, plan/ws074/design.md §19 -- HTML, CSS,
# the layout, JavaScript, the DOM, the network, the images and the CPU and Vulkan renderers -- behind
# <browser/browser.h> (userland/desktop/include/browser/browser.h).  /bin/browser is its shell; any program can show HTML with it.
# The engine sources, private headers, tables and shaders belong to this library.
# The app in desktop/browser uses only <browser/browser.h>; exports.map keeps the implementation private.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The library, and the notices of the licences of its committed tables.
LIBBROWSER_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libbrowser.so=$(BUILD)/dynamic/libbrowser.so) \
	/usr/share/licenses/browser/WHATWG-entities=userland/base/licenses/browser/WHATWG-entities \
	/usr/share/licenses/browser/Unicode-License-V3=userland/base/licenses/browser/Unicode-License-V3

$(eval $(call ZEDBSD_USERLAND_PACKAGE,libbrowser,libbrowser,amd64,n,library,$(LIBBROWSER_SOURCES),y,desktop,desktop/libbrowser,desktop/libtruetype desktop/libvulkan desktop/libmedia desktop/libjpeg-compat desktop/libpng-compat base/libz-compat desktop/libgif-compat,,shared-library,$(LIBBROWSER_IMAGE_DATA),userland/desktop/include/browser/browser.h,lib))
