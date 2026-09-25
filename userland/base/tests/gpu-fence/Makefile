include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

# Explicit standard Vulkan external-fence regression for disposable GPU test boots.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,gpu-fence-test,GPU external fence test,amd64,y,basic,userland/base/tests/gpu-fence/main.c,y,base,base/tests/gpu-fence,base/libvulkan,0755,program,,,bin))
