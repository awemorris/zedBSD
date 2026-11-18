/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wayland EGL window: a wl_surface with the size an EGL window surface
 * draws it at (libwayland-egl).  The calls are the standard ones.
 */

#ifndef WAYLAND_EGL_CORE_H
#define WAYLAND_EGL_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* EGL's <eglplatform.h> picks the Wayland native types when this is defined. */
#define WL_EGL_PLATFORM 1

struct wl_egl_window;
struct wl_surface;

struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height);
void wl_egl_window_destroy(struct wl_egl_window *egl_window);
void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height, int dx, int dy);
void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
