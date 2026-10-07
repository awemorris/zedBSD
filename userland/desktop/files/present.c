/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frames of files shown in its window or on the desktop (WS131 p020):
 * libkeiland's window shows each frame the interface drew on the CPU
 * through its Vulkan presenter (this file's presenter, ws071, moved into
 * libkeiland).  This keeps the size frames are drawn at, whether the frame
 * is see-through (the glass), and the last frame's times for a slow
 * frame's line.
 */

#include "window.h"

#include <errno.h>
#include <string.h>

static VkResult present_size(struct fm_present *present);

/*
 * Made the Vulkan instance before the window for the desktop (ws094-p009);
 * libkeiland's window makes its own with the window, so there is nothing
 * to do.  Returns VK_SUCCESS.
 */
VkResult
fm_present_instance(
	struct fm_present *present)
{
	/* Nothing yet. */
	memset(present, 0, sizeof(*present));
	return VK_SUCCESS;
}

/*
 * Starts showing frames in a window: their size, and whether they are
 * see-through.  Returns VK_SUCCESS, or an error with the call that failed
 * in operation.
 */
VkResult
fm_present_open(
	struct fm_present *present,
	struct fm_window *window)
{
	VkResult result;

	/* Nothing yet but the window. */
	memset(present, 0, sizeof(*present));
	present->window = window;

	/* The size frames are drawn at. */
	result = present_size(present);
	if (result != VK_SUCCESS)
		return result;

	/* Whether zdesktop blends the frame by its alpha. */
	present->premultiplied = kl_window_see_through(window->kui);

	/* Succeeded: frames can be shown. */
	return VK_SUCCESS;
}

/*
 * Gives frames the window's new size (the presenter's swapchain is made
 * again).  Returns VK_SUCCESS, or an error with the call that failed.
 */
VkResult
fm_present_resize(
	struct fm_present *present,
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
 * Shows a frame of the presenter's size, of which only part changed when
 * part is not NULL (BUG-221: only that part is copied and told to
 * zdesktop).  Returns VK_SUCCESS, VK_ERROR_OUT_OF_DATE_KHR when the frames
 * need the window's new size, or another error with the call that failed.
 */
VkResult
fm_present_frame(
	struct fm_present *present,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *part)
{
	struct kl_present_times times;
	int error;

	/* The frame, and its times. */
	present->operation = "kl_window_present_part";
	error = kl_window_present_part(present->window->kui, pixels, stride, part);
	kl_window_present_times(present->window->kui, &times);
	present->copy_ms = times.copy_ms;
	present->acquire_ms = times.acquire_ms;
	present->submit_ms = 0U;
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
fm_present_close(
	struct fm_present *present)
{
	/* Nothing is held. */
	memset(present, 0, sizeof(*present));
}

/* Takes the size frames are drawn at from the window's presenter; VK_SUCCESS or an error. */
static VkResult
present_size(
	struct fm_present *present)
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
