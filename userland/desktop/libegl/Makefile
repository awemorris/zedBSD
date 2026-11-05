include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBEGL_SOURCES := userland/desktop/libegl/egl.c userland/desktop/libegl/vulkan.c
LIBEGL_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libEGL.so=$(BUILD)/dynamic/libEGL.so)

$(eval $(call USERLAND_PACKAGE,libegl,libEGL,x86_64,y,library,$(LIBEGL_SOURCES),y,desktop,desktop/libegl,desktop/libvulkan desktop/libwayland desktop/libwayland-egl,,shared-library,$(LIBEGL_IMAGE_DATA),,lib))
