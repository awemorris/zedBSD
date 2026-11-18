/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wp_viewporter (ws035-p080): a surface shows a rectangle of its buffer
 * (the source) at a size of its own (the destination), so a client can
 * scale a video or crop an image without redrawing it.
 *
 * The source and the destination are double-buffered: set_source and
 * set_destination change the pending state, and the surface's commit
 * applies it.  A surface's size is then the destination when one is set,
 * else the source's size when that is whole pixels, else the buffer's
 * (kwl_surface_size); the source is where the drawing samples the buffer
 * (kwl_viewport_source).
 */

#include "extras.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The requests of wp_viewporter and of a surface's wp_viewport. */
#define VIEWPORTER_DESTROY		0U
#define VIEWPORTER_GET_VIEWPORT		1U
#define VIEWPORT_DESTROY		0U
#define VIEWPORT_SET_SOURCE		1U
#define VIEWPORT_SET_DESTINATION	2U

/* The errors: a surface with a viewport already; a value out of range; a viewport whose surface has gone. */
#define VIEWPORTER_ERROR_VIEWPORT_EXISTS	0U
#define VIEWPORT_ERROR_BAD_VALUE		0U
#define VIEWPORT_ERROR_NO_SURFACE		3U

/* -1 in 24.8 fixed point, which unsets the source. */
#define FIXED_MINUS_ONE			(-256)

static int viewport_create(struct kwl_object *viewporter, const unsigned char *bytes, size_t size);
static int viewport_source(struct kwl_object *viewport, struct kwl_object *surface, const unsigned char *bytes, size_t size);
static int viewport_destination(struct kwl_object *viewport, struct kwl_object *surface, const unsigned char *bytes, size_t size);
static uint32_t viewport_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of wp_viewporter or of a surface's wp_viewport.
 */
