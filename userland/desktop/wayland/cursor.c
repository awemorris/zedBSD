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

#include "compose.h"
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

/* The size of every image, and its middle (the hotspot of most), in logical units. */
#define CURSOR_SIZE			24
#define CURSOR_MIDDLE			12

/* The most pixels across an image is drawn with: its size at 300 %. */
#define CURSOR_PIXELS_MOST		72

/*
 * One image's figure, as a mask of the pixels drawn white (the black edge
 * is found around them).
 */
struct cursor_mask {
	unsigned char fill[CURSOR_SIZE][CURSOR_SIZE];
};

/*
 * One image's figure in the pixels it is drawn with: the logical mask
 * stretched to the output's scale, each pixel taking the unit its middle
 * lies in, so the figure keeps its square steps.
 */
struct cursor_pixels {
	unsigned char fill[CURSOR_PIXELS_MOST][CURSOR_PIXELS_MOST];
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
static int image_make_at(struct kwl_server *server, unsigned index, unsigned scale120, struct kwl_import **made);
static struct kwl_cursor_set *cursor_set_at(struct kwl_server *server, unsigned scale120);
static void cursor_set_release(struct kwl_server *server, struct kwl_cursor_set *set);
static void image_figure(const struct cursor_mask *mask, int size, struct cursor_pixels *pixels);
static int image_edge(const struct cursor_pixels *pixels, int size, int x, int y, int reach);
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
 * Makes the images of every cursor shape at the start, in the start's
 * batch of host images (ws213-p003, p001 review-3 M-6), so that no shape's
 * image is made while the compositor runs; returns 0, or the error of the
 * first image that could not be made (the others are still made, and a
 * missing one is made when it is first asked for, as before).
 */
int
kwl_cursor_images_create(
	struct kwl_server *server)
{
	unsigned index;
	int first_error;
	int error;

	/* Each image not made yet. */
	first_error = 0;
	for (index = 0U; index < KWL_CURSOR_IMAGES; index++) {
		/* One made already. */
		if (server->cursor_images[index] != NULL)
			continue;

		/* The image, its first failure kept. */
		error = image_make(server, index);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Some image could not be made. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: every shape has its image. */
	return 0;
}

/*
 * Makes the arrow and the shapes' images again at the output's scale when
 * it is not the one they were drawn at (the output shown at another
 * scale).  The cursor plane's thread is parked and shows nothing first,
 * so it never reads an image let go; its next update shows the new one.
 * An image that cannot be made is made when first asked for, as at the
 * start; without the arrow the cursor is not drawn.
 */
void
kwl_cursor_images_rescale(
	struct kwl_server *server)
{
	unsigned scale;
	int error;

	/* Nothing made yet, or made at this scale already. */
	if (server->compose == NULL || server->arrow == NULL)
		return;
	scale = server->scale120;
	if (scale == 0U)
		scale = KWL_SCALE_ONE;
	if (scale == server->cursor_scale120)
		return;

	/* The plane lets go of the images first. */
	kwl_cursor_plane_forget(server);

	/* The arrow and the shapes let go, then made at the new scale. */
	kwl_arrow_destroy(server);
	error = kwl_arrow_create(server);
	if (error != 0)
		printf("KWL CURSOR arrow errno=%d\n", error);
	error = kwl_cursor_images_create(server);
	if (error != 0)
		printf("KWL CURSOR images errno=%d\n", error);
	printf("KWL CURSOR scale=%u\n", server->cursor_scale120);

