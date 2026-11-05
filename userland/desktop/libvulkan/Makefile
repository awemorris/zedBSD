include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD standard Vulkan shared library.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBVULKAN_SOURCES := userland/desktop/libvulkan/objects.c \
	userland/desktop/libvulkan/wire.c userland/desktop/libvulkan/context.c \
	userland/desktop/libvulkan/codec.c userland/desktop/libvulkan/dispatch.c \
	userland/desktop/libvulkan/instance.c userland/desktop/libvulkan/device.c \
	userland/desktop/libvulkan/external-properties.c userland/desktop/libvulkan/external-fence.c \
	userland/desktop/libvulkan/resources.c userland/desktop/libvulkan/pipeline.c \
	userland/desktop/libvulkan/descriptors.c userland/desktop/libvulkan/commands.c \
	userland/desktop/libvulkan/sender.c \
	userland/desktop/libvulkan/content.c \
	userland/desktop/libvulkan/memory.c userland/desktop/libvulkan/dedicated.c \
	userland/desktop/libvulkan/sync.c userland/desktop/libvulkan/sync2.c \
	userland/desktop/libvulkan/video.c \
	userland/desktop/libvulkan/queue.c userland/desktop/libvulkan/query.c \
	userland/desktop/libvulkan/wsi.c userland/desktop/libvulkan/wsi-display.c \
	userland/desktop/libvulkan/wsi-swapchain.c userland/desktop/libvulkan/wsi-wayland.c \
	userland/desktop/libvulkan/wsi-image.c userland/desktop/libvulkan/wsi-display-nodes.c \
	userland/desktop/libvulkan/wsi-display-control.c

# The image metadata owns /lib placement; standalone installation honors PREFIX.
# The command numbers are the zedBSD GPU command protocol's (uapi/gpu-op.h, ws167-p002): no third-party notice.
LIBVULKAN_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libvulkan.so=$(BUILD)/dynamic/libvulkan.so)
$(eval $(call USERLAND_PACKAGE,libvulkan,libvulkan,x86_64,y,library,$(LIBVULKAN_SOURCES),y,desktop,desktop/libvulkan,desktop/libwayland,,shared-library,$(LIBVULKAN_IMAGE_DATA),,lib))
