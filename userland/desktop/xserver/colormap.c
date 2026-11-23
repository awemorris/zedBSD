/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Colormaps and colours: the requests from CreateColormap to LookupColor.
 *
 * The screen has one visual, TrueColor of eight bits each of red, green
 * and blue, so every colormap gives each colour the pixel of its own
 * bits and holds no cells: AllocColor keeps the top eight bits of each
 * value, a colour is read back from a pixel's bits, cells that could be
 * written cannot be had (BadAlloc) or written (BadAccess), and the
 * screen's colormap is always the one installed.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The requests' opcodes. */
#define REQUEST_CREATE_COLORMAP		78U
#define REQUEST_FREE_COLORMAP		79U
#define REQUEST_COPY_COLORMAP_AND_FREE	80U
#define REQUEST_INSTALL_COLORMAP	81U
#define REQUEST_UNINSTALL_COLORMAP	82U
#define REQUEST_LIST_INSTALLED_COLORMAPS 83U
#define REQUEST_ALLOC_COLOR		84U
#define REQUEST_ALLOC_NAMED_COLOR	85U
#define REQUEST_ALLOC_COLOR_CELLS	86U
#define REQUEST_ALLOC_COLOR_PLANES	87U
#define REQUEST_FREE_COLORS		88U
#define REQUEST_STORE_COLORS		89U
#define REQUEST_STORE_NAMED_COLOR	90U
#define REQUEST_QUERY_COLORS		91U

/* CreateColormap's allocation of all cells, and the ColormapNotify event and its uninstalled state. */
#define COLORMAP_ALLOC_ALL		1U
#define EVENT_COLORMAP_NOTIFY		32U

/* The colormap changes a window's clients may select. */
#define MASK_COLORMAP_CHANGE		(1U << 23)

/* The planes of a pixel of the screen's depth. */
#define COLOR_PIXEL_MASK		0x00ffffffU

