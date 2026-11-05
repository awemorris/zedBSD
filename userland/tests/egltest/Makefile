include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# EGL and OpenGL ES test (WS068): clears or draws into a Wayland window or the screen and presents it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
EGLTEST_SOURCES := userland/tests/egltest/main.c userland/tests/egltest/scene.c userland/tests/egltest/cube.c \
	userland/tests/egltest/es3.c userland/tests/egltest/formats.c \
	userland/tests/egltest/volumes.c userland/tests/egltest/targets.c \
	userland/tests/egltest/blits.c userland/tests/egltest/queries.c userland/tests/egltest/feedback.c \
	userland/tests/egltest/es2.c userland/tests/egltest/etc2.c
$(eval $(call USERLAND_PACKAGE,egltest,EGL test,x86_64,y,basic,$(EGLTEST_SOURCES),y,tests,tests/egltest,desktop/libegl desktop/libglesv2 desktop/libwayland-egl desktop/libwayland,0755,program,,,bin))
