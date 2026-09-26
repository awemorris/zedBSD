include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

# Acquire fences of zed_gpu_buffer_v1 revision two (WS035 p054): wltest's window
# and renderer, with one more fence the test signals itself.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ACQUIRE_FENCE_TEST_SOURCES := userland/base/tests/acquire-fence/main.c \
	userland/base/wltest/window.c userland/base/wltest/renderer.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,acquire-fence-test,Acquire fence test,amd64,y,basic,$(ACQUIRE_FENCE_TEST_SOURCES),y,base,base/tests/acquire-fence,base/libvulkan base/libwayland,0755,program,,,bin))