int
kwl_viewport_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	int error;

	/* The viewporter: it goes, or it makes a surface's viewport. */
	if (object->kind == KWL_VIEWPORTER) {
		if (opcode == VIEWPORTER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_viewport is left. */
		if (opcode != VIEWPORTER_GET_VIEWPORT)
			return EPROTO;
		error = viewport_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The viewport goes; the surface's next commit shows it without one (kwl_viewport_object_gone). */
	if (opcode == VIEWPORT_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* The other requests need the surface. */
	surface = object->surface;
	if (surface == NULL) {
		(void)kwl_error_code(object->client, object->id, VIEWPORT_ERROR_NO_SURFACE, "the surface has gone");
		return EPROTO;
	}

	/* The source rectangle, or the destination size. */
	if (opcode == VIEWPORT_SET_SOURCE) {
		error = viewport_source(object, surface, bytes, size);
	} else if (opcode == VIEWPORT_SET_DESTINATION) {
		error = viewport_destination(object, surface, bytes, size);
	} else {
		error = EPROTO;
	}

	/* Reports a request that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the pending state changed. */
	return 0;
}

/*
 * Applies a surface's pending viewport state with its commit.
 */
void
kwl_viewport_commit(
	struct kwl_object *surface)
{
	/* Nothing changed since the last commit. */
	if (!surface->viewport_changed)
		return;

	/* The pending source and destination become the surface's. */
	memcpy(surface->source, surface->pending_source, sizeof(surface->source));
	memcpy(surface->destination, surface->pending_destination, sizeof(surface->destination));
	surface->viewport_changed = 0;
	surface->client->server->dirty = 1;
	printf("KWL VIEWPORT surface=%u source=%d,%d,%d,%d destination=%d,%d\n", surface->id,
	       surface->source[0], surface->source[1], surface->source[2], surface->source[3],
	       surface->destination[0], surface->destination[1]);
}

/*
 * Unties an object that is going from the viewports: a viewport's surface
 * loses it (the next commit shows the whole buffer at its size); a
 * surface's viewport names nothing.
 */
void
kwl_viewport_object_gone(
	struct kwl_object *object)
{
	struct kwl_object *surface;

	/* A viewport: its surface's pending state is reset. */
	if (object->kind == KWL_VIEWPORT) {
		surface = object->surface;
		object->surface = NULL;
		if (surface == NULL)
			return;
		surface->viewport = NULL;
		memset(surface->pending_source, 0, sizeof(surface->pending_source));
		memset(surface->pending_destination, 0, sizeof(surface->pending_destination));
		surface->viewport_changed = 1;
		return;
	}

	/* A surface: its viewport names nothing. */
	if (object->kind == KWL_SURFACE && object->viewport != NULL) {
		object->viewport->surface = NULL;
		object->viewport = NULL;
	}
}

/*
 * Finds a surface's size: the viewport's destination, else the source's
 * size when it is whole pixels, else the current buffer's (0 by 0 without
 * one).
 */
void
kwl_surface_size(
	const struct kwl_object *surface,
	uint32_t *width,
	uint32_t *height)
{
	/* The destination. */
	if (surface->destination[0] > 0) {
		*width = (uint32_t)surface->destination[0];
		*height = (uint32_t)surface->destination[1];
		return;
	}

	/* A source of whole pixels. */
	if (surface->source[2] > 0 &&
	    (surface->source[2] & 0xff) == 0 &&
	    (surface->source[3] & 0xff) == 0) {
		*width = (uint32_t)(surface->source[2] >> 8);
		*height = (uint32_t)(surface->source[3] >> 8);
		return;
	}

	/* The buffer's size. */
	*width = 0;
	*height = 0;
	if (surface->current != NULL)
		kwl_buffer_size(surface->current, width, height);
}

/*
 * Finds the part of a surface's buffer that is drawn, as fractions of the
 * buffer (left, top, right, bottom): the viewport's source, or all of it.
 * The source is kept inside the buffer.
 */
void
kwl_viewport_source(
	const struct kwl_object *surface,
	float *uv)
{
	uint32_t width;
	uint32_t height;
	float right;
	float bottom;

	/* All of the buffer, without a source or a buffer. */
	uv[0] = 0.0f;
	uv[1] = 0.0f;
	uv[2] = 1.0f;
	uv[3] = 1.0f;
	if (surface->source[2] <= 0 || surface->current == NULL)
		return;
	kwl_buffer_size(surface->current, &width, &height);
	if (width == 0U || height == 0U)
		return;

	/* The source's edges as fractions of the buffer's size. */
	uv[0] = (float)surface->source[0] / 256.0f / (float)width;
	uv[1] = (float)surface->source[1] / 256.0f / (float)height;
	right = (float)(surface->source[0] + surface->source[2]) / 256.0f / (float)width;
	bottom = (float)(surface->source[1] + surface->source[3]) / 256.0f / (float)height;

	/* Succeeded: kept inside the buffer. */
	uv[2] = right;
	if (right > 1.0f)
		uv[2] = 1.0f;
	uv[3] = bottom;
	if (bottom > 1.0f)
		uv[3] = 1.0f;
}

/* Makes a surface's viewport (one per surface). */
static int
viewport_create(
	struct kwl_object *viewporter,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	struct kwl_object *created;
	uint32_t id;
	uint32_t surface_id;

	/* The new ID and the client's surface. */
	if (size != 8U)
		return EPROTO;
	id = viewport_word(bytes, 0U);
	surface_id = viewport_word(bytes, 4U);
	surface = kwl_find(viewporter->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* One viewport per surface. */
	if (surface->viewport != NULL) {
		(void)kwl_error_code(viewporter->client, viewporter->id, VIEWPORTER_ERROR_VIEWPORT_EXISTS, "the surface has a viewport");
		return EPROTO;
	}

	/* Succeeded: the viewport, tied to its surface both ways. */
	created = kwl_create(viewporter->client, id, KWL_VIEWPORT, viewporter->version);
	if (created == NULL)
		return EPROTO;
	created->surface = surface;
	surface->viewport = created;
	return 0;
}

/* Takes the source rectangle (24.8 fixed point; all -1 unsets it). */
static int
viewport_source(
	struct kwl_object *viewport,
	struct kwl_object *surface,
	const unsigned char *bytes,
	size_t size)
{
	int32_t rectangle[4];
	unsigned index;

	/* Four fixed-point numbers. */
	if (size != 16U)
		return EPROTO;
	for (index = 0; index < 4U; index++)
		rectangle[index] = (int32_t)viewport_word(bytes, (size_t)index * 4U);

	/* All -1: no source. */
	if (rectangle[0] == FIXED_MINUS_ONE &&
	    rectangle[1] == FIXED_MINUS_ONE &&
	    rectangle[2] == FIXED_MINUS_ONE &&
	    rectangle[3] == FIXED_MINUS_ONE) {
		memset(surface->pending_source, 0, sizeof(surface->pending_source));
		surface->viewport_changed = 1;
		return 0;
	}

	/* Otherwise the place must not be negative and the size must be positive. */
	if (rectangle[0] < 0 ||
	    rectangle[1] < 0 ||
	    rectangle[2] <= 0 ||
	    rectangle[3] <= 0) {
		(void)kwl_error_code(viewport->client, viewport->id, VIEWPORT_ERROR_BAD_VALUE, "not a source rectangle");
		return EPROTO;
	}

	/* Succeeded: the pending source. */
	memcpy(surface->pending_source, rectangle, sizeof(rectangle));
	surface->viewport_changed = 1;
	return 0;
}

/* Takes the destination size (-1 by -1 unsets it). */
static int
viewport_destination(
	struct kwl_object *viewport,
	struct kwl_object *surface,
	const unsigned char *bytes,
	size_t size)
{
	int32_t width;
	int32_t height;

	/* Two integers. */
	if (size != 8U)
		return EPROTO;
	width = (int32_t)viewport_word(bytes, 0U);
	height = (int32_t)viewport_word(bytes, 4U);

	/* -1 by -1: no destination. */
	if (width == -1 && height == -1) {
		surface->pending_destination[0] = 0;
		surface->pending_destination[1] = 0;
		surface->viewport_changed = 1;
		return 0;
	}

	/* Otherwise both must be positive. */
	if (width <= 0 || height <= 0) {
		(void)kwl_error_code(viewport->client, viewport->id, VIEWPORT_ERROR_BAD_VALUE, "not a destination size");
		return EPROTO;
	}

	/* Succeeded: the pending destination. */
	surface->pending_destination[0] = width;
	surface->pending_destination[1] = height;
	surface->viewport_changed = 1;
	return 0;
}

/* Reads one native-endian protocol word. */
static uint32_t
viewport_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
