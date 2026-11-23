/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Cursors: their requests, and the one the backend shows.
 *
 * A cursor is a shape of two colours with a mask and a hot spot, made from
 * two bitmaps (CreateCursor) or two glyphs (CreateGlyphCursor).  A glyph
 * of the cursor font is one of the standard cursors: it is kept as that
 * shape's number and the backend shows its own cursor of the shape (the
 * server has no images of them).  A cursor lives while its id does or a
 * window or a grab names it.
 *
 * While the pointer is over an X window, the cursor shown is the active
 * pointer grab's, else the window's, else its nearest ancestor's that
 * names one (none: the system's pointer).  At each tick the backend is
 * told when that changed for the surface under the pointer.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The requests answered here. */
#define REQUEST_CREATE_CURSOR		93U
#define REQUEST_CREATE_GLYPH_CURSOR	94U
#define REQUEST_FREE_CURSOR		95U
#define REQUEST_RECOLOR_CURSOR		96U

/* The largest cursor made from bitmaps or glyphs, each way. */
#define CURSOR_SIZE_MAX			256

/* An opaque pixel of a cursor's image, and a clear one. */
#define CURSOR_OPAQUE			0xff000000U
#define CURSOR_CLEAR			0x00000000U

/* The version the backend is told for the cursor hidden by a client (XFIXES's HideCursor); no cursor has it. */
#define CURSOR_HIDDEN_VERSION		0xffffffffU

static unsigned cursor_create(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned cursor_create_glyph(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned cursor_free_request(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned cursor_recolor(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned cursor_check_id(struct x11server *server, unsigned index, uint32_t id);
static unsigned cursor_add(struct x11server *server, unsigned index, uint32_t id, struct x11_cursor *cursor);
static struct x11_cursor *cursor_new(int width, int height);
static void cursor_colors(struct x11server *server, struct x11_cursor *cursor, const uint8_t *bytes, int msb);
static const struct x11_cursor *cursor_shown(struct x11server *server, struct x11_window *window);
static uint32_t *cursor_image(const struct x11_cursor *cursor);
static void cursor_set_bit(uint8_t *bits, size_t stride, int x, int y);
static int cursor_bit(const uint8_t *bits, size_t stride, int x, int y);

/*
 * Answers a request of cursors: CreateCursor, CreateGlyphCursor,
 * FreeCursor and RecolorCursor.  Returns 0, or the error to send.
 */
unsigned
x11_cursor_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	unsigned code;

	/* The request's own handler. */
	switch (request[0]) {
	case REQUEST_CREATE_CURSOR:
		code = cursor_create(server, index, request, length);
		break;
	case REQUEST_CREATE_GLYPH_CURSOR:
		code = cursor_create_glyph(server, index, request, length);
		break;
	case REQUEST_FREE_CURSOR:
		code = cursor_free_request(server, index, request, length);
		break;
	case REQUEST_RECOLOR_CURSOR:
		code = cursor_recolor(server, index, request, length);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Finds a cursor by its id.  Returns it, or NULL.
 */
struct x11_cursor *
x11_cursor_find(
	struct x11server *server,
	uint32_t id)
{
	struct x11_cursor *cursor;

	/* The id's resource, when it is a cursor. */
	cursor = x11_resource_find(server, id, X11_RESOURCE_CURSOR);
	if (cursor == NULL)
		return NULL;

	/* Succeeded: the cursor. */
	return cursor;
}

/*
 * Holds a cursor for a window or a grab that names it.
 */
void
x11_cursor_hold(
	struct x11_cursor *cursor)
{
	/* One more holder: the cursor lives on until each lets it go. */
	cursor->holders++;
}

/*
 * Lets go of a cursor held (NULL: none); the last holder frees it.
 */
void
x11_cursor_release(
	struct x11server *server,
	struct x11_cursor *cursor)
{
	/* Nothing held. */
	if (cursor == NULL)
		return;

	/* One fewer holder; none left frees its bitmaps, its charge and it. */
	cursor->holders--;
	if (cursor->holders != 0U)
		return;
	free(cursor->source);
	free(cursor->mask);
	x11_memory_release(server, cursor->owner, cursor->charged);
	free(cursor);
}

/*
 * Lets go of a cursor's id (out of the resource ids already).  A cursor a
 * window still names lives on, charged to the server from then.
 */
void
x11_cursor_free(
	struct x11server *server,
	struct x11_cursor *cursor)
{
	int error;

	/* Still named by a window: its charge moves from the client to the server. */
	cursor->id = 0U;
	if (cursor->holders > 1U && cursor->owner != X11_NO_CLIENT) {
		x11_memory_release(server, cursor->owner, cursor->charged);
		cursor->owner = X11_NO_CLIENT;
		error = x11_memory_charge(server, X11_NO_CLIENT, cursor->charged);
		if (error != 0)
			cursor->charged = 0U;
	}

	/* The id's hold. */
	x11_cursor_release(server, cursor);
}

/*
 * Tells the backend the cursor to show over the surface under the
 * pointer, when it is not the one it was last told for that surface.
 */
void
x11_cursor_update(
	struct x11server *server)
{
	struct x11_cursor_image image;
	const struct x11_cursor *cursor;
	struct x11_window *window;
	struct x11_window *top;
	uint32_t *pixels;
	uint32_t version;
	uint32_t clear;
	int hidden;

	/* A backend that shows no X cursor, or a pointer outside the X windows. */
	if (server->ops == NULL || server->ops->cursor == NULL)
		return;
	if (server->pointer_outside)
		return;

	/* The top-level window under the pointer, and its surface. */
	window = x11_input_window_under(server);
	if (window == NULL || window == server->root)
		return;
	top = x11_window_top_level(window);
	if (top == NULL || top->surface == NULL)
		return;

	/* The cursor shown there (none while a client hid it, XFIXES), unless the backend was told of it already. */
	cursor = cursor_shown(server, window);
	version = 0U;
	if (cursor != NULL)
		version = cursor->version;
	hidden = x11_xfixes_cursor_hidden(server);
	if (hidden)
		version = CURSOR_HIDDEN_VERSION;
	if (server->cursor_known &&
	    server->cursor_window == top->id &&
	    server->cursor_surface == top->surface &&
	    server->cursor_shown_version == version)
		return;
	server->cursor_known = 1;
	server->cursor_window = top->id;
	server->cursor_surface = top->surface;
	server->cursor_shown_version = version;

	/* Hidden: a clear cursor of one pixel. */
	if (hidden) {
		clear = CURSOR_CLEAR;
		memset(&image, 0, sizeof(image));
		image.shape = -1;
		image.width = 1;
		image.height = 1;
		image.pixels = &clear;
		server->ops->cursor(server->backend, top->surface, &image);
		return;
	}

	/* None: the system's pointer. */
	if (cursor == NULL) {
		server->ops->cursor(server->backend, top->surface, NULL);
		return;
	}

	/* The cursor's shape, and its image when it has one. */
	memset(&image, 0, sizeof(image));
	image.shape = cursor->shape;
	image.width = cursor->width;
	image.height = cursor->height;
	image.hot_x = cursor->hot_x;
	image.hot_y = cursor->hot_y;
	pixels = NULL;
	if (cursor->source != NULL)
		pixels = cursor_image(cursor);
	image.pixels = pixels;
	if (pixels == NULL) {
		image.width = 0;
		image.height = 0;
	}

	/* Succeeded: the backend shows it. */
	server->ops->cursor(server->backend, top->surface, &image);
	free(pixels);
}

/*
 * Finds the cursor shown now: over the X window under the pointer, the
 * grab's or the window's (as x11_cursor_update shows it).  Returns it, or
 * NULL for the system's pointer (also outside the X windows).
 */
const struct x11_cursor *
x11_cursor_current(
	struct x11server *server)
{
	const struct x11_cursor *cursor;
	struct x11_window *window;

	/* Outside the X windows, or over the root window. */
	if (server->pointer_outside)
		return NULL;
	window = x11_input_window_under(server);
	if (window == NULL || window == server->root)
		return NULL;

	/* Succeeded: the cursor shown over it, if any. */
	cursor = cursor_shown(server, window);
	return cursor;
}

/*
 * Makes a cursor's image as ARGB pixels (malloc'd, width by height), or
 * NULL for a cursor of the cursor font (no image) or without memory.
 */
uint32_t *
x11_cursor_pixels(
	const struct x11_cursor *cursor)
{
	uint32_t *pixels;

	/* A shape of the system's has no image here. */
	if (cursor->source == NULL)
		return NULL;

	/* Succeeded: the image. */
	pixels = cursor_image(cursor);
	return pixels;
}

/*
 * Makes a cursor look like another (XFIXES's ChangeCursor): its shape,
 * bitmaps, colours and hot spot are copied, its charge follows them, and
 * it is a new version, so that the backend shows it again.  Returns 0, or
 * BadAlloc (the cursor is then as it was).
 */
unsigned
x11_cursor_change(
	struct x11server *server,
	struct x11_cursor *target,
	const struct x11_cursor *source)
{
	uint8_t *bits;
	uint8_t *mask;
	size_t bytes;
	size_t charged;
	int error;

	/* The same cursor: nothing to do. */
	if (target == source)
		return 0U;

	/* Copies of the bitmaps. */
	bytes = source->stride * (size_t)source->height;
	bits = NULL;
	mask = NULL;
	if (source->source != NULL) {
		bits = malloc(bytes);
		if (bits == NULL)
			return X11_BAD_ALLOC;
		mask = malloc(bytes);
		if (mask == NULL) {
			free(bits);
			return X11_BAD_ALLOC;
		}
		memcpy(bits, source->source, bytes);
		memcpy(mask, source->mask, bytes);
	}

	/* The new charge in place of the old. */
	charged = sizeof(*target) + bytes * 2U;
	error = x11_memory_charge(server, target->owner, charged);
	if (error != 0) {
		free(bits);
		free(mask);
		return X11_BAD_ALLOC;
	}
	x11_memory_release(server, target->owner, target->charged);
	target->charged = charged;

	/* The look. */
	free(target->source);
	free(target->mask);
	target->source = bits;
	target->mask = mask;
	target->shape = source->shape;
	target->width = source->width;
	target->height = source->height;
	target->hot_x = source->hot_x;
	target->hot_y = source->hot_y;
	target->stride = source->stride;
	memcpy(target->foreground, source->foreground, sizeof(target->foreground));
	memcpy(target->background, source->background, sizeof(target->background));

	/* Succeeded: a new version (never 0), which the backend is shown again. */
	server->cursor_serial++;
	if (server->cursor_serial == 0U)
		server->cursor_serial++;
	target->version = server->cursor_serial;
	return 0U;
}

/*
 * CreateCursor: a cursor of a bitmap of the pixels in the foreground, a
 * bitmap of those shown (None: all of them), the two colours and the hot
 * spot inside the bitmap.
 */
static unsigned
cursor_create(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_pixmap *source;
	struct x11_pixmap *mask;
	struct x11_cursor *cursor;
	uint32_t source_id;
	uint32_t mask_id;
	unsigned code;
	int hot_x;
	int hot_y;
	int x;
	int y;
	int msb;

	/* The id, the bitmaps, both of depth 1 and one size. */
	msb = server->clients[index].order;
	if (length != 32U)
		return X11_BAD_LENGTH;
	code = cursor_check_id(server, index, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	source_id = x11_read32(request + 8, msb);
	source = x11_pixmap_find(server, source_id);
	if (source == NULL)
		return X11_BAD_PIXMAP;
	mask_id = x11_read32(request + 12, msb);
	mask = NULL;
	if (mask_id != 0U) {
		mask = x11_pixmap_find(server, mask_id);
		if (mask == NULL)
			return X11_BAD_PIXMAP;
	}
	if (source->depth != 1U)
		return X11_BAD_MATCH;
	if (mask != NULL) {
		if (mask->depth != 1U || mask->width != source->width || mask->height != source->height)
			return X11_BAD_MATCH;
	}

	/* The hot spot, inside the bitmap or on its right or bottom edge (as X.Org takes it). */
	hot_x = x11_read16(request + 28, msb);
	hot_y = x11_read16(request + 30, msb);
	if (hot_x > source->width || hot_y > source->height)
		return X11_BAD_MATCH;

	/* The cursor, the largest a backend shows (a larger bitmap's top-left part). */
	x = source->width;
	y = source->height;
	if (x > CURSOR_SIZE_MAX)
		x = CURSOR_SIZE_MAX;
	if (y > CURSOR_SIZE_MAX)
		y = CURSOR_SIZE_MAX;
	cursor = cursor_new(x, y);
	if (cursor == NULL)
		return X11_BAD_ALLOC;
	cursor->hot_x = hot_x;
	cursor->hot_y = hot_y;

	/* Each pixel of the bitmaps. */
	for (y = 0; y < cursor->height; y++) {
		for (x = 0; x < cursor->width; x++) {
			if (source->image.pixels[(size_t)y * source->image.stride + (size_t)x] != 0U)
				cursor_set_bit(cursor->source, cursor->stride, x, y);
			if (mask == NULL || mask->image.pixels[(size_t)y * mask->image.stride + (size_t)x] != 0U)
				cursor_set_bit(cursor->mask, cursor->stride, x, y);
		}
	}

	/* The cursor in its colours, found by its id. */
	cursor_colors(server, cursor, request + 16, msb);
	code = cursor_add(server, index, x11_read32(request + 4, msb), cursor);
	if (code != 0U)
		return code;

	/* Succeeded: the cursor exists. */
	return 0U;
}

/*
 * CreateGlyphCursor: a cursor of a glyph in the foreground, shown where a
 * mask glyph (None: the glyph's whole box) has ink, its origins on the
 * hot spot.  A glyph of the cursor font is that standard cursor.
 */
static unsigned
cursor_create_glyph(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	const struct x11_font_glyph *glyph;
	struct x11_font_glyph source_glyph;
	struct x11_font_glyph mask_glyph;
	struct x11_font *source_font;
	struct x11_font *mask_font;
	struct x11_cursor *cursor;
	uint8_t *source_bits;
	uint32_t mask_id;
	unsigned source_char;
	unsigned mask_char;
	unsigned code;
	int present;
	int left;
	int top;
	int right;
	int bottom;
	int x;
	int y;
	int set;
	int msb;

	/* The id, the fonts and their characters. */
	msb = server->clients[index].order;
	if (length != 32U)
		return X11_BAD_LENGTH;
	code = cursor_check_id(server, index, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	source_font = x11_font_find(server, x11_read32(request + 8, msb));
	if (source_font == NULL)
		return X11_BAD_FONT;
	mask_id = x11_read32(request + 12, msb);
	mask_font = NULL;
	if (mask_id != 0U) {
		mask_font = x11_font_find(server, mask_id);
		if (mask_font == NULL)
			return X11_BAD_FONT;
	}
	source_char = x11_read16(request + 16, msb);
	mask_char = x11_read16(request + 18, msb);
	present = x11_font_has_char(source_font, source_char);
	if (!present)
		return X11_BAD_VALUE;
	if (mask_font != NULL) {
		present = x11_font_has_char(mask_font, mask_char);
		if (!present)
			return X11_BAD_VALUE;
	}

	/* A standard cursor: its shape, which the backend draws. */
	if (source_font->kind == X11_FONT_CURSOR) {
		cursor = cursor_new(0, 0);
		if (cursor == NULL)
			return X11_BAD_ALLOC;
		cursor->shape = (int)source_char;
		cursor_colors(server, cursor, request + 20, msb);
		code = cursor_add(server, index, x11_read32(request + 4, msb), cursor);
		if (code != 0U)
			return code;
		return 0U;
	}

	/* The source glyph's box and bits, kept (the mask's may take its place in the font). */
	memset(&source_glyph, 0, sizeof(source_glyph));
	memset(&mask_glyph, 0, sizeof(mask_glyph));
	source_bits = NULL;
	glyph = x11_font_glyph(server, source_font, source_char);
	if (glyph != NULL) {
		source_glyph = *glyph;
		source_bits = malloc(glyph->stride * (size_t)glyph->height);
		if (source_bits == NULL)
			return X11_BAD_ALLOC;
		memcpy(source_bits, glyph->bits, glyph->stride * (size_t)glyph->height);
		source_glyph.bits = source_bits;
	}

	/* The mask glyph's (the source's box without one). */
	mask_glyph = source_glyph;
	if (mask_font != NULL) {
		memset(&mask_glyph, 0, sizeof(mask_glyph));
		glyph = x11_font_glyph(server, mask_font, mask_char);
		if (glyph != NULL)
			mask_glyph = *glyph;
	}

	/* The box of both glyphs around their origin, the hot spot (a glyph without ink: one clear pixel). */
	left = -source_glyph.left;
	top = source_glyph.top;
	right = source_glyph.width - left;
	bottom = source_glyph.height - top;
	if (mask_glyph.bits != NULL) {
		if (-mask_glyph.left > left)
			left = -mask_glyph.left;
		if (mask_glyph.top > top)
			top = mask_glyph.top;
		if (mask_glyph.left + mask_glyph.width > right)
			right = mask_glyph.left + mask_glyph.width;
		if (mask_glyph.height - mask_glyph.top > bottom)
			bottom = mask_glyph.height - mask_glyph.top;
	}
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right < 1)
		right = 1;
	if (bottom < 0)
		bottom = 0;
	if (top + bottom == 0)
		bottom = 1;
	if (left + right > CURSOR_SIZE_MAX || top + bottom > CURSOR_SIZE_MAX) {
		free(source_bits);
		return X11_BAD_VALUE;
	}

	/* The cursor of that box. */
	cursor = cursor_new(left + right, top + bottom);
	if (cursor == NULL) {
		free(source_bits);
		return X11_BAD_ALLOC;
	}
	cursor->hot_x = left;
	cursor->hot_y = top;

	/* The glyphs' bits at their places: the source's ink, and the mask's (the source's whole box without a mask font). */
	for (y = 0; y < source_glyph.height && source_bits != NULL; y++) {
		for (x = 0; x < source_glyph.width; x++) {
			set = cursor_bit(source_bits, source_glyph.stride, x, y);
			if (set)
				cursor_set_bit(cursor->source, cursor->stride, left + source_glyph.left + x, top - source_glyph.top + y);
			if (mask_font == NULL)
				cursor_set_bit(cursor->mask, cursor->stride, left + source_glyph.left + x, top - source_glyph.top + y);
		}
	}
	for (y = 0; y < mask_glyph.height && mask_font != NULL && mask_glyph.bits != NULL; y++) {
		for (x = 0; x < mask_glyph.width; x++) {
			set = cursor_bit(mask_glyph.bits, mask_glyph.stride, x, y);
			if (set)
				cursor_set_bit(cursor->mask, cursor->stride, left + mask_glyph.left + x, top - mask_glyph.top + y);
		}
	}
	free(source_bits);

	/* The cursor in its colours, found by its id. */
	cursor_colors(server, cursor, request + 20, msb);
	code = cursor_add(server, index, x11_read32(request + 4, msb), cursor);
	if (code != 0U)
		return code;

	/* Succeeded: the cursor exists. */
	return 0U;
}

/* FreeCursor: the id goes (the cursor lives on while a window or a grab names it). */
static unsigned
cursor_free_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_cursor *cursor;

	/* The cursor of the id. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	cursor = x11_cursor_find(server, x11_read32(request + 4, server->clients[index].order));
	if (cursor == NULL)
		return X11_BAD_CURSOR;

	/* Succeeded: the id is gone. */
	x11_resource_remove(server, cursor->id);
	x11_cursor_free(server, cursor);
	return 0U;
}

/* RecolorCursor: a cursor's two colours, shown at once where it is. */
static unsigned
cursor_recolor(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_cursor *cursor;
	int msb;

	/* The cursor of the id. */
	msb = server->clients[index].order;
	if (length != 20U)
		return X11_BAD_LENGTH;
	cursor = x11_cursor_find(server, x11_read32(request + 4, msb));
	if (cursor == NULL)
		return X11_BAD_CURSOR;

	/* Succeeded: its new colours. */
	cursor_colors(server, cursor, request + 8, msb);
	return 0U;
}

/* Checks a new cursor's id: one of the client's range not in use (0, or BadIDChoice). */
static unsigned
cursor_check_id(
	struct x11server *server,
	unsigned index,
	uint32_t id)
{
	unsigned taken;
	int chosen;

	/* The client's own range, and free. */
	chosen = x11_id_chosen(&server->clients[index], id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* Succeeded: the id may be used. */
	return 0U;
}

/*
 * Charges a new cursor to its client and finds it by its id, the id's
 * hold its first.  Returns 0, or BadAlloc (the cursor is then freed).
 */
static unsigned
cursor_add(
	struct x11server *server,
	unsigned index,
	uint32_t id,
	struct x11_cursor *cursor)
{
	int error;

	/* The client holds the cursor and its bitmaps. */
	cursor->charged = sizeof(*cursor) + cursor->stride * (size_t)cursor->height * 2U;
	error = x11_memory_charge(server, index, cursor->charged);
	if (error != 0) {
		cursor->charged = 0U;
		cursor->owner = X11_NO_CLIENT;
		cursor->holders = 1U;
		x11_cursor_release(server, cursor);
		return X11_BAD_ALLOC;
	}
	cursor->owner = index;
	cursor->id = id;
	cursor->holders = 1U;

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_CURSOR, cursor);
	if (error != 0) {
		x11_cursor_release(server, cursor);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the cursor exists. */
	return 0U;
}

/* Makes a cursor of a size with clear bitmaps (none for a size of 0), of no shape.  Returns it, or NULL. */
static struct x11_cursor *
cursor_new(
	int width,
	int height)
{
	struct x11_cursor *cursor;

	/* The cursor. */
	cursor = calloc(1U, sizeof(*cursor));
	if (cursor == NULL)
		return NULL;
	cursor->shape = -1;
	cursor->width = width;
	cursor->height = height;
	cursor->stride = ((size_t)width + 7U) / 8U;
	if (width == 0 || height == 0)
		return cursor;

	/* The bitmap of the pixels in the foreground. */
	cursor->source = calloc(cursor->stride, (size_t)height);
	if (cursor->source == NULL) {
		free(cursor);
		return NULL;
	}

	/* The bitmap of the pixels shown. */
	cursor->mask = calloc(cursor->stride, (size_t)height);
	if (cursor->mask == NULL) {
		free(cursor->source);
		free(cursor);
		return NULL;
	}

	/* Succeeded: the cursor. */
	return cursor;
}

/*
 * Reads a cursor's two colours (red, green and blue of the foreground,
 * then the background, 16 bits each), as a new version of it.
 */
static void
cursor_colors(
	struct x11server *server,
	struct x11_cursor *cursor,
	const uint8_t *bytes,
	int msb)
{
	unsigned channel;

	/* The foreground's channels, then the background's. */
	for (channel = 0U; channel < 3U; channel++) {
		cursor->foreground[channel] = x11_read16(bytes + channel * 2U, msb);
		cursor->background[channel] = x11_read16(bytes + 6U + channel * 2U, msb);
	}

	/*
	 * A version the server has given no other cursor, so that the backend
	 * is told again of a cursor changed, or of another made where a freed
	 * one was.  Never 0, which means the system's pointer.
	 */
	server->cursor_serial++;
	if (server->cursor_serial == 0U)
		server->cursor_serial++;
	cursor->version = server->cursor_serial;
}

/*
 * Finds the cursor shown over a window: the active pointer grab's, else
 * the window's or its nearest ancestor's that names one.  Returns it, or
 * NULL for the system's pointer.
 */
static const struct x11_cursor *
cursor_shown(
	struct x11server *server,
	struct x11_window *window)
{
	const struct x11_cursor *cursor;
	struct x11_window *ancestor;

	/* The grab's cursor while one is active. */
	if (server->pointer_grab.client != X11_NO_CLIENT && server->pointer_grab.cursor != 0U) {
		cursor = x11_cursor_find(server, server->pointer_grab.cursor);
		if (cursor != NULL)
			return cursor;
	}

	/* The window's, or the nearest ancestor's. */
	for (ancestor = window; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor->cursor_held != NULL)
			return ancestor->cursor_held;
	}

	/* None: the system's pointer. */
	return NULL;
}

/*
 * Makes a cursor's image: each pixel the mask shows in the foreground or
 * the background, the others clear.  Returns it (malloc'd), or NULL.
 */
static uint32_t *
cursor_image(
	const struct x11_cursor *cursor)
{
	uint32_t foreground;
	uint32_t background;
	uint32_t *pixels;
	int shown;
	int set;
	int x;
	int y;

	/* The colours, eight bits a channel. */
	foreground = CURSOR_OPAQUE | ((uint32_t)(cursor->foreground[0] >> 8) << 16) | ((uint32_t)(cursor->foreground[1] >> 8) << 8) | (uint32_t)(cursor->foreground[2] >> 8);
	background = CURSOR_OPAQUE | ((uint32_t)(cursor->background[0] >> 8) << 16) | ((uint32_t)(cursor->background[1] >> 8) << 8) | (uint32_t)(cursor->background[2] >> 8);

	/* The pixels. */
	pixels = malloc((size_t)cursor->width * (size_t)cursor->height * sizeof(*pixels));
	if (pixels == NULL)
		return NULL;

	/* Each one shown or clear. */
	for (y = 0; y < cursor->height; y++) {
		for (x = 0; x < cursor->width; x++) {
			shown = cursor_bit(cursor->mask, cursor->stride, x, y);
			set = cursor_bit(cursor->source, cursor->stride, x, y);
			if (!shown) {
				pixels[(size_t)y * (size_t)cursor->width + (size_t)x] = CURSOR_CLEAR;
			} else if (set) {
				pixels[(size_t)y * (size_t)cursor->width + (size_t)x] = foreground;
			} else {
				pixels[(size_t)y * (size_t)cursor->width + (size_t)x] = background;
			}
		}
	}

	/* Succeeded: the image. */
	return pixels;
}

/* Sets a bit of a bitmap (most significant first), when it is inside the bitmap's rows of stride bytes. */
static void
cursor_set_bit(
	uint8_t *bits,
	size_t stride,
	int x,
	int y)
{
	/* A bit outside is dropped. */
	if (x < 0 || y < 0 || (size_t)x >= stride * 8U)
		return;

	/* The bit. */
	bits[(size_t)y * stride + (size_t)x / 8U] |= (uint8_t)(0x80U >> (x % 8));
}

/* Tells whether a bit of a bitmap (most significant first) is set (1, or 0). */
static int
cursor_bit(
	const uint8_t *bits,
	size_t stride,
	int x,
	int y)
{
	/* The bit's byte. */
	if ((bits[(size_t)y * stride + (size_t)x / 8U] & (0x80U >> (x % 8))) == 0U)
		return 0;

	/* Succeeded: set. */
	return 1;
}
