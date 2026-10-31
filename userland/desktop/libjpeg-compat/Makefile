include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libjpeg-compat: the IJG libjpeg decompression interface for the desktop programs (plan/ws074/design.md §10,
# after plan/ws035/ws.md D1 to D6); no libjpeg code.  This first part decodes sequential Huffman JPEG
# (ws074-p019); progressive, CMYK/YCCK and saved markers come with ws074-p020.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBJPEG_COMPAT_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libjpeg-compat.so=$(BUILD)/dynamic/libjpeg-compat.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libjpeg-compat,libjpeg-compat,*,y,library,$(LIBJPEG_COMPAT_SOURCES),y,desktop,desktop/libjpeg-compat,,,shared-library,$(LIBJPEG_COMPAT_IMAGE_DATA),,lib))
