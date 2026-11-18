/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The windows, pixmaps and graphics contexts, and drawing into them.
 *
 * A window keeps its own pixels, so it can be shown again without its
 * client; positions are on the root window.  What changes is gathered
 * into one box (x11_mark_dirty) that rootless.c shows at the end of a
 * pass.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The colour text is drawn in without a graphics context. */
#define WINDOW_TEXT_COLOR	0xffffffU

static void window_remove(struct x11server *server, struct x11_window *window);

/*
 * Returns the window of an id, or NULL.
 */
struct x11_window *
x11_window_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;
	unsigned slot;

	/* The window of the id, when the table has one. */
	window = NULL;
	for (slot = 0U; slot < server->window_count; slot++) {
		if (server->windows[slot].id == id) {
			window = &server->windows[slot];
			break;
		}
	}

	/* No such window. */
	if (window == NULL)
		return NULL;

	/* Succeeded: the window. */
	return window;
}

/*
 * Returns the pixmap of an id, or NULL.
 */
struct x11_pixmap *
x11_pixmap_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_pixmap *pixmap;
	unsigned slot;

	/* The pixmap of the id, when the table has one. */
	pixmap = NULL;
	for (slot = 0U; slot < server->pixmap_count; slot++) {
		if (server->pixmaps[slot].id == id) {
			pixmap = &server->pixmaps[slot];
			break;
		}
	}

	/* No such pixmap. */
	if (pixmap == NULL)
		return NULL;

	/* Succeeded: the pixmap. */
	return pixmap;
}

/*
 * Returns the graphics context of an id, or NULL.
 */
struct x11_gc *
x11_gc_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_gc *gc;
	unsigned slot;

	/* The context of the id, when the table has one. */
	gc = NULL;
	for (slot = 0U; slot < server->gc_count; slot++) {
		if (server->gcs[slot].id == id) {
			gc = &server->gcs[slot];
			break;
		}
	}

	/* No such context. */
	if (gc == NULL)
		return NULL;

	/* Succeeded: the context. */
	return gc;
}

/*
 * Allocates pixels of a size, each of a colour.  Returns NULL for an empty
 * size or when there is no memory.
 */
uint32_t *
x11_pixels_alloc(
	uint16_t width,
	uint16_t height,
	uint32_t color)
{
	uint32_t *pixels;
	size_t count;
	size_t index;

	/* An empty size has no pixels. */
	if (width == 0U || height == 0U)
		return NULL;

	/* The memory. */
	count = (size_t)width * (size_t)height;
	pixels = malloc(count * sizeof(*pixels));
	if (pixels == NULL)
		return NULL;

	/* Each pixel of the colour. */
	for (index = 0U; index < count; index++)
		pixels[index] = color;

	/* Succeeded: the pixels. */
	return pixels;
}

/*
 * Gives a window pixels of a new size, keeping the part that fits and
 * clearing the rest to its background.  Returns 0, or -1 without memory.
 */
int
x11_window_resize(
	struct x11_window *window,
	uint16_t width,
	uint16_t height)
{
	uint32_t *pixels;
	unsigned copy_width;
	unsigned copy_height;
	unsigned row;

	/* The same size keeps its pixels. */
	if (width == window->width && height == window->height)
		return 0;

	/* The new pixels. */
	pixels = x11_pixels_alloc(width, height, window->background);
	if (pixels == NULL)
		return -1;

	/* The part of the old ones that fits, row by row. */
	copy_width = width;
	if (copy_width > window->width)
		copy_width = window->width;
	copy_height = height;
	if (copy_height > window->height)
		copy_height = window->height;
	for (row = 0U; row < copy_height && window->pixels != NULL; row++)
		memcpy(pixels + (size_t)row * width, window->pixels + (size_t)row * window->width, (size_t)copy_width * sizeof(*pixels));

	/* The old pixels go. */
	free(window->pixels);
	window->pixels = pixels;

	/* Succeeded: the window has pixels of the new size. */
	return 0;
}

/*
 * Destroys a window and its descendants: their owners (and the parent's,
 * when it watches its children) are told, and a top-level window's
 * desktop window closes.  The root window is never destroyed.
 */
