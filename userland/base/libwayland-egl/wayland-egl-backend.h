/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of a Wayland EGL window, shared by libwayland-egl, which the
 * application calls, and libEGL, which draws into the window.
 */

#ifndef WAYLAND_EGL_BACKEND_H
#define WAYLAND_EGL_BACKEND_H

struct wl_surface;

/*
 * One Wayland EGL window: the application's wl_surface and the size EGL
 * draws it at.
 *
 * It lives from wl_egl_window_create to wl_egl_window_destroy; the
 * surface stays the application's.
 */
struct wl_egl_window {
	/* The surface the window's images are attached to. */
	struct wl_surface *surface;

	/* The size asked for, and how far the next image moves the surface (wl_surface.attach's dx and dy). */
	int width;
	int height;
	int dx;
	int dy;

	/* The size of the image EGL last attached (0 before the first). */
	int attached_width;
	int attached_height;

	/* Counts the resizes, so EGL notices one at its next frame. */
	unsigned generation;
};

#endif
