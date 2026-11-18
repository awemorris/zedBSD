# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBJPEG_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libjpeg-compat.so=$(BUILD)/dynamic/libjpeg-compat.so)

$(eval $(call USERLAND_PACKAGE,libjpeg-compat,libjpeg-compat,*,y,library,$(LIBJPEG_COMPAT_SOURCES),y,desktop,desktop/libjpeg-compat,,,shared-library,$(LIBJPEG_COMPAT_IMAGE_DATA),,lib))