static unsigned colormap_create(struct x11server *server, unsigned index, uint32_t id, uint32_t visual);
static unsigned colormap_check(struct x11server *server, uint32_t id);
static unsigned colormap_free(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned colormap_copy_and_free(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned colormap_installed(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned colormap_alloc_color(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned colormap_named_color(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned colormap_query_colors(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static void colormap_forget(struct x11server *server, uint32_t id);
static uint32_t colormap_pixel(uint16_t red, uint16_t green, uint16_t blue);
static uint16_t colormap_value(uint32_t pixel, unsigned shift);

/*
 * Answers one of the colormap requests (CreateColormap to LookupColor).
 * Returns 0, or the error to send.
 */
unsigned
x11_colormap_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	uint32_t visual;
	size_t fixed;
	unsigned code;
	int msb;

	/* The handler of the opcode. */
	msb = server->clients[index].order;
	switch (request[0]) {
	case REQUEST_CREATE_COLORMAP:
		/* An allocation of none (all is no use with a fixed visual), a window, and one of the screen's visuals. */
		if (length != 16U)
			return X11_BAD_LENGTH;
		if (request[1] > COLORMAP_ALLOC_ALL)
			return X11_BAD_VALUE;
		window = x11_window_find(server, x11_read32(request + 8, msb));
		if (window == NULL)
			return X11_BAD_WINDOW;
		visual = x11_read32(request + 12, msb);
		if (visual != X11_VISUAL_XID && visual != X11_VISUAL_ARGB_XID)
			return X11_BAD_MATCH;
		if (request[1] == COLORMAP_ALLOC_ALL)
			return X11_BAD_MATCH;
		code = colormap_create(server, index, x11_read32(request + 4, msb), visual);
		break;
	case REQUEST_FREE_COLORMAP:
		code = colormap_free(server, index, request, length);
		break;
	case REQUEST_COPY_COLORMAP_AND_FREE:
		code = colormap_copy_and_free(server, index, request, length);
		break;
	case REQUEST_INSTALL_COLORMAP:
	case REQUEST_UNINSTALL_COLORMAP:
		/* The screen's colormap stays installed: nothing changes. */
		if (length != 8U)
			return X11_BAD_LENGTH;
		code = colormap_check(server, x11_read32(request + 4, msb));
		break;
	case REQUEST_LIST_INSTALLED_COLORMAPS:
		code = colormap_installed(server, index, request, length);
		break;
	case REQUEST_ALLOC_COLOR:
		code = colormap_alloc_color(server, index, request, length);
		break;
	case REQUEST_ALLOC_NAMED_COLOR:
		code = colormap_named_color(server, index, request, length);
		break;
	case REQUEST_ALLOC_COLOR_CELLS:
	case REQUEST_ALLOC_COLOR_PLANES:
		/* No cell of a fixed visual can be written (AllocColorPlanes asks for three masks more). */
		fixed = 12U;
		if (request[0] == REQUEST_ALLOC_COLOR_PLANES)
			fixed = 16U;
		if (length != fixed)
			return X11_BAD_LENGTH;
		code = colormap_check(server, x11_read32(request + 4, msb));
		if (code == 0U)
			code = X11_BAD_ALLOC;
		break;
	case REQUEST_FREE_COLORS:
		/* No cell was given, so none is freed. */
		if (length < 12U || length % 4U != 0U)
			return X11_BAD_LENGTH;
		code = colormap_check(server, x11_read32(request + 4, msb));
		break;
	case REQUEST_STORE_COLORS:
	case REQUEST_STORE_NAMED_COLOR:
		/* No cell of a fixed visual can be written. */
		if (length < 8U || length % 4U != 0U)
			return X11_BAD_LENGTH;
		code = colormap_check(server, x11_read32(request + 4, msb));
		if (code == 0U)
			code = X11_BAD_ACCESS;
		break;
	case REQUEST_QUERY_COLORS:
		code = colormap_query_colors(server, index, request, length);
		break;
	default:
		/* LookupColor. */
		code = colormap_named_color(server, index, request, length);
		break;
	}

	/* Answered, or the error to send. */
	return code;
}

/*
 * Frees a colormap that no id names, and what its owner is charged for
 * it.
 */
void
x11_colormap_free(
	struct x11server *server,
	struct x11_colormap *colormap)
{
	/* The windows that used it use none. */
	colormap_forget(server, colormap->id);

	/* The colormap, with its charge. */
	x11_memory_release(server, colormap->owner, sizeof(*colormap));
	free(colormap);
}

/*
 * Gives the visual of a colormap: the screen's colormap has the screen's
 * visual.  Returns the visual's id, or 0 when the id names no colormap.
 */
uint32_t
x11_colormap_visual(
	struct x11server *server,
	uint32_t id)
{
	struct x11_colormap *colormap;

	/* The screen's. */
	if (id == X11_COLORMAP_XID)
		return X11_VISUAL_XID;

	/* One a client made, or none. */
	colormap = x11_resource_find(server, id, X11_RESOURCE_COLORMAP);
	if (colormap == NULL)
		return 0U;

	/* Succeeded: its visual. */
	return colormap->visual;
}

/* Makes a colormap of an id the client chose, of a visual.  Returns 0, or the error. */
static unsigned
colormap_create(
	struct x11server *server,
	unsigned index,
	uint32_t id,
	uint32_t visual)
{
	struct x11_colormap *colormap;
	unsigned taken;
	int chosen;
	int error;

	/* An id of the client's range not in use. */
	chosen = x11_id_chosen(&server->clients[index], id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The colormap, charged to the client. */
	error = x11_memory_charge(server, index, sizeof(*colormap));
	if (error != 0)
		return X11_BAD_ALLOC;
	colormap = calloc(1U, sizeof(*colormap));
	if (colormap == NULL) {
		x11_memory_release(server, index, sizeof(*colormap));
		return X11_BAD_ALLOC;
	}
	colormap->id = id;
	colormap->owner = index;
	colormap->visual = visual;

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_COLORMAP, colormap);
	if (error != 0) {
		x11_memory_release(server, index, sizeof(*colormap));
		free(colormap);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/* Tells whether an id names a colormap (the screen's, or one a client made): 0, or BadColormap. */
static unsigned
colormap_check(
	struct x11server *server,
	uint32_t id)
{
	unsigned kind;

	/* The screen's. */
	if (id == X11_COLORMAP_XID)
		return 0U;

	/* One a client made. */
	kind = x11_resource_type(server, id);
	if (kind != X11_RESOURCE_COLORMAP)
		return X11_BAD_COLORMAP;

	/* Succeeded: a colormap. */
	return 0U;
}

/* FreeColormap: a client's colormap goes (the screen's stays). */
static unsigned
colormap_free(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_colormap *colormap;
	uint32_t id;
	unsigned code;

	/* A colormap. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, server->clients[index].order);
	code = colormap_check(server, id);
	if (code != 0U)
		return code;

	/* The screen's is not freed. */
	if (id == X11_COLORMAP_XID)
		return 0U;

	/* Succeeded: its id goes, and it. */
	colormap = x11_resource_find(server, id, X11_RESOURCE_COLORMAP);
	x11_resource_remove(server, id);
	x11_colormap_free(server, colormap);
	return 0U;
}

/*
 * CopyColormapAndFree: a new colormap with the colours of another (all
 * of them: a fixed visual's colormaps are all alike).
 */
static unsigned
colormap_copy_and_free(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t visual;
	unsigned code;
	int msb;

	/* The colormap copied. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	code = colormap_check(server, x11_read32(request + 8, msb));
	if (code != 0U)
		return code;

	/* The new one, of the same visual. */
	visual = x11_colormap_visual(server, x11_read32(request + 8, msb));
	code = colormap_create(server, index, x11_read32(request + 4, msb), visual);
	if (code != 0U)
		return code;

	/* Succeeded: made. */
	return 0U;
}

/* ListInstalledColormaps: the screen's colormap, always installed. */
static unsigned
colormap_installed(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_window *window;
	uint8_t reply[36];

	/* A window. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Succeeded: one colormap. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, 1U, client->order);
	x11_write32(reply + 32, X11_COLORMAP_XID, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * AllocColor: the pixel of the colour nearest one asked for (its top
 * eight bits of each value), and the colour it is.
 */
static unsigned
colormap_alloc_color(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	uint32_t pixel;
	unsigned code;
	int msb;

	/* A colormap. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	code = colormap_check(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;

	/* The pixel, and the colour read back from it. */
	pixel = colormap_pixel(x11_read16(request + 8, msb), x11_read16(request + 10, msb), x11_read16(request + 12, msb));
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, colormap_value(pixel, 16U), msb);
	x11_write16(reply + 10, colormap_value(pixel, 8U), msb);
	x11_write16(reply + 12, colormap_value(pixel, 0U), msb);
	x11_write32(reply + 16, pixel, msb);

	/* Succeeded: answered. */
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * AllocNamedColor and LookupColor: a colour by its name, as the name
 * gives it (exact) and as the screen shows it (visual), and for
 * AllocNamedColor its pixel.  BadName for a name the table does not know.
 */
static unsigned
colormap_named_color(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	uint32_t pixel;
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	size_t name_length;
	size_t at;
	unsigned code;
	int found;
	int msb;

	/* A colormap and a name filling the request to its last word. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	name_length = x11_read16(request + 8, msb);
	if (length != 12U + ((name_length + 3U) & ~(size_t)3U))
		return X11_BAD_LENGTH;
	code = colormap_check(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;

	/* The colour of the name. */
	found = x11_color_lookup((const char *)(request + 12), name_length, &red, &green, &blue);
	if (!found)
		return X11_BAD_NAME;
	pixel = colormap_pixel(red, green, blue);

	/* The exact and the visual colour, after the pixel for AllocNamedColor. */
	memset(reply, 0, sizeof(reply));
	at = 8U;
	if (request[0] == REQUEST_ALLOC_NAMED_COLOR) {
		x11_write32(reply + 8, pixel, msb);
		at = 12U;
	}
	x11_write16(reply + at, red, msb);
	x11_write16(reply + at + 2U, green, msb);
	x11_write16(reply + at + 4U, blue, msb);
	x11_write16(reply + at + 6U, colormap_value(pixel, 16U), msb);
	x11_write16(reply + at + 8U, colormap_value(pixel, 8U), msb);
	x11_write16(reply + at + 10U, colormap_value(pixel, 0U), msb);

	/* Succeeded: answered. */
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * QueryColors: the colour of each pixel given (BadValue for one that is
 * not a pixel of the screen's depth).
 */
static unsigned
colormap_query_colors(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t *reply;
	uint32_t pixel;
	uint32_t planes;
	uint32_t visual;
	size_t count;
	size_t at;
	unsigned code;
	int msb;

	/* A colormap and whole pixels, each of its visual's depth (all 32 bits for the ARGB visual). */
	client = &server->clients[index];
	msb = client->order;
	if (length < 8U || length % 4U != 0U)
		return X11_BAD_LENGTH;
	code = colormap_check(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	visual = x11_colormap_visual(server, x11_read32(request + 4, msb));
	planes = COLOR_PIXEL_MASK;
	if (visual == X11_VISUAL_ARGB_XID)
		planes = 0xffffffffU;
	count = (length - 8U) / 4U;
	for (at = 0U; at < count; at++) {
		pixel = x11_read32(request + 8U + at * 4U, msb);
		if ((pixel & ~planes) != 0U)
			return X11_BAD_VALUE;
	}

	/* Room for the answer. */
	reply = calloc(1U, 32U + count * 8U);
	if (reply == NULL)
		return X11_BAD_ALLOC;

	/* Each pixel's colour read back from its bits. */
	x11_write16(reply + 8, (uint16_t)count, msb);
	for (at = 0U; at < count; at++) {
		pixel = x11_read32(request + 8U + at * 4U, msb);
		x11_write16(reply + 32U + at * 8U, colormap_value(pixel, 16U), msb);
		x11_write16(reply + 32U + at * 8U + 2U, colormap_value(pixel, 8U), msb);
		x11_write16(reply + 32U + at * 8U + 4U, colormap_value(pixel, 0U), msb);
	}

	/* Succeeded: answered. */
	x11_reply(server, client, reply, 32U + count * 8U);
	free(reply);
	return 0U;
}

/*
 * Has every window that used a colormap going away use none, telling
 * each window's ColormapChange clients (ColormapNotify: the colormap is
 * None, changed, uninstalled).
 */
static void
colormap_forget(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;
	uint8_t event[32];

	/* Each window of the tree. */
	for (window = server->root;
	     window != NULL;
	     window = x11_window_next(window, server->root)) {
		if (window->colormap != id)
			continue;
		window->colormap = 0U;
		memset(event, 0, sizeof(event));
		event[0] = EVENT_COLORMAP_NOTIFY;
		x11_write32(event + 4, window->id, 0);
		event[12] = 1U;
		(void)x11_event_deliver(server, window, MASK_COLORMAP_CHANGE, event);
	}
}

/* Returns the pixel of a colour: the top eight bits of each value in its place. */
static uint32_t
colormap_pixel(
	uint16_t red,
	uint16_t green,
	uint16_t blue)
{
	uint32_t pixel;

	/* Red, green and blue from the high byte down. */
	pixel = ((uint32_t)(red >> 8) << 16) | ((uint32_t)(green >> 8) << 8) | (uint32_t)(blue >> 8);

	/* The pixel. */
	return pixel;
}

/* Returns the sixteen-bit value of one colour of a pixel (the eight bits at a shift, spread). */
static uint16_t
colormap_value(
	uint32_t pixel,
	unsigned shift)
{
	/* The eight bits, repeated in the low byte. */
	return (uint16_t)(((pixel >> shift) & 0xffU) * 257U);
}