void
x11_window_destroy(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;
	struct x11_window *parent;
	uint32_t child;
	unsigned slot;

	/* The root window and an unknown id are left alone. */
	if (id == X11_ROOT_XID)
		return;
	window = x11_window_find(server, id);
	if (window == NULL)
		return;

	/* Each child first, so that no window is left with a dead parent. */
	for (;;) {
		child = 0U;
		for (slot = 1U; slot < server->window_count; slot++) {
			if (server->windows[slot].parent == id) {
				child = server->windows[slot].id;
				break;
			}
		}

		/* No child is left. */
		if (child == 0U)
			break;
		x11_window_destroy(server, child);
	}

	/* The window again (the table moved under the children). */
	window = x11_window_find(server, id);
	if (window == NULL)
		return;

	/* Its owner, and its parent's owner when it watches its children, are told. */
	if ((window->event_mask & X11_MASK_STRUCTURE_NOTIFY) != 0U)
		x11_destroy_notify(server, window->owner, window->id, window->id);
	parent = x11_window_find(server, window->parent);
	if (parent != NULL && (parent->event_mask & X11_MASK_SUBSTRUCTURE_NOTIFY) != 0U)
		x11_destroy_notify(server, parent->owner, parent->id, window->id);

	/* What was under it is shown again. */
	x11_mark_dirty(server, window->x, window->y, window->width, window->height);

	/* The focus and a grab on it go back to the root window and to nothing. */
	if (server->focus == window->id)
		server->focus = X11_ROOT_XID;
	if (server->grab_window == window->id) {
		server->grab_owner = -1;
		server->grab_window = 0U;
	}

	/* Its properties and the selections it owned (selection.c). */
	x11_selection_forget_window(server, window->id);

	/* Its pixels, its desktop window, and its slot. */
	window_remove(server, window);
}

/*
 * Puts a window on top of the stack (the root window stays at the bottom).
 */
void
x11_window_raise(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_window saved;
	size_t slot;

	/* The root window and a window already on top stay. */
	if (window == NULL || window == &server->windows[0])
		return;
	slot = (size_t)(window - server->windows);
	if (slot >= server->window_count || slot + 1U == server->window_count)
		return;

	/* The later windows move down, and the window goes last. */
	saved = *window;
	memmove(&server->windows[slot], &server->windows[slot + 1U], (server->window_count - slot - 1U) * sizeof(server->windows[0]));
	server->windows[server->window_count - 1U] = saved;

	/* Its place is shown again. */
	x11_mark_dirty(server, saved.x, saved.y, saved.width, saved.height);
}

/*
 * Returns the top-level window (the root window's child) a window is in.
 */
struct x11_window *
x11_window_top_level(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_window *parent;

	/* Up the tree to the root window's child. */
	while (window != NULL && window->parent != X11_ROOT_XID) {
		parent = x11_window_find(server, window->parent);
		if (parent == NULL || parent == window)
			break;
		window = parent;
	}

	/* Succeeded: the top-level window (or the one with no parent left). */
	return window;
}

/*
 * Returns the deepest mapped window under a place on the root window, or
 * the root window.
 */
struct x11_window *
x11_window_at(
	struct x11server *server,
	int x,
	int y)
{
	struct x11_window *window;

	/* The deepest window there. */
	window = x11_window_child_at(server, X11_ROOT_XID, x, y);
	if (window == NULL)
		return &server->windows[0];

	/* Succeeded: that window. */
	return window;
}

/*
 * Returns the deepest mapped descendant of a window under a place on the
 * root window, the topmost of its siblings; NULL when none is there.
 */
struct x11_window *
x11_window_child_at(
	struct x11server *server,
	uint32_t parent,
	int x,
	int y)
{
	struct x11_window *window;
	struct x11_window *child;
	struct x11_window *found;
	unsigned slot;

	/* The topmost child under the point: the children from the top of the stack down. */
	found = NULL;
	slot = server->window_count;
	while (slot > 1U && found == NULL) {
		slot--;
		window = &server->windows[slot];
		if (!window->mapped || window->parent != parent)
			continue;
		if (x < window->x ||
		    y < window->y ||
		    x >= window->x + window->width ||
		    y >= window->y + window->height)
			continue;

		/* The deepest of its own children there, or the child itself. */
		child = x11_window_child_at(server, window->id, x, y);
		found = window;
		if (child != NULL)
			found = child;
	}

	/* No child is there. */
	if (found == NULL)
		return NULL;

	/* Succeeded: the deepest window under the point. */
	return found;
}

