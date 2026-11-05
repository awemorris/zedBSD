include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Judges the frames of /dev/camN against the test pattern a PC shows through an HDMI capture device.
# The MJPEG frames are decoded by libjpeg-compat's sources built into the program, since a basic
# test program links with the C library alone.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
CAMCHECK_SOURCES := userland/tests/camcheck/main.c userland/tests/camcheck/analyze.c \
	userland/tests/camcommon/camcommon.c \
	userland/desktop/libjpeg-compat/error.c userland/desktop/libjpeg-compat/memory.c \
	userland/desktop/libjpeg-compat/source.c userland/desktop/libjpeg-compat/marker.c \
	userland/desktop/libjpeg-compat/huffman.c userland/desktop/libjpeg-compat/idct.c \
	userland/desktop/libjpeg-compat/decompress.c
$(eval $(call USERLAND_PACKAGE,camcheck,camera pattern check,*,n,basic,$(CAMCHECK_SOURCES),y,tests,tests/camcheck,,0755,program,,,bin))
