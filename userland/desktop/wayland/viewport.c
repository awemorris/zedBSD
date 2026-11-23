/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wp_viewporter (ws035-p080) and the buffer scale: a surface shows a
 * rectangle of its buffer (the source) at a size of its own (the
 * destination), so a client can scale a video or crop an image without
 * redrawing it, and a buffer may hold more pixels than the surface has
 * units (wl_surface.set_buffer_scale), so a client draws a display's
 * pixels for a surface of the plane's units.
 *
 * The source, the destination and the buffer scale are double-buffered:
 * set_source, set_destination and set_buffer_scale change the pending
 * state, and the surface's commit applies it.  The source is in the
 * surface's units before the viewport, that is the buffer's pixels divided
 * by the buffer scale.  A surface's size is then the destination when one
 * is set, else the source's size when that is whole units, else the
 * buffer's divided by the buffer scale (kwl_surface_size); the source is
 * where the drawing samples the buffer (kwl_viewport_source).
 */

#include "extras.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
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
static void viewport_size(const struct kwl_object *surface, const struct kwl_object *buffer, uint32_t *width, uint32_t *height);
static void viewport_map(const struct kwl_object *surface, const struct kwl_object *buffer, double *source, uint32_t *buffer_width, uint32_t *buffer_height, uint32_t *width, uint32_t *height);
static int32_t viewport_clamp(int64_t value, int64_t least, int64_t most);
static int64_t viewport_floor(double value);
static int64_t viewport_ceil(double value);

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

	/* The pending source, destination and buffer scale become the surface's. */
	memcpy(surface->source, surface->pending_source, sizeof(surface->source));
	memcpy(surface->destination, surface->pending_destination, sizeof(surface->destination));
	surface->buffer_scale = surface->pending_buffer_scale;
	surface->viewport_changed = 0;
	surface->client->server->dirty = 1;
	printf("KWL VIEWPORT surface=%u source=%d,%d,%d,%d destination=%d,%d buffer_scale=%u\n", surface->id,
	       surface->source[0], surface->source[1], surface->source[2], surface->source[3],
	       surface->destination[0], surface->destination[1], kwl_surface_buffer_scale(surface));
}

/*
 * Takes a surface's pending buffer scale (wl_surface.set_buffer_scale),
 * which its next commit applies with the viewport.
 */
void
kwl_viewport_buffer_scale(
	struct kwl_object *surface,
	uint32_t scale)
{
	/* The scale waits for the commit, which applies the viewport's state with it. */
	surface->pending_buffer_scale = scale;
	surface->viewport_changed = 1;
}

/*
 * Reports a surface's buffer scale: the pixels of its buffer that make
 * one unit of the surface each way, 1 for a surface that never set one.
 */
