/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal with Vulkan (render.c) behind display.h: the
 * window's own swapchain, the GPU's atlas and the vertices the build
 * stores into it.  What failed last is a Vulkan call and its result, which
 * the error line names.
 */

#include "render.h"
#include "display.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The drawing of one window: the renderer and the result of the Vulkan
 * call it made last (VK_SUCCESS until one fails).
 */
struct terminal_display {
	struct terminal_renderer renderer;
	VkResult result;
};

static int display_error(struct terminal_display *display, VkResult result);

/*
 * Makes the Vulkan objects of the window: the device, the swapchain, the
 * pipeline, the glyph atlas (given to the font) and the vertex buffer.
 *
 * Returns 0, ENOMEM when the drawing has no memory (no display is made),
 * or EIO with the display made in part (terminal_display_operation names
 * what failed; terminal_display_close releases it).
 */
int
terminal_display_open(
	struct terminal_display **display,
	struct terminal_window *window,
	struct terminal_font *font)
{
	struct terminal_display *made;
	VkResult result;
	int error;

	/* The display's memory. */
	*display = NULL;
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Nothing is owned yet; the caller holds what is made from now on. */
	memset(made, 0, sizeof(*made));
	*display = made;

	/* The Vulkan objects, at the window's size. */
	result = terminal_renderer_open(&made->renderer, window, font);
	error = display_error(made, result);
	if (error != 0)
		return error;

	/* Succeeded: frames can be drawn. */
	return 0;
}

/*
 * Replaces the swapchain with one of a new size; the size it drew at is
 * then terminal_display_size's.
 *
 * Returns 0 or EIO.
 */
int
terminal_display_resize(
	struct terminal_display *display,
	uint32_t width,
	uint32_t height)
{
	VkResult result;
	int error;

	/* The swapchain at the new size. */
	result = terminal_renderer_resize(&display->renderer, width, height);
	error = display_error(display, result);
	if (error != 0)
		return error;

	/* Succeeded: the next frame is drawn at the new size. */
	return 0;
}

/*
 * Draws the grid, and over it the input method's text being composed,
 * presents it, and waits for the frame to finish.
 *
 * Returns 0, EAGAIN when the swapchain no longer matches the window (the
 * caller resizes and draws again), or EIO.  errno is left as it is.
 */
int
terminal_display_draw(
	struct terminal_display *display,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window)
{
	VkResult result;
	int error;

	/* The frame. */
	result = terminal_renderer_draw(&display->renderer, screen, font, window);
	error = display_error(display, result);
	if (error != 0)
		return error;

	/* Succeeded: the grid is on the window. */
	return 0;
}

/*
 * Shows a frame painted elsewhere (width by height pixels, a row after
 * another, premultiplied 0xAARRGGBB) in the grid's place, the swapchain's
 * size.
 *
 * Returns 0, EAGAIN when the swapchain no longer matches the window or
 * the frame is not its size (the caller resizes and draws again), or EIO.
 */
int
terminal_display_page(
	struct terminal_display *display,
	const uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	VkResult result;
	int error;

	/* A frame of another size than the swapchain's is drawn again at its size. */
	if (width != display->renderer.extent.width || height != display->renderer.extent.height)
		return EAGAIN;

	/* The frame. */
	result = terminal_renderer_page(&display->renderer, pixels, width, height);
	error = display_error(display, result);
	if (error != 0)
		return error;

	/* Succeeded: the page is on the window. */
	return 0;
}

/*
 * Waits for the frame drawn last: after this the font may change the
 * atlas's pixels.  Returns 0 or EIO.
 */
int
terminal_display_idle(
	struct terminal_display *display)
{
	VkResult result;
	int error;

	/* The frame's fence. */
	result = terminal_renderer_idle(&display->renderer);
	error = display_error(display, result);
	if (error != 0)
		return error;

	/* Succeeded: no frame reads the atlas now. */
	return 0;
}

/*
 * Gives the size the frames are drawn at: the swapchain's.
 */
void
terminal_display_size(
	const struct terminal_display *display,
	uint32_t *width,
	uint32_t *height)
{
	/* The swapchain's extent. */
	*width = display->renderer.extent.width;
	*height = display->renderer.extent.height;
}

/*
 * Names the Vulkan call that failed last ("none" before one ran) and gives
 * its result, for the error line.
 */
const char *
terminal_display_operation(
	const struct terminal_display *display,
	int *result)
{
	/* No display was made: nothing ran. */
	if (display == NULL) {
		*result = (int)VK_SUCCESS;
		return "malloc";
	}

	/* The call and its result. */
	*result = (int)display->result;
	return display->renderer.operation;
}

/*
 * Releases every Vulkan object and the display.
 */
void
terminal_display_close(
	struct terminal_display *display)
{
	/* No display was made. */
	if (display == NULL)
		return;

	/* The Vulkan objects, children before their parents, then the display. */
	terminal_renderer_close(&display->renderer);
	free(display);
}

/* Keeps a Vulkan result and reports it as 0, EAGAIN (out of date) or EIO. */
static int
display_error(
	struct terminal_display *display,
	VkResult result)
{
	/* The result the error line names. */
	display->result = result;

	/* A swapchain the window no longer matches is made again by the caller. */
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
		return EAGAIN;

	/* Any other failure. */
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded. */
	return 0;
}
