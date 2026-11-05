# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBBROWSER_IMAGE_DATA := \
	$(if $(TOPLEVEL_BUILD),/lib/libbrowser.so=$(BUILD)/dynamic/libbrowser.so) \
		/usr/share/licenses/browser/WHATWG-entities=userland/base/licenses/browser/WHATWG-entities \
		/usr/share/licenses/browser/Unicode-License-V3=userland/base/licenses/browser/Unicode-License-V3

$(eval $(call USERLAND_PACKAGE,libbrowser,libbrowser,x86_64,n,library,$(LIBBROWSER_SOURCES),y,desktop,desktop/libbrowser,desktop/libtruetype desktop/libvulkan desktop/libmedia desktop/libjpeg-compat desktop/libpng-compat base/libz-compat desktop/libgif-compat desktop/libwebp-compat,,shared-library,$(LIBBROWSER_IMAGE_DATA),userland/desktop/include/browser/browser.h userland/desktop/include/browser/storage.h,lib))