/*
 * Fills a rectangle of a window (in its own coordinates) with a colour,
 * clipped to the window.
 */
void
x11_window_fill(
	struct x11server *server,
	struct x11_window *window,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row;
	int column;

	/* Clipped to the window. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Clipped at the top. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Clipped at the right and the bottom. */
	if (x + width > window->width)
		width = window->width - x;
	if (y + height > window->height)
		height = window->height - y;
	if (width <= 0 || height <= 0)
		return;

	/* Each pixel. */
	for (row = y; row < y + height; row++) {
		for (column = x; column < x + width; column++)
			window->pixels[(size_t)row * window->width + (size_t)column] = color;
	}

	/* Shown again. */
	x11_mark_dirty(server, window->x + x, window->y + y, width, height);
}

/*
 * Fills a rectangle of a pixmap with a colour, clipped to the pixmap.
 */
void
x11_pixmap_fill(
	struct x11_pixmap *pixmap,
	int x,
	int y,
	int width,
	int height,
	uint32_t color)
{
	int row;
	int column;

	/* Clipped to the pixmap. */
	if (x < 0) {
		width += x;
		x = 0;
	}

	/* Clipped at the top. */
	if (y < 0) {
		height += y;
		y = 0;
	}

	/* Clipped at the right and the bottom. */
	if (x + width > pixmap->width)
		width = pixmap->width - x;
	if (y + height > pixmap->height)
		height = pixmap->height - y;
	if (width <= 0 || height <= 0)
		return;

	/* Each pixel. */
	for (row = y; row < y + height; row++) {
		for (column = x; column < x + width; column++)
			pixmap->pixels[(size_t)row * pixmap->width + (size_t)column] = color;
	}
}

/*
 * Draws characters (bytes, or big-endian pairs when wide) in the core font
 * onto a window, the cells standing on a baseline at y.
 */
void
x11_draw_text(
	struct x11server *server,
	struct x11_window *window,
	const struct x11_gc *gc,
	int x,
	int y,
	const uint8_t *text,
	size_t count,
	int wide)
{
	struct x11_glyph glyph;
	uint32_t codepoint;
	uint32_t color;
	size_t index;
	unsigned bits;
	int glyph_x;
	int glyph_y;
	int pixel_x;
	int pixel_y;
	int top;
	int missing;

	/* The context's colour. */
	color = WINDOW_TEXT_COLOR;
	if (gc != NULL)
		color = gc->foreground;

	/* Each character. */
	for (index = 0U; index < count; index++) {
		codepoint = text[index];
		if (wide)
			codepoint = ((uint32_t)text[index * 2U] << 8) | (uint32_t)text[index * 2U + 1U];

		/* Its glyph; a character without one is skipped. */
		missing = x11_glyph(&server->glyphs, codepoint, &glyph);
		if (missing != 0)
			continue;

		/* Each set bit of the cell that lands in the window. */
		top = y - (int)glyph.height;
		for (glyph_y = 0; glyph_y < (int)glyph.height; glyph_y++) {
			for (glyph_x = 0; glyph_x < (int)glyph.width; glyph_x++) {
				pixel_x = x + glyph_x;
				pixel_y = top + glyph_y;
				if (pixel_x < 0 ||
				    pixel_y < 0 ||
				    pixel_x >= window->width ||
				    pixel_y >= window->height)
					continue;
				bits = glyph.bitmap[(size_t)glyph_y * glyph.stride + (unsigned)glyph_x / 8U];
				if ((bits & (0x80U >> ((unsigned)glyph_x & 7U))) != 0U)
					window->pixels[(size_t)pixel_y * window->width + (size_t)pixel_x] = color;
			}
		}

		/* The cell is shown again, and the pen moves on. */
		x11_mark_dirty(server, window->x + x, window->y + top, (int)glyph.width, (int)glyph.height);
		x += (int)glyph.advance;
	}
}