	/* Every output is drawn with the new cursor. */
	server->dirty = 1;
}

/*
 * Gives the image to draw the cursor with on a pass shown at a scale (a
 * head's, compose.c): the image itself (the arrow or a shape's, made at
 * the output's scale) when the scale is the output's, else the same
 * cursor drawn at that scale, made the first time it is drawn there.
 * When it cannot be made the image itself is given (drawn scaled).
 */
const struct kwl_import *
kwl_cursor_scaled(
	struct kwl_server *server,
	const struct kwl_import *image,
	unsigned scale120)
{
	struct kwl_cursor_set *set;
	struct kwl_import **slot;
	unsigned index;
	int error;

	/* The output's scale, or no image: the image itself. */
	if (image == NULL || scale120 == 0U || scale120 == server->cursor_scale120)
		return image;

	/* The set at the scale (none: drawn scaled). */
	set = cursor_set_at(server, scale120);
	if (set == NULL)
		return image;

	/* The arrow's place in the set, or the shape's. */
	slot = NULL;
	if (image == server->arrow)
		slot = &set->arrow;
	for (index = 0U; index < KWL_CURSOR_IMAGES && slot == NULL; index++) {
		if (image == server->cursor_images[index])
			slot = &set->images[index];
	}

	/* Not one of the compositor's images: as it is. */
	if (slot == NULL)
		return image;

	/* Made the first time it is drawn at the scale. */
	if (*slot == NULL) {
		if (slot == &set->arrow)
			error = kwl_arrow_make(server, scale120, slot);
		else
			error = image_make_at(server, (unsigned)(slot - set->images), scale120, slot);
		if (error != 0) {
			printf("KWL CURSOR scaled scale=%u errno=%d\n", scale120, error);
			return image;
		}
	}

	/* Succeeded: the cursor at the scale. */
	return *slot;
}

/*
 * Releases the images of the cursor shapes, and every set at another
 * scale.
 */
void
kwl_cursor_images_destroy(
	struct kwl_server *server)
{
	unsigned index;

	/* The sets at other scales. */
	for (index = 0U; index < KWL_CURSOR_SETS; index++)
		cursor_set_release(server, &server->cursor_sets[index]);

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

	/* The arrow's image, made at the start, is made now if that failed, like a client's shape's. */
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

	/* The next frame draws the new cursor, which is not part of the scene: only its place is damaged. */
	server->frame_edges = edges;
	kwl_damage_pointer(server, server->pointer_x, server->pointer_y);

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
	 * The shapes' images are made at the start (kwl_cursor_images_create,
	 * in the start's batch, ws213-p003); one that could not be made then is
	 * made the first time it is asked for, and without it the shape is drawn
	 * as the arrow.
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

/*
 * Makes one image at the scale of the cursor's images: its figure white, a
 * black edge around it as wide as a unit's pixels, transparent elsewhere.
 * At 100 % it is the logical mask as it is, with a one-pixel edge.
 */
static int
image_make(
	struct kwl_server *server,
	unsigned index)
{
	int error;

	/* Succeeded or not: the image at the output's scale, in its place. */
	error = image_make_at(server, index, server->cursor_scale120, &server->cursor_images[index]);
	return error;
}

/*
 * Makes the image of a cursor shape (its index) at a scale into *made: the
 * figure white, its edge black, a round(scale) pixels wide.  Returns 0,
 * ENOMEM or EIO (*made untouched).
 */
static int
image_make_at(
	struct kwl_server *server,
	unsigned index,
	unsigned scale120,
	struct kwl_import **made)
{
	struct cursor_mask mask;
	struct cursor_pixels pixels;
	struct kwl_import *image;
	uint32_t *row;
	int size;
	int reach;
	int x;
	int y;
	int edge;
	VkResult result;

	/* The image's pixels across, and how far its edge reaches. */
	size = kwl_scale_snap(CURSOR_SIZE, scale120);
	if (size > CURSOR_PIXELS_MOST)
		size = CURSOR_PIXELS_MOST;
	if (size < CURSOR_SIZE)
		size = CURSOR_SIZE;
	reach = kwl_scale_snap(1, scale120);
	if (reach < 1)
		reach = 1;

	/* A linear, host-visible image the CPU writes once. */
	image = calloc(1, sizeof(*image));
	if (image == NULL)
		return ENOMEM;
	result = kwl_host_image_create(server->compose, (uint32_t)size, (uint32_t)size, server->compose->sampler, image);
	if (result != VK_SUCCESS) {
		kwl_host_image_release(server->compose, image);
		free(image);
		return EIO;
	}

	/* The figure, in the image's pixels. */
	mask_figure(index, &mask);
	image_figure(&mask, size, &pixels);

