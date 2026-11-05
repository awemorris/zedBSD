include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws269-p008: CPU and GPU cache coherency stress (buffers through a compute shader, images through a clear and a copy).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkcoherency,Vulkan coherency stress,amd64 visionfive2,n,basic,userland/tests/vkcoherency/main.c,y,tests,tests/vkcoherency,desktop/libvulkan,0755,program,,,bin))
