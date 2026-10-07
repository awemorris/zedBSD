/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Cursor shapes (wp_cursor_shape_manager_v1, ws035-p080): a client names
 * the cursor it wants (text, a pointing hand, a resize arrow, ...) instead
 * of drawing one on a surface of its own.
 *
 * The compositor draws the shapes itself: each image is a white figure with a
 * black edge, the figure made from a few rectangles and triangles and the
 * edge found around it, so no picture is kept in the source.  Shapes with
 * no image of their own are drawn as the compositor's arrow.  A shape is taken
 * only from the client the pointer is on; the pointer going to another
 * client, or a cursor surface, brings the arrow back.
 */

#include "extras.h"
#include "popup.h"
#include "toplevel.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The requests of the manager and of a pointer's cursor-shape device. */
#define MANAGER_DESTROY			0U
#define MANAGER_GET_POINTER		1U
#define DEVICE_DESTROY			0U
#define DEVICE_SET_SHAPE		1U

/* The device's error: a shape that is not one. */
#define DEVICE_ERROR_INVALID_SHAPE	1U

/* The shapes of wp_cursor_shape_device_v1 (1 to 34). */
#define SHAPE_DEFAULT			1U
#define SHAPE_POINTER			4U
#define SHAPE_PROGRESS			5U
#define SHAPE_WAIT			6U
#define SHAPE_CELL			7U
#define SHAPE_CROSSHAIR			8U
#define SHAPE_TEXT			9U
#define SHAPE_VERTICAL_TEXT		10U
#define SHAPE_MOVE			13U
#define SHAPE_NO_DROP			14U
#define SHAPE_NOT_ALLOWED		15U
#define SHAPE_GRAB			16U
#define SHAPE_GRABBING			17U
#define SHAPE_E_RESIZE			18U
#define SHAPE_N_RESIZE			19U
#define SHAPE_NE_RESIZE			20U
#define SHAPE_NW_RESIZE			21U
#define SHAPE_S_RESIZE			22U
#define SHAPE_SE_RESIZE			23U
#define SHAPE_SW_RESIZE			24U
#define SHAPE_W_RESIZE			25U
#define SHAPE_EW_RESIZE			26U
#define SHAPE_NS_RESIZE			27U
#define SHAPE_NESW_RESIZE		28U
#define SHAPE_NWSE_RESIZE		29U
#define SHAPE_COL_RESIZE		30U
#define SHAPE_ROW_RESIZE		31U
#define SHAPE_ALL_SCROLL		32U
#define SHAPE_LAST			34U

/* The images the compositor draws, by index into server->cursor_images. */
#define IMAGE_TEXT			0U
#define IMAGE_HAND			1U
#define IMAGE_CROSSHAIR			2U
#define IMAGE_EW			3U
#define IMAGE_NS			4U
#define IMAGE_NWSE			5U
#define IMAGE_NESW			6U
#define IMAGE_MOVE			7U
#define IMAGE_NOT_ALLOWED		8U
#define IMAGE_WAIT			9U
#define IMAGE_NONE			KWL_CURSOR_IMAGES

/* The size of every image, and its middle (the hotspot of most). */
#define CURSOR_SIZE			24
#define CURSOR_MIDDLE			12

/*
 * One image's figure, as a mask of the pixels drawn white (the black edge
 * is found around them).
 */
struct cursor_mask {
	unsigned char fill[CURSOR_SIZE][CURSOR_SIZE];
};

/* The hotspot of each image (x, y), by index: the middle, the hand's fingertip. */
static const int32_t cursor_hotspots[KWL_CURSOR_IMAGES][2] = {
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ 9, 1 },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE },
	{ CURSOR_MIDDLE, CURSOR_MIDDLE }
};

static int device_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int device_set_shape(struct kwl_object *device, const unsigned char *bytes, size_t size);
static unsigned shape_image(uint32_t shape);
static uint32_t frame_shape(uint32_t edges);
static void mask_figure(unsigned index, struct cursor_mask *mask);
static void mask_rectangle(struct cursor_mask *mask, int x0, int y0, int x1, int y1);
static void mask_arrow_heads(struct cursor_mask *mask, unsigned horizontal);
static void mask_diagonal(struct cursor_mask *mask, unsigned falling);
static void mask_ring(struct cursor_mask *mask);
static void mask_hourglass(struct cursor_mask *mask);
static int image_make(struct kwl_server *server, unsigned index);
static uint32_t cursor_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of wp_cursor_shape_manager_v1 or of a pointer's
 * wp_cursor_shape_device_v1.
 */
