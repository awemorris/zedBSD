include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws269-p009: the smallest Vulkan compute test (one dispatch, read back, one line).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkcompute,Vulkan compute test,amd64 visionfive2,n,basic,userland/tests/vkcompute/main.c,y,tests,tests/vkcompute,desktop/libvulkan,0755,program,,,bin))