	/* Each pixel: white in the figure, black next to it, transparent elsewhere (B, G, R, A in memory). */
	for (y = 0; y < size; y++) {
		row = (uint32_t *)((unsigned char *)image->map + (size_t)y * image->row_pitch);
		for (x = 0; x < size; x++) {
			/* A pixel of the figure. */
			if (pixels.fill[y][x]) {
				row[x] = 0xffffffffU;
				continue;
			}

			/* The edge black, anything else clear. */
			edge = image_edge(&pixels, size, x, y, reach);
			row[x] = 0x00000000U;
			if (edge)
				row[x] = 0xff000000U;
		}
	}

	/* Succeeded: drawn with alpha. */
	image->draw = KWL_DRAW_ALPHA;
	*made = image;
	return 0;
}

/*
 * Finds the set of the cursor's images at a scale, or takes one for it:
 * an empty set, else one whose scale no open head of the extended mode
 * shows now (its images let go).  Returns NULL when every set is shown
 * (more scales than sets).
 */
static struct kwl_cursor_set *
cursor_set_at(
	struct kwl_server *server,
	unsigned scale120)
{
	struct kwl_cursor_set *set;
	struct kwl_compose *compose;
	unsigned index;
	unsigned head;
	unsigned shown;

	/* The set at the scale already. */
	for (index = 0U; index < KWL_CURSOR_SETS; index++) {
		if (server->cursor_sets[index].scale120 == scale120)
			return &server->cursor_sets[index];
	}

	/* An empty set, else one no head shows. */
	compose = server->compose;
	for (index = 0U; index < KWL_CURSOR_SETS; index++) {
		set = &server->cursor_sets[index];
		shown = 0U;
		for (head = 0U; head < KWL_HEADS && set->scale120 != 0U; head++) {
			if (compose->heads[head].open && compose->heads[head].scale120 == set->scale120)
				shown = 1U;
		}

		/* A set some head shows stays. */
		if (shown)
			continue;

		/* Taken for the scale; its images are made when drawn. */
		cursor_set_release(server, set);
		set->scale120 = scale120;
		return set;
	}

	/* Succeeded: no set free. */
	return NULL;
}

/* Lets go of a set's images; it is empty after. */
static void
cursor_set_release(
	struct kwl_server *server,
	struct kwl_cursor_set *set)
{
	unsigned index;

	/* The arrow. */
	if (set->arrow != NULL) {
		kwl_host_image_release(server->compose, set->arrow);
		free(set->arrow);
		set->arrow = NULL;
	}

	/* Each shape's image made. */
	for (index = 0U; index < KWL_CURSOR_IMAGES; index++) {
		if (set->images[index] == NULL)
			continue;
		kwl_host_image_release(server->compose, set->images[index]);
		free(set->images[index]);
		set->images[index] = NULL;
	}

	/* Succeeded: empty. */
	set->scale120 = 0U;
}

/* Stretches a logical mask to an image of size pixels across: each pixel takes the unit its middle lies in. */
static void
image_figure(
	const struct cursor_mask *mask,
	int size,
	struct cursor_pixels *pixels)
{
	int x;
	int y;
	int unit_x;
	int unit_y;

	/* Nothing outside the image. */
	memset(pixels, 0, sizeof(*pixels));

	/* Each pixel's middle, (2 p + 1) / (2 size) of the way across, in the logical mask. */
	for (y = 0; y < size; y++) {
		unit_y = ((2 * y + 1) * CURSOR_SIZE) / (2 * size);
		for (x = 0; x < size; x++) {
			unit_x = ((2 * x + 1) * CURSOR_SIZE) / (2 * size);
			pixels->fill[y][x] = mask->fill[unit_y][unit_x];
		}
	}
}

/* Tells whether a pixel outside the figure lies within reach pixels of it (either way, and diagonally): its edge. */
static int
image_edge(
	const struct cursor_pixels *pixels,
	int size,
	int x,
	int y,
	int reach)
{
	int dx;
	int dy;

	/* The pixels around it, inside the image. */
	for (dy = -reach; dy <= reach; dy++) {
		/* A row outside the image. */
		if (y + dy < 0 || y + dy >= size)
			continue;

		/* That row's pixels. */
		for (dx = -reach; dx <= reach; dx++) {
			/* A pixel of the figure inside the image. */
			if (x + dx >= 0 &&
			    x + dx < size &&
			    pixels->fill[y + dy][x + dx])
				return 1;
		}
	}

	/* Nothing of the figure near it. */
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