int
kwl_cursor_shape_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The manager: it goes, or it makes a pointer's device (tablet tools are not offered). */
	if (object->kind == KWL_CURSOR_SHAPE_MANAGER) {
		if (opcode == MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_pointer is left (tablet tools are not offered). */
		if (opcode != MANAGER_GET_POINTER)
			return EPROTO;
		error = device_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The device goes. */
	if (opcode == DEVICE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* Only set_shape is left. */
	if (opcode != DEVICE_SET_SHAPE)
		return EPROTO;
	error = device_set_shape(object, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the shape is the cursor's while the pointer is on the client. */
	return 0;
}

/*
 * Unties a wl_pointer that is going from the cursor-shape devices made for
 * it.
 */
void
kwl_cursor_shape_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *other;

	/* Only a pointer is named by devices. */
	if (object->kind != KWL_POINTER)
		return;

	/* Its client's devices of it. */
	for (other = object->client->objects; other != NULL; other = other->next) {
		/* A device of this pointer names nothing now. */
		if (other->kind == KWL_CURSOR_SHAPE_DEVICE && other->shape_pointer == object)
			other->shape_pointer = NULL;
	}
}

/*
 * Releases the images of the cursor shapes.
 */
void
kwl_cursor_images_destroy(
	struct kwl_server *server)
{
	unsigned index;

	/* Each image made. */
	for (index = 0; index < KWL_CURSOR_IMAGES; index++) {
		/* An empty slot. */
		if (server->cursor_images[index] == NULL)
			continue;

		/* Its Vulkan objects, then the record. */
		kwl_host_image_release(server->compose, server->cursor_images[index]);
		free(server->cursor_images[index]);
		server->cursor_images[index] = NULL;
	}
}

/*
 * Finds the image of the shape the cursor has now, and its hotspot; NULL
 * when the cursor is the compositor's arrow.
 */
const struct kwl_import *
kwl_cursor_image(
	const struct kwl_server *server,
	int32_t *hotspot_x,
	int32_t *hotspot_y)
{
	uint32_t shape;
	unsigned index;

	/* A window frame's resize arrow comes first, then the client's shape; the arrow by default. */
	*hotspot_x = 0;
	*hotspot_y = 0;
	shape = server->cursor_shape;
	if (server->frame_edges != 0U)
		shape = frame_shape(server->frame_edges);
	index = shape_image(shape);
	if (index == IMAGE_NONE || server->cursor_images[index] == NULL)
		return NULL;

	/* Succeeded: the shape's image and hotspot. */
	*hotspot_x = cursor_hotspots[index][0];
	*hotspot_y = cursor_hotspots[index][1];
	return server->cursor_images[index];
}

/*
 * Shows the resize arrow of a window frame's edges under the pointer (the
 * glass look's frames, shell.c), or takes it away with no edges.
 *
 * The frame's arrow is drawn over whatever the client under the pointer
 * asked for, its shape or its cursor surface, until the edges are 0 again.
 */
void
kwl_cursor_frame(
	struct kwl_server *server,
	uint32_t edges)
{
	unsigned index;
	int error;

	/* An unchanged frame keeps its cursor. */
	if (server->frame_edges == edges)
		return;

	/* The arrow's image is made the first time it is needed, like a client's shape's. */
	if (edges != 0U) {
		index = shape_image(frame_shape(edges));
		if (index != IMAGE_NONE &&
		    server->cursor_images[index] == NULL &&
		    server->compose != NULL) {
			error = image_make(server, index);
			if (error != 0)
				printf("KWL CURSOR image=%u errno=%d\n", index, error);
		}
	}

	/* The next frame draws the new cursor. */
	server->frame_edges = edges;
	server->dirty = 1;

	/* Succeeded: the log line the tests read. */
	printf("KWL CURSOR frame edges=%u\n", edges);
}

/*
 * Tells whether the cursor a client chose (hidden, its surface or its shape)
 * is shown now: only while the pointer is over that client's window body,
 * or while its popup holds the pointer; elsewhere (the desktop, the system
 * bar, a title bar, another client's window) the compositor's arrow is shown
 * (BUG-118: an X terminal that hid the cursor hid it on the whole screen).
 * The compositor's own cursor, and the plain look, are always shown.
 */
int
kwl_cursor_client_shown(
	struct kwl_server *server)
{
	struct kwl_object *window;
	unsigned shown;

	/* The compositor's own cursor, or the plain look (one window at its place): as it is. */
	if (server->cursor_client == NULL || !server->glass)
		return 1;

	/* A popup's grab keeps the pointer with its client. */
	shown = 1U;
	if (!server->pointer_grabbed) {
		/* The window whose body is under the pointer must be the client's. */
		window = kwl_glass_body_at(server, server->pointer_x, server->pointer_y);
		if (window == NULL || window->client != server->cursor_client)
			shown = 0U;
	}

	/* The log says when it changes (for the tests). */
	if (server->cursor_client_logged != shown + 1U) {
		server->cursor_client_logged = shown + 1U;
		printf("KWL CURSOR client=%llu shown=%u\n", (unsigned long long)server->cursor_client->number, shown);
	}

	/* Succeeded: whether the client's cursor is shown. */
	if (shown == 0U)
		return 0;
	return 1;
}

/* Makes a pointer's cursor-shape device. */
static int
device_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *pointer;
	struct kwl_object *created;
	uint32_t id;
	uint32_t pointer_id;

	/* The new ID and the client's pointer. */
	if (size != 8U)
		return EPROTO;
	id = cursor_word(bytes, 0U);
	pointer_id = cursor_word(bytes, 4U);
	pointer = kwl_find(manager->client, pointer_id);
	if (pointer == NULL || pointer->kind != KWL_POINTER)
		return EPROTO;

	/* Succeeded: the device, naming its pointer. */
	created = kwl_create(manager->client, id, KWL_CURSOR_SHAPE_DEVICE, manager->version);
	if (created == NULL)
		return EPROTO;
	created->shape_pointer = pointer;
	return 0;
}

/* Takes a shape a client asks for (with the serial of the pointer's enter). */
static int
device_set_shape(
	struct kwl_object *device,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	uint32_t shape;
	unsigned index;
	int error;

	/* The serial and the shape, which must be one. */
	if (size != 8U)
		return EPROTO;
	shape = cursor_word(bytes, 4U);
	if (shape == 0U || shape > SHAPE_LAST) {
		(void)kwl_error_code(device->client, device->id, DEVICE_ERROR_INVALID_SHAPE, "not a cursor shape");
		return EPROTO;
	}

	/* Only the client the pointer is on sets the cursor (its pointer must still exist). */
	server = device->client->server;
	if (device->shape_pointer == NULL ||
	    server->pointer_surface == NULL ||
	    server->pointer_surface->client != device->client)
		return 0;

	/*
	 * The shape's image is made the first time it is asked for (making all
	 * of them at start-up would delay the compositor's READY); without it the
	 * shape is drawn as the arrow.
	 */
	index = shape_image(shape);
	if (index != IMAGE_NONE &&
	    server->cursor_images[index] == NULL &&
	    server->compose != NULL) {
		error = image_make(server, index);
		if (error != 0)
			printf("KWL CURSOR image=%u errno=%d\n", index, error);
	}

	/* The shape replaces a cursor surface, and is drawn from the next frame. */
	if (server->cursor_surface != NULL)
		server->cursor_surface->cursor_role = 0;
	server->cursor_surface = NULL;
	server->cursor_hidden = 0;
	server->cursor_shape = shape;
	server->cursor_client = device->client;
	server->dirty = 1;

	/* Succeeded: the log line the tests read. */
	printf("KWL CURSOR shape client=%llu shape=%u image=%u\n", (unsigned long long)device->client->number, shape, shape_image(shape));
	return 0;
}

/* Tells which image a shape is drawn with (IMAGE_NONE for the arrow). */
static unsigned
shape_image(
	uint32_t shape)
{
	/* The shapes by the image they share. */
	switch (shape) {
	case SHAPE_TEXT:
	case SHAPE_VERTICAL_TEXT:
		return IMAGE_TEXT;
	case SHAPE_POINTER:
	case SHAPE_GRAB:
	case SHAPE_GRABBING:
		return IMAGE_HAND;
	case SHAPE_CROSSHAIR:
	case SHAPE_CELL:
		return IMAGE_CROSSHAIR;
	case SHAPE_E_RESIZE:
	case SHAPE_W_RESIZE:
	case SHAPE_EW_RESIZE:
	case SHAPE_COL_RESIZE:
		return IMAGE_EW;
	case SHAPE_N_RESIZE:
	case SHAPE_S_RESIZE:
	case SHAPE_NS_RESIZE:
	case SHAPE_ROW_RESIZE:
		return IMAGE_NS;
	case SHAPE_NW_RESIZE:
	case SHAPE_SE_RESIZE:
	case SHAPE_NWSE_RESIZE:
		return IMAGE_NWSE;
	case SHAPE_NE_RESIZE:
	case SHAPE_SW_RESIZE:
	case SHAPE_NESW_RESIZE:
		return IMAGE_NESW;
	case SHAPE_MOVE:
	case SHAPE_ALL_SCROLL:
		return IMAGE_MOVE;
	case SHAPE_NOT_ALLOWED:
	case SHAPE_NO_DROP:
		return IMAGE_NOT_ALLOWED;
	case SHAPE_WAIT:
	case SHAPE_PROGRESS:
		return IMAGE_WAIT;
	default:
		break;
	}

	/* Every other shape (default, help, copy, ...) is the arrow. */
	return IMAGE_NONE;
}

/* Tells which resize shape a frame's edges show: a side's two-way arrow, a corner's diagonal one. */
static uint32_t
frame_shape(
	uint32_t edges)
{
	/* The corners, by the diagonal they drag along. */
	if (edges == (KWL_EDGE_TOP | KWL_EDGE_LEFT))
		return SHAPE_NW_RESIZE;
	if (edges == (KWL_EDGE_BOTTOM | KWL_EDGE_RIGHT))
		return SHAPE_SE_RESIZE;
	if (edges == (KWL_EDGE_TOP | KWL_EDGE_RIGHT))
		return SHAPE_NE_RESIZE;
	if (edges == (KWL_EDGE_BOTTOM | KWL_EDGE_LEFT))
		return SHAPE_SW_RESIZE;

	/* The sides. */
	if (edges == KWL_EDGE_TOP)
		return SHAPE_N_RESIZE;
	if (edges == KWL_EDGE_BOTTOM)
		return SHAPE_S_RESIZE;
	if (edges == KWL_EDGE_LEFT)
		return SHAPE_W_RESIZE;

	/* The right side, the only one left. */
	return SHAPE_E_RESIZE;
}

/* Makes one image's figure. */
static void
mask_figure(
	unsigned index,
	struct cursor_mask *mask)
{
	/* Nothing drawn yet. */
	memset(mask, 0, sizeof(*mask));

	/* Each image's rectangles, triangles, lines and rings. */
	switch (index) {
	case IMAGE_TEXT:
		/* An I-beam: the stem and the two serifs. */
		mask_rectangle(mask, 11, 4, 13, 20);
		mask_rectangle(mask, 8, 3, 11, 5);
		mask_rectangle(mask, 13, 3, 16, 5);
		mask_rectangle(mask, 8, 19, 11, 21);
		mask_rectangle(mask, 13, 19, 16, 21);
		break;
	case IMAGE_HAND:
		/* A pointing hand: the index finger up, the palm and the other fingers below. */
		mask_rectangle(mask, 8, 1, 11, 12);
		mask_rectangle(mask, 11, 7, 14, 12);
		mask_rectangle(mask, 14, 8, 17, 13);
		mask_rectangle(mask, 5, 10, 8, 16);
		mask_rectangle(mask, 7, 12, 18, 21);
		break;
	case IMAGE_CROSSHAIR:
		/* A cross, open in the middle. */
		mask_rectangle(mask, 11, 2, 13, 10);
		mask_rectangle(mask, 11, 14, 13, 22);
		mask_rectangle(mask, 2, 11, 10, 13);
		mask_rectangle(mask, 14, 11, 22, 13);
		break;
	case IMAGE_EW:
		/* A left-right arrow. */
		mask_rectangle(mask, 5, 11, 19, 13);
		mask_arrow_heads(mask, 1U);
		break;
	case IMAGE_NS:
		/* An up-down arrow. */
		mask_rectangle(mask, 11, 5, 13, 19);
		mask_arrow_heads(mask, 0U);
		break;
	case IMAGE_NWSE:
		/* An arrow from the top left to the bottom right. */
		mask_diagonal(mask, 1U);
		break;
	case IMAGE_NESW:
		/* An arrow from the top right to the bottom left. */
		mask_diagonal(mask, 0U);
		break;
	case IMAGE_MOVE:
		/* Both straight arrows. */
		mask_rectangle(mask, 5, 11, 19, 13);
		mask_rectangle(mask, 11, 5, 13, 19);
		mask_arrow_heads(mask, 1U);
		mask_arrow_heads(mask, 0U);
		break;
	case IMAGE_NOT_ALLOWED:
		/* A ring with a slash across it. */
		mask_ring(mask);
		break;
	case IMAGE_WAIT:
		/* An hourglass. */
		mask_hourglass(mask);
		break;
	default:
		break;
	}
}

/* Fills a ring around the middle with a slash across it (not allowed). */
static void
mask_ring(
	struct cursor_mask *mask)
{
	int x;
	int y;
	int distance;
	int across;

	/* Each pixel whose distance from the middle is on the ring, or inside it on the slash. */
	for (y = 0; y < CURSOR_SIZE; y++) {
		/* The row's pixels. */
		for (x = 0; x < CURSOR_SIZE; x++) {
			/* The squared distance from the middle, and from the slash. */
			distance = (x - CURSOR_MIDDLE) * (x - CURSOR_MIDDLE) + (y - CURSOR_MIDDLE) * (y - CURSOR_MIDDLE);
			across = x - y;
			if (across < 0)
				across = -across;

			/* On the ring (radius 7 to 9), or on the slash inside it. */
			if (distance >= 49 && distance <= 81)
				mask->fill[y][x] = 1;
			if (distance < 49 && across <= 1)
				mask->fill[y][x] = 1;
		}
	}
}

/* Fills an hourglass: the top and bottom bars and two triangles meeting in the middle (wait). */
static void
mask_hourglass(
	struct cursor_mask *mask)
{
	int y;
	int distance;

	/* The bars. */
	mask_rectangle(mask, 6, 2, 18, 4);
	mask_rectangle(mask, 6, 20, 18, 22);

	/* The glass, narrowing to the middle and widening again. */
	for (y = 4; y < 20; y++) {
		/* The row's distance from the middle sets its width. */
		distance = y - CURSOR_MIDDLE;
		if (distance < 0)
			distance = -distance;
		mask_rectangle(mask, CURSOR_MIDDLE - 1 - (distance * 5) / 8, y, CURSOR_MIDDLE + 1 + (distance * 5) / 8, y + 1);
	}
}

/* Fills a rectangle of a mask (x0, y0 inclusive; x1, y1 exclusive), within the image. */
static void
mask_rectangle(
	struct cursor_mask *mask,
	int x0,
	int y0,
	int x1,
	int y1)
{
	int x;
	int y;

	/* Each pixel inside both the rectangle and the image. */
	for (y = y0; y < y1; y++) {
		/* A row outside the image. */
		if (y < 0 || y >= CURSOR_SIZE)
			continue;

		/* Its pixels. */
		for (x = x0; x < x1; x++) {
			/* A pixel inside the image. */
			if (x >= 0 && x < CURSOR_SIZE)
				mask->fill[y][x] = 1;
		}
	}
}

/* Fills the two heads of a straight arrow: left and right (horizontal), or top and bottom. */
static void
mask_arrow_heads(
	struct cursor_mask *mask,
	unsigned horizontal)
{
	int step;

	/* Each step from the tip widens the head by one pixel each side. */
	for (step = 0; step < 6; step++) {
		/* The two heads, across the arrow's line. */
		if (horizontal) {
			mask_rectangle(mask, 1 + step, 11 - step, 2 + step, 13 + step);
			mask_rectangle(mask, 22 - step, 11 - step, 23 - step, 13 + step);
		} else {
			mask_rectangle(mask, 11 - step, 1 + step, 13 + step, 2 + step);
			mask_rectangle(mask, 11 - step, 22 - step, 13 + step, 23 - step);
		}
	}
}

/* Fills a diagonal arrow: falling (top left to bottom right) or rising (top right to bottom left). */
static void
mask_diagonal(
	struct cursor_mask *mask,
	unsigned falling)
{
	int x;
	int y;
	int along;
	int across;

	/* Each pixel near the diagonal line, or inside a head at one of its ends. */
	for (y = 0; y < CURSOR_SIZE; y++) {
		/* The row's pixels. */
		for (x = 0; x < CURSOR_SIZE; x++) {
			/* How far along the line and how far across it the pixel is. */
			if (falling) {
				along = x + y;
				across = x - y;
			} else {
				along = (CURSOR_SIZE - 1 - x) + y;
				across = (CURSOR_SIZE - 1 - x) - y;
			}

			/* Across the line either way. */
			if (across < 0)
				across = -across;

			/* The line between the heads, and the heads at both ends. */
			if (across <= 1 &&
			    along >= 8 &&
			    along <= 38)
				mask->fill[y][x] = 1;
			if (along >= 4 &&
			    along <= 12 &&
			    across <= along - 4)
				mask->fill[y][x] = 1;
			if (along >= 34 &&
			    along <= 42 &&
			    across <= 42 - along)
				mask->fill[y][x] = 1;
		}
	}
}

/* Makes one image: its figure white, a black edge around it, transparent elsewhere. */
static int
image_make(
	struct kwl_server *server,
	unsigned index)
{
	struct cursor_mask mask;
	struct kwl_import *image;
	uint32_t *row;
	int x;
	int y;
	int dx;
	int dy;
	int edge;
	VkResult result;

	/* A linear, host-visible image the CPU writes once. */
	image = calloc(1, sizeof(*image));
	if (image == NULL)
		return ENOMEM;
	result = kwl_host_image_create(server->compose, CURSOR_SIZE, CURSOR_SIZE, server->compose->sampler, image);
	if (result != VK_SUCCESS) {
		kwl_host_image_release(server->compose, image);
		free(image);
		return EIO;
	}

	/* The figure. */
	mask_figure(index, &mask);

	/* Each pixel: white in the figure, black next to it, transparent elsewhere (B, G, R, A in memory). */
	for (y = 0; y < CURSOR_SIZE; y++) {
		row = (uint32_t *)((unsigned char *)image->map + (size_t)y * image->row_pitch);
		for (x = 0; x < CURSOR_SIZE; x++) {
			/* A pixel of the figure. */
			if (mask.fill[y][x]) {
				row[x] = 0xffffffffU;
				continue;
			}

			/* A pixel next to the figure is its edge. */
			edge = 0;
			for (dy = -1; dy <= 1; dy++) {
				/* The three neighbours of this row. */
				for (dx = -1; dx <= 1; dx++) {
					/* A neighbour inside the image and in the figure. */
					if (y + dy >= 0 &&
					    y + dy < CURSOR_SIZE &&
					    x + dx >= 0 &&
					    x + dx < CURSOR_SIZE &&
					    mask.fill[y + dy][x + dx])
						edge = 1;
				}
			}

			/* The edge black, anything else clear. */
			row[x] = 0x00000000U;
			if (edge)
				row[x] = 0xff000000U;
		}
	}

	/* Succeeded: drawn with alpha. */
	image->draw = KWL_DRAW_ALPHA;
	server->cursor_images[index] = image;
	return 0;
}

/* Reads one native-endian protocol word. */
static uint32_t
cursor_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
