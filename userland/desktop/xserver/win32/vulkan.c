/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: the Vulkan surface of a window, which
 * the core shows the window's pixels through.
 *
 * The program imports no Vulkan library: the loader (vulkan-1.dll) is
 * opened from the system folder when the core first asks for a surface,
 * and vkCreateWin32SurfaceKHR is looked up on the core's instance, which
 * the same loader made.
 */

#include "userland/desktop/xserver/win32/internal.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <errno.h>

static int vulkan_loader(struct x11_win32 *backend, PFN_vkGetInstanceProcAddr *lookup);

/*
 * Makes the Vulkan surface of a window (the backend's vulkan_surface).
 *
 * instance is the core's VkInstance, made with VK_KHR_win32_surface;
 * result points at the VkSurfaceKHR to fill.  The window is then painted
 * by the core's swapchain only, not by GDI.  Returns 0, ENOENT when the
 * system has no Vulkan loader or the instance no Win32 surfaces, or EIO
 * when the surface cannot be made.
 */
int
win32_surface_vulkan(
	void *context,
	struct x11_surface *surface,
	void *instance,
	void *result)
{
	struct x11_win32 *backend;
	PFN_vkGetInstanceProcAddr lookup;
	PFN_vkVoidFunction found;
	PFN_vkCreateWin32SurfaceKHR create;
	VkWin32SurfaceCreateInfoKHR information;
	VkSurfaceKHR made;
	VkResult created;
	int error;

	/* The loader's lookup of the instance's functions. */
	backend = context;
	error = vulkan_loader(backend, &lookup);
	if (error != 0)
		return error;

	/* The instance's function that makes a surface of a window. */
	found = lookup((VkInstance)instance, "vkCreateWin32SurfaceKHR");
	if (found == NULL)
		return ENOENT;
	create = (PFN_vkCreateWin32SurfaceKHR)(void *)found;

	/* Makes the surface of the window. */
	information.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	information.pNext = NULL;
	information.flags = 0;
	information.hinstance = backend->instance;
	information.hwnd = surface->hwnd;
	created = create((VkInstance)instance, &information, NULL, &made);
	if (created != VK_SUCCESS) {
		win32_log(backend, "X11 WIN32 vkCreateWin32SurfaceKHR failed: %d", (int)created);
		return EIO;
	}

	/* Succeeded: the core paints the window through the surface. */
	surface->vulkan = 1;
	*(VkSurfaceKHR *)result = made;
	win32_log(backend, "X11 WIN32 vulkan surface window=0x%lx", (unsigned long)surface->window);
	return 0;
}

/*
 * Opens the system's Vulkan loader (once; only from System32, never from
 * the program's folder) and gives its vkGetInstanceProcAddr.  Returns 0,
 * or ENOENT.
 */
static int
vulkan_loader(
	struct x11_win32 *backend,
	PFN_vkGetInstanceProcAddr *lookup)
{
	FARPROC found;

	/* Opens the loader the first time. */
	if (backend->vulkan_loader == NULL) {
		backend->vulkan_loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (backend->vulkan_loader == NULL)
			return ENOENT;
	}

	/* Its lookup of the instances' functions. */
	found = GetProcAddress(backend->vulkan_loader, "vkGetInstanceProcAddr");
	if (found == NULL)
		return ENOENT;

	/* Succeeded: the lookup. */
	*lookup = (PFN_vkGetInstanceProcAddr)(void *)found;
	return 0;
}
