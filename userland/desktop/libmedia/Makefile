include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
# libmedia: original AAC-LC and standard Vulkan Video H.264 decoding, SBC encoding and decoding (ws205-p002), AAC-LC encoding (ws205-p003), container readers and a silent video engine.
# Optional software codecs and sound streams belong to applications; this library needs libc alone.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBMEDIA_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libmedia.so=$(BUILD)/dynamic/libmedia.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libmedia,libmedia,amd64,n,library,$(LIBMEDIA_SOURCES),y,desktop,desktop/libmedia,,,shared-library,$(LIBMEDIA_IMAGE_DATA),,lib))
