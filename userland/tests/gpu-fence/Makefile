include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Explicit standard Vulkan external-fence regression for disposable GPU test boots.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,gpu-fence-test,GPU external fence test,amd64,y,basic,userland/tests/gpu-fence/main.c,y,tests,tests/gpu-fence,desktop/libvulkan,0755,program,,,bin))
