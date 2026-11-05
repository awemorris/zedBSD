include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Captures YUYV frames from /dev/camN into imported Vulkan memory and turns them gray on the GPU (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
CAMVK_SOURCES := userland/tests/camvk/main.c userland/tests/camcommon/camcommon.c
$(eval $(call USERLAND_PACKAGE,camvk,camera to Vulkan test,*,n,basic,$(CAMVK_SOURCES),y,tests,tests/camvk,desktop/libvulkan,0755,program,,,bin))
