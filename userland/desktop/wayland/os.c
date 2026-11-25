/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's operating system, through libkeiland-backend (ws131-p008;
 * it was linux/os-linux.c, freebsd/os-freebsd.c and zedbsd/os-zedbsd.c
 * behind zwl-os.h).
 *
 * The seat is taken before Vulkan opens (none on zedBSD, where sessiond has
 * handed the display over); the primary node it holds is the one Vulkan's
 * inquiry must open too; the backend's descriptors join the event loop's
 * poll; and the display is acquired and released through the backend with
 * the compositor's Vulkan instance.  Close tolerates every partial open.
 */

#include "compose.h"
#include "kwl.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-display.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void os_vulkan(const struct kwl_server *server, struct kl_backend_vulkan *vulkan);

/*
 * Takes the seat and its primary node before Vulkan opens, and chooses the
 * default socket.
 */
int
kwl_os_open(
	struct kwl_server *server)
{
	const char *path;
	int descriptor;
	int error;

	/* The seat, where the system has one for the compositor. */
	error = kl_backend_seat_open(server->backend);
	if (error != 0 && error != ENOTSUP)
		return error;
	server->os_paused = (unsigned)kl_backend_seat_paused(server->backend);

	/* Vulkan's inquiry opens the same primary node the seat holds (skipped in headless). */
	if (!server->headless) {
		error = kl_backend_display_node(server->backend, &descriptor, &path);
		if (error == 0) {
			error = setenv("KEILAND_DRM_DEVICE", path, 1);
			if (error != 0)
				return errno;
		}
	}

	/* The system's default socket, unless the command line chose one. */
	if (server->socket_given == 0) {
		error = kl_backend_display_socket(server->socket_path, sizeof(server->socket_path));
		if (error != 0 && error != ENOTSUP)
			return error;
	}

	/* Succeeded: the compositor may open Vulkan. */
	return 0;
}

/*
 * Returns the seat after the display and the inputs have been given back.
 */
void
kwl_os_close(
	struct kwl_server *server)
{
	/* The seat (the virtual terminal is restored with it on Linux). */
	kl_backend_seat_close(server->backend);
	server->os_paused = 0;
}

/*
 * Counts the backend's descriptors for the next poll.
 */
size_t
kwl_os_poll_count(
	const struct kwl_server *server)
{
	size_t count;

	/* The seat's service, while the display is paused too. */
	count = kl_backend_poll_count(server->backend);
	return count;
}

/*
 * Fills the backend's poll descriptors.
 */
void
kwl_os_poll_fill(
	struct kwl_server *server,
	struct pollfd *descriptors)
{
	/* The backend fills its own range. */
	kl_backend_poll_fill(server->backend, descriptors);
}

/*
 * Handles what poll reported for the backend (the callbacks are in
 * backend-host.c).
 */
void
kwl_os_poll_done(
	struct kwl_server *server,
	const struct pollfd *descriptors)
{
	/* The backend dispatches and calls back. */
	kl_backend_poll_done(server->backend, descriptors);
}

/*
 * Lets Vulkan acquire the chosen display.
 */
VkResult
kwl_os_display_acquire(
	struct kwl_server *server,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	struct kl_backend_vulkan vulkan;
	VkResult result;

	/* The backend hands Vulkan the display's node, where there is one. */
	os_vulkan(server, &vulkan);
	result = kl_backend_display_acquire(server->backend, &vulkan, physical, display);
	return result;
}

/*
 * Returns an acquired display after its swapchain is destroyed.
 */
void
kwl_os_display_release(
	struct kwl_server *server,
	VkPhysicalDevice physical,
	VkDisplayKHR display)
{
	struct kl_backend_vulkan vulkan;
	VkResult result;

	/* A failed release is reported; the seat still returns its node at close. */
	os_vulkan(server, &vulkan);
	result = kl_backend_display_release(server->backend, &vulkan, physical, display);
	if (result != VK_SUCCESS)
		fprintf(stderr, "wayland: display release result=%d\n", result);
}

/*
 * Copies a display's EDID (up to size bytes) through the backend, where
 * this system's Vulkan gives it.  Returns its length, 0 for none.
 */
size_t
kwl_os_display_edid(
	struct kwl_server *server,
	VkDisplayKHR display,
	uint8_t *edid,
	size_t size)
{
	struct kl_backend_vulkan vulkan;
	size_t length;
	int error;

	/* The backend reads it, or says it cannot. */
	os_vulkan(server, &vulkan);
	error = kl_backend_display_edid(server->backend, &vulkan, server->compose->physical, display, edid, size, &length);
	if (error != 0)
		return 0U;

	/* Succeeded: the EDID's length, 0 when the display has none. */
	return length;
}

/* The compositor's Vulkan as the backend needs it. */
static void
os_vulkan(
	const struct kwl_server *server,
	struct kl_backend_vulkan *vulkan)
{
	/* The instance and the lookup that resolves its extensions. */
	vulkan->instance = server->compose->instance;
	vulkan->get_instance_proc_addr = vkGetInstanceProcAddr;
}