/*
 * Adds a rectangle on the root window to what has changed.
 */
void
x11_mark_dirty(
	struct x11server *server,
	int x,
	int y,
	int width,
	int height)
{
	/* An empty rectangle changes nothing. */
	if (width <= 0 || height <= 0)
		return;

	/* The first change is the box. */
	if (!server->dirty) {
		server->dirty = 1;
		server->dirty_x0 = x;
		server->dirty_y0 = y;
		server->dirty_x1 = x + width;
		server->dirty_y1 = y + height;
		return;
	}

	/* A later one widens it. */
	if (x < server->dirty_x0)
		server->dirty_x0 = x;
	if (y < server->dirty_y0)
		server->dirty_y0 = y;
	if (x + width > server->dirty_x1)
		server->dirty_x1 = x + width;
	if (y + height > server->dirty_y1)
		server->dirty_y1 = y + height;
}

/*
 * Releases every window, pixmap, graphics context and font a client owns.
 */
void
x11_resources_release(
	struct x11server *server,
	unsigned owner)
{
	uint32_t id;
	unsigned slot;

	/* Its windows (each with its descendants), one at a time as the table changes. */
	for (;;) {
		id = 0U;
		for (slot = 1U; slot < server->window_count; slot++) {
			if (server->windows[slot].owner == owner) {
				id = server->windows[slot].id;
				break;
			}
		}

		/* No window of its own is left. */
		if (id == 0U)
			break;
		x11_window_destroy(server, id);
	}

	/* The root window's events it selected go back to no one. */
	if (server->windows[0].owner == owner) {
		server->windows[0].owner = X11_NO_CLIENT;
		server->windows[0].event_mask = 0U;
	}

	/* Its pixmaps, with their pixels. */
	slot = 0U;
	while (slot < server->pixmap_count) {
		if (server->pixmaps[slot].owner != owner) {
			slot++;
			continue;
		}

		/* Its pixels, and the later pixmaps move down over it. */
		free(server->pixmaps[slot].pixels);
		memmove(&server->pixmaps[slot], &server->pixmaps[slot + 1U], (server->pixmap_count - slot - 1U) * sizeof(server->pixmaps[0]));
		server->pixmap_count--;
		memset(&server->pixmaps[server->pixmap_count], 0, sizeof(server->pixmaps[0]));
	}

	/* Its graphics contexts. */
	slot = 0U;
	while (slot < server->gc_count) {
		if (server->gcs[slot].owner != owner) {
			slot++;
			continue;
		}

		/* The later contexts move down over it. */
		memmove(&server->gcs[slot], &server->gcs[slot + 1U], (server->gc_count - slot - 1U) * sizeof(server->gcs[0]));
		server->gc_count--;
		memset(&server->gcs[server->gc_count], 0, sizeof(server->gcs[0]));
	}

	/* Its fonts. */
	slot = 0U;
	while (slot < server->font_count) {
		if (server->fonts[slot].owner != owner) {
			slot++;
			continue;
		}

		/* The later fonts move down over it. */
		memmove(&server->fonts[slot], &server->fonts[slot + 1U], (server->font_count - slot - 1U) * sizeof(server->fonts[0]));
		server->font_count--;
		memset(&server->fonts[server->font_count], 0, sizeof(server->fonts[0]));
	}
}

/* Frees a window's pixels and desktop window and takes it out of the table. */
static void
window_remove(
	struct x11server *server,
	struct x11_window *window)
{
	size_t slot;

	/* Its pixels, its desktop window and its composite. */
	free(window->pixels);
	window->pixels = NULL;
	x11_rootless_forget(window);

	/* The later windows move down over it. */
	slot = (size_t)(window - server->windows);
	memmove(&server->windows[slot], &server->windows[slot + 1U], (server->window_count - slot - 1U) * sizeof(server->windows[0]));
	server->window_count--;
	memset(&server->windows[server->window_count], 0, sizeof(server->windows[0]));
}
