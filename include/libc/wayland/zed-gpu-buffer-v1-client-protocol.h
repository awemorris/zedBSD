/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Declares the selected zed protocol objects and typed requests. */

#ifndef ZEDBSD_ZED_GPU_BUFFER_V1_CLIENT_PROTOCOL_H
#define ZEDBSD_ZED_GPU_BUFFER_V1_CLIENT_PROTOCOL_H

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
struct zed_gpu_buffer_v1;

struct zed_gpu_buffer_v1;
extern const struct wl_interface zed_gpu_buffer_v1_interface;
#define ZED_GPU_BUFFER_V1_DESTROY 0U
void zed_gpu_buffer_v1_destroy(struct zed_gpu_buffer_v1 *object);
#define ZED_GPU_BUFFER_V1_CREATE_BUFFER 1U
struct wl_buffer *zed_gpu_buffer_v1_create_buffer(struct zed_gpu_buffer_v1 *object, int fd, struct wl_array *metadata);
void zed_gpu_buffer_v1_set_user_data(struct zed_gpu_buffer_v1 *object, void *data);
void *zed_gpu_buffer_v1_get_user_data(struct zed_gpu_buffer_v1 *object);
uint32_t zed_gpu_buffer_v1_get_version(struct zed_gpu_buffer_v1 *object);


#ifdef __cplusplus
}
#endif

#endif
