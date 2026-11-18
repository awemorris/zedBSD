include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Vulkan matrix of what the Keiland desktop draws with (push constants, the batch vertex, textures, blends, many draws, passes, scissor and viewport), each case read back and compared.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkmatrix,Vulkan matrix test,visionfive2,n,basic,userland/tests/vkmatrix/main.c,y,tests,tests/vkmatrix,desktop/libvulkan,0755,program,,,bin))
