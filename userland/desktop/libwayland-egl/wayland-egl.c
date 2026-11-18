/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libwayland-egl: the Wayland EGL window (WS068 p002).
 *
 * The window only remembers a surface and a size; libEGL makes the
 * swapchain for the surface and remakes it when the size changes.
 */

#include "wayland-egl-backend.h"

#include <stdlib.h>
#include <wayland-egl-core.h>

/*
 * Makes an EGL window of a size over a surface.  Returns NULL for a
 * missing surface, a size that is not positive, or no memory.
 */
struct wl_egl_window *
wl_egl_window_create(
	struct wl_surface *surface,
	int width,
	int height)
{
	struct wl_egl_window *window;

	/* A window needs a surface and a size. */
	if (surface == NULL || width <= 0 || height <= 0)
		return NULL;

	/* The window. */
	window = calloc(1U, sizeof(*window));
	if (window == NULL)
		return NULL;

	/* Succeeded: the surface and its size; nothing attached yet. */
	window->surface = surface;
	window->width = width;
	window->height = height;
	return window;
}

/*
 * Releases an EGL window (not its surface, which is the application's).
 */
void
wl_egl_window_destroy(
	struct wl_egl_window *egl_window)
{
	/* The memory only. */
	free(egl_window);
}

/*
 * Gives an EGL window a new size; EGL draws the next frame at it.
 */
void
wl_egl_window_resize(
	struct wl_egl_window *egl_window,
	int width,
	int height,
	int dx,
	int dy)
{
	/* A missing window or a size that is not positive changes nothing. */
	if (egl_window == NULL || width <= 0 || height <= 0)
		return;

	/* The new size and offset, and a new generation for EGL to notice. */
	egl_window->width = width;
	egl_window->height = height;
	egl_window->dx = dx;
	egl_window->dy = dy;
	egl_window->generation++;
}

/*
 * Reports the size of the image EGL last attached to the window.
 */
void
wl_egl_window_get_attached_size(
	struct wl_egl_window *egl_window,
	int *width,
	int *height)
{
	/* A missing window has attached nothing. */
	if (egl_window == NULL)
		return;

	/* Each size asked for. */
	if (width != NULL)
		*width = egl_window->attached_width;
	if (height != NULL)
		*height = egl_window->attached_height;
}
