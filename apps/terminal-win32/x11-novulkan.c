/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's X server without Vulkan: the windows are shown through the
 * backend's GDI present.
 *
 * mikuterm imports no Vulkan library (the loader is to be opened at run
 * time from System32 only), while the X server's Vulkan present calls the
 * loader's functions directly; until it opens the loader itself, mikuterm
 * leaves it out and gives the core and the backend these, which say that
 * Vulkan cannot show the windows.  The core then shows every window
 * through the backend's present (it is created with no WSI).
 */

#include "userland/desktop/xserver/internal.h"
#include "userland/desktop/xserver/win32/internal.h"

#include <errno.h>

/*
 * Opens no Vulkan state: the core shows the windows through the backend.
 */
struct x11_vulkan *
x11_vulkan_open(
	const char *wsi)
{
	/* No Vulkan in mikuterm's X server yet. */
	(void)wsi;
	return NULL;
}

/*
 * Closes the Vulkan state there is none of.
 */
void
x11_vulkan_close(
	struct x11_vulkan *vulkan)
{
	/* Nothing was opened. */
	(void)vulkan;
}

/*
 * Opens no swapchain: the window is shown through the backend.
 */
struct x11_vulkan_window *
x11_vulkan_window_open(
	struct x11server *server,
	struct x11_surface *surface,
	unsigned width,
	unsigned height)
{
	/* No swapchain. */
	(void)server;
	(void)surface;
	(void)width;
	(void)height;
	return NULL;
}

/*
 * Shows nothing through a swapchain there is none of.
 */
int
x11_vulkan_window_present(
	struct x11_vulkan_window *window,
	const uint32_t *pixels)
{
	/* No swapchain. */
	(void)window;
	(void)pixels;
	return ENOTSUP;
}

/*
 * Resizes no swapchain.
 */
int
x11_vulkan_window_resize(
	struct x11_vulkan_window *window,
	unsigned width,
	unsigned height)
{
	/* No swapchain. */
	(void)window;
	(void)width;
	(void)height;
	return ENOTSUP;
}

/*
 * Closes no swapchain.
 */
void
x11_vulkan_window_close(
	struct x11_vulkan_window *window)
{
	/* Nothing was opened. */
	(void)window;
}

/*
 * Makes no Vulkan surface of a window (the backend's vulkan_surface
 * without its Vulkan file).
 */
int
win32_surface_vulkan(
	void *context,
	struct x11_surface *surface,
	void *instance,
	void *result)
{
	/* No Vulkan. */
	(void)context;
	(void)surface;
	(void)instance;
	(void)result;
	return ENOTSUP;
}
