# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# libyolox: YOLOX-Nano object detection with Vulkan compute or on the
# CPU (the CPU reference only, so far).

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk


LIBYOLOX_SOURCES := userland/desktop/libyolox/model.c userland/desktop/libyolox/runner.c \
	userland/desktop/libyolox/cpu.c userland/desktop/libyolox/preprocess.c \
	userland/desktop/libyolox/decode.c userland/desktop/libyolox/classes.c

LIBYOLOX_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libyolox.so=$(BUILD)/dynamic/libyolox.so)

$(eval $(call USERLAND_PACKAGE,libyolox,libyolox,x86_64,n,library,$(LIBYOLOX_SOURCES),y,desktop,desktop/libyolox,,,shared-library,$(LIBYOLOX_IMAGE_DATA),,lib))
