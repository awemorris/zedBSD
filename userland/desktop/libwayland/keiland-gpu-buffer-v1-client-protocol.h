/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Declares the selected Keiland protocol objects and typed requests. */

#ifndef KERN_KEILAND_GPU_BUFFER_V1_CLIENT_PROTOCOL_H
#define KERN_KEILAND_GPU_BUFFER_V1_CLIENT_PROTOCOL_H

#include <wayland/wayland-client-core.h>
#include <wayland/wayland-client-protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_registry;
struct wl_callback;
struct wl_compositor;
struct wl_surface;
struct wl_region;
struct wl_buffer;
struct wl_output;
struct xdg_wm_base;
struct xdg_positioner;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;
struct kl_gpu_buffer_v1;

struct kl_gpu_buffer_v1;
extern const struct wl_interface kl_gpu_buffer_v1_interface;
#define KL_GPU_BUFFER_V1_DESTROY 0U
void kl_gpu_buffer_v1_destroy(struct kl_gpu_buffer_v1 *object);
#define KL_GPU_BUFFER_V1_CREATE_BUFFER 1U
struct wl_buffer *kl_gpu_buffer_v1_create_buffer(struct kl_gpu_buffer_v1 *object, int fd, struct wl_array *metadata);
#define KL_GPU_BUFFER_V1_SET_ACQUIRE_FENCE 2U
#define KL_GPU_BUFFER_V1_SET_ACQUIRE_FENCE_SINCE_VERSION 2U
void kl_gpu_buffer_v1_set_acquire_fence(struct kl_gpu_buffer_v1 *object, struct wl_surface *surface, int fd, uint64_t generation);
#define KL_GPU_BUFFER_V1_SET_ALPHA 3U
#define KL_GPU_BUFFER_V1_SET_ALPHA_SINCE_VERSION 3U
#define KL_GPU_BUFFER_V1_ALPHA_OPAQUE 0U
#define KL_GPU_BUFFER_V1_ALPHA_PREMULTIPLIED 1U
void kl_gpu_buffer_v1_set_alpha(struct kl_gpu_buffer_v1 *object, struct wl_buffer *buffer, uint32_t alpha);
void kl_gpu_buffer_v1_set_user_data(struct kl_gpu_buffer_v1 *object, void *data);
void *kl_gpu_buffer_v1_get_user_data(struct kl_gpu_buffer_v1 *object);
uint32_t kl_gpu_buffer_v1_get_version(struct kl_gpu_buffer_v1 *object);


#ifdef __cplusplus
}
#endif

#endif
