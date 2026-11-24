/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Showing a frame on Android: the frame is premultiplied 0xAARRGGBB words
 * the program drew on the processor, and the native window takes bytes in
 * the order red, green, blue, alpha.  The parts that changed are written
 * into the window's buffer (a lock that tells what must be written, a
 * write, and a post), at the place the insets leave to the logical window.
 * The title bar's strip (titlebar.c) is the rows above the frame.
 */

#include "apps/libkeiland/android/android.h"

#include <android/native_window.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The system's format of the buffer: four bytes a pixel, red first. */
#define PRESENT_FORMAT	WINDOW_FORMAT_RGBA_8888

/* The native window whose format was set last (the format is set again for another). */
static ANativeWindow *present_native;

/* Where the last frame was put in the native window: the window, the frame's offset and size. */
static ANativeWindow *present_laid;
static int32_t present_laid_left;
static int32_t present_laid_top;
static uint32_t present_laid_width;
static uint32_t present_laid_height;
static uint32_t present_laid_strip;

/* What copying a frame needs of the buffer and the frame. */
struct present_view {
	uint8_t *bits;
	int32_t stride;
	const uint32_t *pixels;
	size_t pixels_stride;
	const uint32_t *strip;
	uint32_t strip_height;
	uint32_t width;
	int32_t left;
	int32_t top;
};

static int present_keep(struct kl_window *window, const uint32_t *pixels, size_t stride, const struct kl_rect *parts, unsigned count);
static void present_gather(const struct kl_window *window, struct android_view *view, int strip_dirty);
static void present_fill(uint8_t *bits, int32_t stride, const ARect *area, const ARect *keep);
static void present_rows(const ARect *area, const struct present_view *view);

/*
 * Reports the size of the content to draw: the window's.
 */
int
kl_window_present_resize(
	struct kl_window *window,
	uint32_t *width,
	uint32_t *height)
{
	/* A window and the places for the size. */
	if (window == NULL || width == NULL || height == NULL)
		return EINVAL;

	/* The size the window has now. */
	*width = window->width;
	*height = window->height;

	/* Succeeded. */
	return 0;
}

/*
 * Shows a whole frame (stride words a row).
 */
