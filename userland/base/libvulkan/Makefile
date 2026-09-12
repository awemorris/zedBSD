include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD standard Vulkan shared library.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBVULKAN_SOURCES := userland/base/libvulkan/objects.c \
	userland/base/libvulkan/wire.c userland/base/libvulkan/context.c \
	userland/base/libvulkan/codec.c userland/base/libvulkan/dispatch.c \
	userland/base/libvulkan/instance.c userland/base/libvulkan/device.c \
	userland/base/libvulkan/resources.c userland/base/libvulkan/pipeline.c \
	userland/base/libvulkan/descriptors.c userland/base/libvulkan/commands.c \
	userland/base/libvulkan/memory.c userland/base/libvulkan/sync.c \
	userland/base/libvulkan/queue.c userland/base/libvulkan/query.c \
	userland/base/libvulkan/wsi.c userland/base/libvulkan/wsi-display.c \
	userland/base/libvulkan/wsi-swapchain.c

# The image metadata owns /lib placement; standalone installation honors PREFIX.
LIBVULKAN_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libvulkan.so=$(BUILD)/dynamic/libvulkan.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libvulkan,Vulkan 1.0,amd64,n,library,$(LIBVULKAN_SOURCES),y,base,base/libvulkan,,,shared-library,$(LIBVULKAN_IMAGE_DATA),,lib))
