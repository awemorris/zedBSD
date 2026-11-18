include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws269-p015: the GPU reset recovery test (a kernel that never ends, then a fresh device); destructive, run with --isolated.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,vkreset,GPU reset recovery test,x86_64 visionfive2,n,basic,userland/tests/vkreset/main.c,y,tests,tests/vkreset,desktop/libvulkan tests/vkcompute,0755,program,,,bin))
