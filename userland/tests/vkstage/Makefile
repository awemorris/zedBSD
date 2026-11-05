include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws269-p010: the gpu-selftest stages after image (clear, copy, draw, sample, depth, present), one a run.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkstage,Vulkan stage test,amd64 visionfive2,n,basic,userland/tests/vkstage/main.c,y,tests,tests/vkstage,desktop/libvulkan,0755,program,,,bin))
