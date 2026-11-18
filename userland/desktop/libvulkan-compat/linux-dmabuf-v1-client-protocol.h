/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private version-three DMA-BUF wire declarations, owned by this WSI implementation. */
#ifndef COMPAT_LINUX_DMABUF_PROTOCOL_H
#define COMPAT_LINUX_DMABUF_PROTOCOL_H
#include <wayland-client.h>

/* Opaque wire objects owned until their protocol destructor is sent. */
struct zwp_linux_dmabuf_v1;
struct zwp_linux_buffer_params_v1;

/* Registry and parameter wire descriptions; no system libwayland is required. */
extern const struct wl_interface zwp_linux_dmabuf_v1_interface;
extern const struct wl_interface zwp_linux_buffer_params_v1_interface;

/* Receives the version-three format/modifier pairs on the surface's private queue. */
struct zwp_linux_dmabuf_v1_listener {
	void (*format)(void *, struct zwp_linux_dmabuf_v1 *, uint32_t);
	void (*modifier)(void *, struct zwp_linux_dmabuf_v1 *, uint32_t, uint32_t, uint32_t);
};

void compat_dmabuf_destroy(struct zwp_linux_dmabuf_v1 *object);
struct zwp_linux_buffer_params_v1 *compat_dmabuf_params(struct zwp_linux_dmabuf_v1 *object);
void compat_params_destroy(struct zwp_linux_buffer_params_v1 *object);
void compat_params_add(struct zwp_linux_buffer_params_v1 *object, int fd, uint32_t offset, uint32_t stride, uint64_t modifier);
struct wl_buffer *compat_params_buffer(struct zwp_linux_buffer_params_v1 *object, int width, int height, uint32_t format);
#endif
