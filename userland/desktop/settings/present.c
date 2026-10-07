/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frames of Settings shown in its window (WS131 p019): libkeiland's
 * window shows each frame the interface drew on the CPU through its
 * Vulkan presenter (the file manager's present.c that Settings had copied,
 * ws071, moved into libkeiland).  This keeps the size frames are drawn at,
 * the device's name About shows, whether the frame is see-through (the
 * glass), and the last frame's times for a slow frame's line.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static VkResult present_size(struct se_present *present);

/*
 * Starts showing frames in a window: their size, the device and whether
 * they are see-through.  Returns VK_SUCCESS, or an error with the call
 * that failed in operation.
 */
VkResult
se_present_open(
	struct se_present *present,
	struct se_window *window)
{
	const char *name;
	VkResult result;

	/* Nothing yet but the window. */
	memset(present, 0, sizeof(*present));
	present->window = window;

	/* The size frames are drawn at. */
	result = present_size(present);
	if (result != VK_SUCCESS)
		return result;

	/* The device's name, and whether the compositor blends the frame by its alpha. */
	name = kl_window_device_name(window->kui);
	(void)snprintf(present->device_name, sizeof(present->device_name), "%s", name);
	present->premultiplied = kl_window_see_through(window->kui);

	/* Succeeded: frames can be shown. */
	return VK_SUCCESS;
}

/*
 * Gives frames the window's new size (the presenter's swapchain is made
 * again).  Returns VK_SUCCESS, or an error with the call that failed.
 */
VkResult
se_present_resize(
	struct se_present *present,
	uint32_t width,
	uint32_t height)
{
	VkResult result;

	/* The window's size is the presenter's. */
	(void)width;
	(void)height;
	result = present_size(present);
	return result;
}

/*
 * Shows a frame of the presenter's size.  Returns VK_SUCCESS,
 * VK_ERROR_OUT_OF_DATE_KHR when the frames need the window's new size, or
 * another error with the call that failed.
 */
VkResult
se_present_frame(
	struct se_present *present,
	const uint32_t *pixels,
	size_t stride)
{
	struct kl_present_times times;
	int error;

	/* The frame, and its times. */
	present->operation = "kl_window_present";
	error = kl_window_present(present->window->kui, pixels, stride);
	kl_window_present_times(present->window->kui, &times);
	present->copy_ms = times.copy_ms;
	present->acquire_ms = times.acquire_ms;
	present->present_ms = times.present_ms;
	present->wait_ms = times.wait_ms;

	/* A stale swapchain asks for the new size, any other refusal is a failure. */
	if (error == EAGAIN)
		return VK_ERROR_OUT_OF_DATE_KHR;
	if (error != 0)
		return VK_ERROR_DEVICE_LOST;

	/* Succeeded: the frame is shown. */
	return VK_SUCCESS;
}

/*
 * Stops showing frames (the window's presenter goes with the window).
 */
void
se_present_close(
	struct se_present *present)
{
	/* Nothing is held. */
	memset(present, 0, sizeof(*present));
}

/* Takes the size frames are drawn at from the window's presenter; VK_SUCCESS or an error. */
static VkResult
present_size(
	struct se_present *present)
{
	uint32_t width;
	uint32_t height;
	int error;

	/* The presenter at the window's size. */
	present->operation = "kl_window_present_resize";
	error = kl_window_present_resize(present->window->kui, &width, &height);
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: the size. */
	present->extent.width = width;
	present->extent.height = height;
	return VK_SUCCESS;
}