uint32_t
kwl_surface_buffer_scale(
	const struct kwl_object *surface)
{
	/* A surface that never set one draws a pixel for a unit. */
	if (surface->buffer_scale == 0U)
		return 1U;

	/* Succeeded: the scale the client set. */
	return surface->buffer_scale;
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
 * size when it is whole units, else the current buffer's divided by its
 * buffer scale (0 by 0 without one).
 */
void
kwl_surface_size(
	const struct kwl_object *surface,
	uint32_t *width,
	uint32_t *height)
{
	/* The size its current buffer shows it at. */
	viewport_size(surface, surface->current, width, height);
}

/*
 * Finds the part of a surface's buffer that is drawn, as fractions of the
 * buffer (left, top, right, bottom): the viewport's source, or all of it.
 * The source is in the surface's units, so it is multiplied by the buffer
 * scale to name the buffer's pixels.  The source is kept inside the
 * buffer.
 */
void
kwl_viewport_source(
	const struct kwl_object *surface,
	float *uv)
{
	uint32_t width;
	uint32_t height;
	float units;
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

	/* The buffer's pixels for one 256th of a unit of the source. */
	units = (float)kwl_surface_buffer_scale(surface) / 256.0f;

	/* The source's edges as fractions of the buffer's size. */
	uv[0] = (float)surface->source[0] * units / (float)width;
	uv[1] = (float)surface->source[1] * units / (float)height;
	right = ((float)surface->source[0] + (float)surface->source[2]) * units / (float)width;
	bottom = ((float)surface->source[1] + (float)surface->source[3]) * units / (float)height;

	/* Succeeded: kept inside the buffer. */
	uv[2] = right;
	if (right > 1.0f)
		uv[2] = 1.0f;
	uv[3] = bottom;
	if (bottom > 1.0f)
		uv[3] = 1.0f;
}

/*
 * Turns a rectangle of a surface's units (x0, y0, x1, y1: wl_surface.damage)
 * to the pixels of a buffer the surface shows (the one its commit
 * attached), through the viewport's destination and source and the buffer
 * scale the surface has now, taken outward and kept inside the buffer.
 * Reports 0 for a rectangle that covers no pixel (or no buffer), else 1.
 */
int
kwl_viewport_damage_buffer(
	const struct kwl_object *surface,
	const struct kwl_object *buffer,
	const int32_t *units,
	int32_t *pixels)
{
	double source[4];
	uint32_t buffer_width;
	uint32_t buffer_height;
	uint32_t width;
	uint32_t height;
	int64_t edges[4];
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;

	/* The surface's size, its source in the buffer's pixels and the buffer's size. */
	viewport_map(surface, buffer, source, &buffer_width, &buffer_height, &width, &height);
	if (width == 0U ||
	    height == 0U ||
	    buffer_width == 0U ||
	    buffer_height == 0U)
		return 0;

	/* The rectangle kept inside the surface: what lies outside shows no pixel. */
	left = viewport_clamp(units[0], 0, (int64_t)width);
	top = viewport_clamp(units[1], 0, (int64_t)height);
	right = viewport_clamp(units[2], 0, (int64_t)width);
	bottom = viewport_clamp(units[3], 0, (int64_t)height);
	if (right <= left || bottom <= top)
		return 0;

	/*
	 * Each edge goes through the destination to the source (the source's
	 * place plus the edge's share of its size) and on to the buffer's
	 * pixels (times the buffer scale's pixels a unit), the left and top
	 * rounded down and the right and bottom up.
	 */
	edges[0] = viewport_floor(source[0] + (double)left * source[2] / (double)width);
	edges[1] = viewport_floor(source[1] + (double)top * source[3] / (double)height);
	edges[2] = viewport_ceil(source[0] + (double)right * source[2] / (double)width);
	edges[3] = viewport_ceil(source[1] + (double)bottom * source[3] / (double)height);

	/* The edges kept inside the buffer. */
	pixels[0] = viewport_clamp(edges[0], 0, (int64_t)buffer_width);
	pixels[1] = viewport_clamp(edges[1], 0, (int64_t)buffer_height);
	pixels[2] = viewport_clamp(edges[2], 0, (int64_t)buffer_width);
	pixels[3] = viewport_clamp(edges[3], 0, (int64_t)buffer_height);

	/* A source that shows none of the buffer leaves nothing. */
	if (pixels[2] <= pixels[0] || pixels[3] <= pixels[1])
		return 0;

	/* Succeeded: the buffer's pixels the rectangle shows. */
	return 1;
}

/*
 * Turns a rectangle of a surface's current buffer's pixels (x0, y0, x1,
 * y1: the committed damage) to the surface's units it is shown at, through
 * the buffer scale and the viewport's source and destination the surface
 * has now, taken outward and kept inside the surface.  Reports 0 for a
 * rectangle the surface does not show (or a surface without a buffer),
 * else 1.
 */
int
kwl_viewport_damage_surface(
	const struct kwl_object *surface,
	const int32_t *pixels,
	int32_t *units)
{
	double source[4];
	uint32_t buffer_width;
	uint32_t buffer_height;
	uint32_t width;
	uint32_t height;
	int64_t edges[4];

	/* The surface's size, its source in the buffer's pixels and its buffer's size. */
	viewport_map(surface, surface->current, source, &buffer_width, &buffer_height, &width, &height);
	if (width == 0U ||
	    height == 0U ||
	    source[2] <= 0.0 ||
	    source[3] <= 0.0)
		return 0;

	/* Each edge from the source's share to the destination, the left and top rounded down and the right and bottom up. */
	edges[0] = viewport_floor(((double)pixels[0] - source[0]) * (double)width / source[2]);
	edges[1] = viewport_floor(((double)pixels[1] - source[1]) * (double)height / source[3]);
	edges[2] = viewport_ceil(((double)pixels[2] - source[0]) * (double)width / source[2]);
	edges[3] = viewport_ceil(((double)pixels[3] - source[1]) * (double)height / source[3]);

	/* The edges kept inside the surface. */
	units[0] = viewport_clamp(edges[0], 0, (int64_t)width);
	units[1] = viewport_clamp(edges[1], 0, (int64_t)height);
	units[2] = viewport_clamp(edges[2], 0, (int64_t)width);
	units[3] = viewport_clamp(edges[3], 0, (int64_t)height);

	/* A rectangle outside the source shows nothing. */
	if (units[2] <= units[0] || units[3] <= units[1])
		return 0;

	/* Succeeded: the surface's units the rectangle is shown at. */
	return 1;
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

/*
 * Finds the size a buffer shows a surface at: the viewport's destination,
 * else the source's size when it is whole units, else the buffer's
 * divided by the buffer scale (0 by 0 without a buffer).
 */
static void
viewport_size(
	const struct kwl_object *surface,
	const struct kwl_object *buffer,
	uint32_t *width,
	uint32_t *height)
{
	uint32_t scale;

	/* The destination. */
	if (surface->destination[0] > 0) {
		*width = (uint32_t)surface->destination[0];
		*height = (uint32_t)surface->destination[1];
		return;
	}

	/* A source of whole units. */
	if (surface->source[2] > 0 &&
	    (surface->source[2] & 0xff) == 0 &&
	    (surface->source[3] & 0xff) == 0) {
		*width = (uint32_t)(surface->source[2] >> 8);
		*height = (uint32_t)(surface->source[3] >> 8);
		return;
	}

	/* No buffer has no size. */
	*width = 0;
	*height = 0;
	if (buffer == NULL)
		return;

	/* The buffer's size in the surface's units, rounded down (a client should give a whole multiple). */
	kwl_buffer_size(buffer, width, height);
	scale = kwl_surface_buffer_scale(surface);
	if (scale == 1U)
		return;
	*width /= scale;
	*height /= scale;

	/* A buffer smaller than its scale still shows a unit. */
	if (*width == 0U)
		*width = 1U;
	if (*height == 0U)
		*height = 1U;
}

/*
 * Finds what maps a surface's units to a buffer's pixels: the source in
 * the buffer's pixels (left, top, width, height: the viewport's source
 * times the buffer scale, or the whole buffer), the buffer's size and the
 * size the buffer shows the surface at.  All are 0 without a buffer.
 */
static void
viewport_map(
	const struct kwl_object *surface,
	const struct kwl_object *buffer,
	double *source,
	uint32_t *buffer_width,
	uint32_t *buffer_height,
	uint32_t *width,
	uint32_t *height)
{
	double scale;

	/* Nothing is shown without a buffer. */
	source[0] = 0.0;
	source[1] = 0.0;
	source[2] = 0.0;
	source[3] = 0.0;
	*buffer_width = 0U;
	*buffer_height = 0U;
	*width = 0U;
	*height = 0U;
	if (buffer == NULL)
		return;

	/* The buffer's size and the surface's. */
	kwl_buffer_size(buffer, buffer_width, buffer_height);
	viewport_size(surface, buffer, width, height);

	/* The whole buffer, without a source. */
	if (surface->source[2] <= 0) {
		source[2] = (double)*buffer_width;
		source[3] = (double)*buffer_height;
		return;
	}

	/* The source's 256ths of a unit, each the buffer scale's pixels over 256. */
	scale = (double)kwl_surface_buffer_scale(surface) / 256.0;
	source[0] = (double)surface->source[0] * scale;
	source[1] = (double)surface->source[1] * scale;
	source[2] = (double)surface->source[2] * scale;
	source[3] = (double)surface->source[3] * scale;
}

/* Keeps a value between two bounds that fit in 32 bits. */
static int32_t
viewport_clamp(
	int64_t value,
	int64_t least,
	int64_t most)
{
	/* Below the least. */
	if (value < least)
		return (int32_t)least;

	/* Above the most. */
	if (value > most)
		return (int32_t)most;

	/* Succeeded: the value, inside. */
	return (int32_t)value;
}

/* Rounds a value down to a whole number, kept in 64 bits (a damage's edge far outside is kept at the bound). */
static int64_t
viewport_floor(
	double value)
{
	/* A value past what 64 bits hold is at their bound; the caller keeps it inside the buffer. */
	if (value <= -9.0e18)
		return INT64_MIN / 2;
	if (value >= 9.0e18)
		return INT64_MAX / 2;

	/* Succeeded: rounded down. */
	return (int64_t)floor(value);
}

/* Rounds a value up to a whole number, kept in 64 bits. */
static int64_t
viewport_ceil(
	double value)
{
	/* A value past what 64 bits hold is at their bound; the caller keeps it inside the buffer. */
	if (value <= -9.0e18)
		return INT64_MIN / 2;
	if (value >= 9.0e18)
		return INT64_MAX / 2;

	/* Succeeded: rounded up. */
	return (int64_t)ceil(value);
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