int
kl_window_present(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride)
{
	int error;

	/* The parts' way with none named: all of it. */
	error = kl_window_present_parts(window, pixels, stride, NULL, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Shows a frame of which one part changed (NULL: the whole frame).
 */
int
kl_window_present_part(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *part)
{
	int error;

	/* One part, or none. */
	error = kl_window_present_parts(window, pixels, stride, part, part == NULL ? 0U : 1U);
	if (error != 0)
		return error;

	/* Succeeded: the part is shown. */
	return 0;
}

/*
 * Shows a frame of which some parts changed (count 0: the whole frame).  A
 * window that is not the one shown, or while nothing can be drawn in (no
 * native window, the Activity stopped), keeps the frame and shows nothing: the
 * frame is painted when it can be (android_present_repaint).
 */
int
kl_window_present_parts(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *parts,
	unsigned count)
{
	struct android_view view;
	struct kl_app *app;
	struct kl_rect whole;
	int error;

	/* A window and a frame. */
	if (window == NULL || pixels == NULL)
		return EINVAL;
	app = window->app;

	/* No part named is the whole frame. */
	if (count == 0U || parts == NULL) {
		whole.x = 0;
		whole.y = 0;
		whole.width = (int)window->width;
		whole.height = (int)window->height;
		parts = &whole;
		count = 1U;
	}

	/* The frame is kept, so that a native window made later (the Activity made again) can be painted without the program. */
	error = present_keep(window, pixels, stride, parts, count);
	if (error != 0)
		return error;

	/* Nothing to draw in, or not the window shown: the frame is painted when it can be. */
	if (app->native == NULL || !app->started || android_window_top(app) != window)
		return 0;

	/* Written and posted. */
	present_gather(window, &view, 0);
	error = android_present_copy(app->native, app->insets, &view, parts, count);
	if (error != 0)
		return error;

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Paints the window on top again from the frame kept: the native window a new
 * Activity gives has no picture, and a program draws again only for a change.
 */
void
android_present_repaint(
	struct kl_app *app)
{
	struct android_view view;
	struct kl_rect whole;
	struct kl_window *window;
	unsigned count;

	/* A window that can be drawn in. */
	window = android_window_top(app);
	if (window == NULL || app->native == NULL || !app->started)
		return;

	/* A frame of its size to paint from, or the strip alone. */
	present_gather(window, &view, 1);
	count = 1U;
	if (window->shown == NULL || window->shown_width != window->width || window->shown_height != window->height) {
		view.pixels = NULL;
		count = 0U;
		if (view.strip == NULL)
			return;
	}

	/* All of it. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)window->width;
	whole.height = (int)window->height;
	(void)android_present_copy(app->native, app->insets, &view, &whole, count);
}

/*
 * Shows the window's strip again: only the strip is written, when the
 * window is the one shown and the native window can be drawn in.
 */
void
android_present_strip(
	struct kl_window *window)
{
	struct android_view view;
	struct kl_app *app;

	/* A strip, a window that is shown and can be drawn in. */
	app = window->app;
	if (window->bar.pixels == NULL || app->native == NULL || !app->started || android_window_top(app) != window)
		return;

	/* Only the strip is dirty; the frame is not rewritten. */
	present_gather(window, &view, 1);
	(void)android_present_copy(app->native, app->insets, &view, NULL, 0U);
}

/*
 * Lets the frame kept go.
 */
void
android_present_release(
	struct kl_window *window)
{
	/* The memory and its size. */
	free(window->shown);
	window->shown = NULL;
	window->shown_width = 0U;
	window->shown_height = 0U;
}

/*
 * Android has no Vulkan loader for the program: the instance extensions a
 * surface needs are none (kl_window_vulkan_surface is not made, as no program
 * of this layer draws with Vulkan).
 */
int
kl_window_vulkan_extensions(
	const char *const **names,
	uint32_t *count)
{
	/* The places for the answer. */
	if (names == NULL || count == NULL)
		return EINVAL;

	/* No loader to ask. */
	return EIO;
}

/*
 * Writes the parts of a frame into the native window's buffer and posts it.
 * The lock tells which area must be written: the one asked for, or more when
 * the buffer it hands out is older than the last frame.
 */
int
android_present_copy(
	ANativeWindow *native,
	const int32_t insets[5],
	const struct android_view *view,
	const struct kl_rect *parts,
	unsigned count)
{
	ANativeWindow_Buffer buffer;
	struct present_view source;
	ARect asked;
	ARect dirty;
	ARect frame;
	unsigned index;
	uint32_t strip;
	int32_t left;
	int32_t top;
	int32_t bottom;
	int32_t right;
	int32_t x;
	int32_t y;
	int widened;
	int moved;
	int status;

	/* The format, set again only for another window. */
	if (present_native != native) {
		status = ANativeWindow_setBuffersGeometry(native, 0, 0, PRESENT_FORMAT);
		if (status != 0)
			return EIO;
		present_native = native;
	}

	/* The strip above the frame, when there is one. */
	strip = 0U;
	if (view->strip != NULL)
		strip = view->strip_height;

	/*
	 * Where the frame lies in the native window (its place, its size, the strip over it and the native window itself): when it
	 * is not where the last one was, the bands around it hold what the last one left, so the whole native
	 * window is written, the frame in full and the bands black.
	 */
	moved = (present_laid != native || present_laid_left != insets[ANDROID_INSET_LEFT] ||
	    present_laid_top != insets[ANDROID_INSET_TOP] || present_laid_width != view->width || present_laid_height != view->height ||
	    present_laid_strip != strip);
	present_laid = native;
	present_laid_left = insets[ANDROID_INSET_LEFT];
	present_laid_top = insets[ANDROID_INSET_TOP];
	present_laid_width = view->width;
	present_laid_height = view->height;
	present_laid_strip = strip;

	/* The area the parts cover (below the strip), and the strip when it is dirty. */
	left = 0x7fffffff;
	top = 0x7fffffff;
	right = 0;
	bottom = 0;
	for (index = 0U; index < count; index++) {
		x = parts[index].x;
		y = parts[index].y;
		if (x < 0)
			x = 0;
		if (y < 0)
			y = 0;
		if (x < left)
			left = x;
		if ((int32_t)strip + y < top)
			top = (int32_t)strip + y;
		if (parts[index].x + parts[index].width > right)
			right = parts[index].x + parts[index].width;
		if ((int32_t)strip + parts[index].y + parts[index].height > bottom)
			bottom = (int32_t)strip + parts[index].y + parts[index].height;
	}
	if (strip != 0U && (view->strip_dirty || moved)) {
		if (left > 0)
			left = 0;
		if (right < (int32_t)view->width)
			right = (int32_t)view->width;
		top = 0;
		if (bottom < (int32_t)strip)
			bottom = (int32_t)strip;
	}
	if (right > (int32_t)view->width)
		right = (int32_t)view->width;
	if (bottom > (int32_t)(strip + view->height))
		bottom = (int32_t)(strip + view->height);
	if (moved) {
		left = 0;
		top = 0;
		right = (int32_t)view->width;
		bottom = (int32_t)(strip + view->height);
	}
	if (left >= right || top >= bottom)
		return 0;
	asked.left = left + insets[ANDROID_INSET_LEFT];
	asked.top = top + insets[ANDROID_INSET_TOP];
	asked.right = right + insets[ANDROID_INSET_LEFT];
	asked.bottom = bottom + insets[ANDROID_INSET_TOP];
	if (moved) {
		asked.left = 0;
		asked.top = 0;
		asked.right = ANativeWindow_getWidth(native);
		asked.bottom = ANativeWindow_getHeight(native);
	}

	/* The buffer, and the area to write (the one asked for, or wider). */
	dirty = asked;
	status = ANativeWindow_lock(native, &buffer, &dirty);
	if (status != 0)
		return EIO;
	widened = (moved || dirty.left != asked.left || dirty.top != asked.top || dirty.right != asked.right ||
	    dirty.bottom != asked.bottom);

	/* The strip and the frame's place in the buffer. */
	frame.left = insets[ANDROID_INSET_LEFT];
	frame.top = insets[ANDROID_INSET_TOP];
	frame.right = frame.left + (int32_t)view->width;
	frame.bottom = frame.top + (int32_t)strip + (int32_t)view->height;

	/* Clipped to the buffer. */
	if (dirty.left < 0)
		dirty.left = 0;
	if (dirty.top < 0)
		dirty.top = 0;
	if (dirty.right > buffer.width)
		dirty.right = buffer.width;
	if (dirty.bottom > buffer.height)
		dirty.bottom = buffer.height;
	if (frame.right > buffer.width)
		frame.right = buffer.width;
	if (frame.bottom > buffer.height)
		frame.bottom = buffer.height;

	/* The bands the frame does not cover are black, written when the area is wider than what changed. */
	if (widened)
		present_fill(buffer.bits, buffer.stride, &dirty, &frame);

	/* The rows in the area: the strip's, then the frame's. */
	source.bits = buffer.bits;
	source.stride = buffer.stride;
	source.pixels = view->pixels;
	source.pixels_stride = view->stride;
	source.strip = view->strip;
	source.strip_height = strip;
	source.width = view->width;
	source.left = frame.left;
	source.top = frame.top;
	if (dirty.left < frame.left)
		dirty.left = frame.left;
	if (dirty.top < frame.top)
		dirty.top = frame.top;
	if (dirty.right > frame.right)
		dirty.right = frame.right;
	if (dirty.bottom > frame.bottom)
		dirty.bottom = frame.bottom;
	present_rows(&dirty, &source);

	/* Posted. */
	status = ANativeWindow_unlockAndPost(native);
	if (status != 0)
		return EIO;

	/* Succeeded: the parts are shown. */
	return 0;
}

/*
 * Turns premultiplied 0xAARRGGBB words into bytes: red, green, blue and an
 * opaque alpha (the window is opaque, and its colours are as they are).
 */
void
android_pixels_row(
	uint8_t *out,
	const uint32_t *in,
	uint32_t count)
{
	uint32_t index;
	uint32_t word;

	/* Each pixel. */
	for (index = 0U; index < count; index++) {
		word = in[index];
		out[index * 4U + 0U] = (uint8_t)(word >> 16);
		out[index * 4U + 1U] = (uint8_t)(word >> 8);
		out[index * 4U + 2U] = (uint8_t)word;
		out[index * 4U + 3U] = 0xFFU;
	}
}

/* Paints the part of an area that a kept rectangle does not cover black (opaque). */
static void
present_fill(
	uint8_t *bits,
	int32_t stride,
	const ARect *area,
	const ARect *keep)
{
	int32_t y;
	int32_t x;
	uint8_t *row;
	int inside_rows;

	/* Each row of the area. */
	for (y = area->top; y < area->bottom; y++) {
		row = bits + (size_t)y * (size_t)stride * 4U;
		inside_rows = (y >= keep->top && y < keep->bottom);
		for (x = area->left; x < area->right; x++) {
			if (inside_rows && x >= keep->left && x < keep->right) {
				x = keep->right - 1;
				continue;
			}
			row[(size_t)x * 4U + 0U] = 0U;
			row[(size_t)x * 4U + 1U] = 0U;
			row[(size_t)x * 4U + 2U] = 0U;
			row[(size_t)x * 4U + 3U] = 0xFFU;
		}
	}
}

/* Writes the rows of an area of the buffer: a row of the strip's height is the strip's, the rest the frame's (the frame's place in the buffer is view->left, view->top). */
static void
present_rows(
	const ARect *area,
	const struct present_view *view)
{
	int32_t y;
	int32_t width;
	int32_t row;
	const uint32_t *source;
	uint8_t *target;

	/* An empty area. */
	if (area->left >= area->right || area->top >= area->bottom)
		return;
	width = area->right - area->left;

	/* Each row. */
	for (y = area->top; y < area->bottom; y++) {
		row = y - view->top;
		if ((uint32_t)row < view->strip_height)
			source = view->strip + (size_t)row * view->width + (size_t)(area->left - view->left);
		else if (view->pixels != NULL)
			source = view->pixels + (size_t)((uint32_t)row - view->strip_height) * view->pixels_stride + (size_t)(area->left - view->left);
		else
			continue;
		target = view->bits + ((size_t)y * (size_t)view->stride + (size_t)area->left) * 4U;
		android_pixels_row(target, source, (uint32_t)width);
	}
}

/* Gathers what is shown of a window: its strip (when it has one) and the frame kept. */
static void
present_gather(
	const struct kl_window *window,
	struct android_view *view,
	int strip_dirty)
{
	/* The strip above the frame. */
	view->strip = window->bar.pixels;
	view->strip_height = window->bar.height;
	view->strip_dirty = strip_dirty;

	/* The frame kept, which is of the window's size once the program drew. */
	view->pixels = window->shown;
	view->stride = window->width;
	view->width = window->width;
	view->height = window->height;
}

/* Copies the parts of a frame into the frame kept (made again, black, when the window's size changed).  Returns 0 or ENOMEM. */
static int
present_keep(
	struct kl_window *window,
	const uint32_t *pixels,
	size_t stride,
	const struct kl_rect *parts,
	unsigned count)
{
	unsigned index;
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
	int32_t y;

	/* A frame of the window's size. */
	if (window->shown == NULL || window->shown_width != window->width || window->shown_height != window->height) {
		android_present_release(window);
		window->shown = calloc((size_t)window->width * (size_t)window->height, sizeof(uint32_t));
		if (window->shown == NULL)
			return ENOMEM;
		window->shown_width = window->width;
		window->shown_height = window->height;
	}

	/* Each part's rows. */
	for (index = 0U; index < count; index++) {
		left = parts[index].x < 0 ? 0 : parts[index].x;
		top = parts[index].y < 0 ? 0 : parts[index].y;
		right = parts[index].x + parts[index].width;
		bottom = parts[index].y + parts[index].height;
		if (right > (int32_t)window->width)
			right = (int32_t)window->width;
		if (bottom > (int32_t)window->height)
			bottom = (int32_t)window->height;
		for (y = top; y < bottom && left < right; y++) {
			memcpy(
				window->shown + (size_t)y * window->width + (size_t)left,
				pixels + (size_t)y * stride + (size_t)left,
				(size_t)(right - left) * sizeof(uint32_t));
		}
	}

	/* Succeeded. */
	return 0;
}
