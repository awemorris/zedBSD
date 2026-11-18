include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkdemo,vkdemo,x86_64 visionfive2,y,basic,userland/tests/vkdemo/main.c userland/tests/vkdemo/renderer.c userland/tests/vkdemo/display.c userland/base/common/sha256.c,y,tests,tests/vkdemo,desktop/libvulkan,0755,program,,,bin))
