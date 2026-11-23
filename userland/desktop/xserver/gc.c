/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Graphics contexts: CreateGC, ChangeGC, CopyGC, SetDashes,
 * SetClipRectangles and FreeGC, and the values a context keeps.
 *
 * A context belongs to the depth of the drawable it was made for.  Every
 * value is checked before any is kept, so a request that fails leaves the
 * context as it was.  The tile, the stipple and the clip mask are copied
 * when they are set (the protocol leaves the effect of later changes to
 * those pixmaps undefined), the clip mask as the region of its set
 * pixels; the copies and a list of dashes are charged to the context's
 * owner.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The values' bits, in the order a request gives them, and how many there are. */
#define GC_FUNCTION		0U
#define GC_PLANE_MASK		1U
#define GC_FOREGROUND		2U
#define GC_BACKGROUND		3U
#define GC_LINE_WIDTH		4U
#define GC_LINE_STYLE		5U
#define GC_CAP_STYLE		6U
#define GC_JOIN_STYLE		7U
#define GC_FILL_STYLE		8U
#define GC_FILL_RULE		9U
#define GC_TILE			10U
#define GC_STIPPLE		11U
#define GC_TILE_X		12U
#define GC_TILE_Y		13U
#define GC_FONT			14U
#define GC_SUBWINDOW_MODE	15U
#define GC_EXPOSURES		16U
#define GC_CLIP_X		17U
#define GC_CLIP_Y		18U
#define GC_CLIP_MASK		19U
#define GC_DASH_OFFSET		20U
#define GC_DASHES		21U
#define GC_ARC_MODE		22U
#define GC_COUNT		23U

/* The largest value of each enumerated value. */
#define GC_FUNCTION_MAX		15U
#define GC_LINE_STYLE_MAX	2U
#define GC_CAP_STYLE_MAX	3U
#define GC_JOIN_STYLE_MAX	2U
#define GC_FILL_STYLE_MAX	3U
#define GC_BOOLEAN_MAX		1U

/* The default dash length, and the clip mask None. */
#define GC_DEFAULT_DASHES	4U
#define GC_NONE			0U

/* The largest ordering SetClipRectangles names (Unsorted, YSorted, YXSorted, YXBanded). */
#define GC_ORDERING_MAX		3U

/*
 * The values a request gives: which bits it names, each value, and the
 * pixmaps the tile, stipple and clip mask name (NULL: none named, or
 * None for the clip mask).
 */
struct gc_values {
	uint32_t mask;
	uint32_t value[GC_COUNT];
	struct x11_pixmap *tile;
	struct x11_pixmap *stipple;
	struct x11_pixmap *clip;
};

static unsigned gc_read(struct x11server *server, const struct x11_gc *gc, int msb, const uint8_t *values, uint32_t mask, struct gc_values *read);
static unsigned gc_apply(struct x11server *server, struct x11_gc *gc, const struct gc_values *values);
static int gc_clip_region(const struct x11_image *bitmap, struct x11_region *region);
static void gc_defaults(struct x11_gc *gc);
static size_t gc_region_bytes(const struct x11_region *region);
static size_t gc_kept_bytes(const struct x11_gc *gc);
static void gc_recharge(struct x11server *server, struct x11_gc *gc, size_t added);
static unsigned gc_copy(struct x11server *server, struct x11_gc *target, const struct x11_gc *source, uint32_t mask);

/*
 * CreateGC: a graphics context for a drawable's depth, with the
 * protocol's defaults for the values its request does not give.
 */
unsigned
x11_request_create_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct gc_values values;
	struct x11_client *client;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	struct x11_gc *gc;
	uint32_t mask;
	uint32_t id;
	unsigned depth;
	unsigned code;
	unsigned taken;
	int chosen;
	int fits;
	int error;

	/* An id of the client's range not in use, and one value for each bit of the mask. */
	client = &server->clients[index];
	if (length < 16U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 12, client->order);
	if ((mask >> GC_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 16U, length);
	if (!fits)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, client->order);
	chosen = x11_id_chosen(client, id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The drawable, which gives the depth (an InputOnly window has none). */
	window = x11_window_find(server, x11_read32(request + 8, client->order));
	pixmap = NULL;
	if (window == NULL)
		pixmap = x11_pixmap_find(server, x11_read32(request + 8, client->order));
	if (window == NULL && pixmap == NULL)
		return X11_BAD_DRAWABLE;
	depth = X11_DEPTH;
	if (pixmap != NULL)
		depth = pixmap->depth;
	if (window != NULL && window->window_class == X11_INPUT_ONLY)
		return X11_BAD_MATCH;

	/* The context with the defaults, charged to the client. */
	error = x11_memory_charge(server, index, sizeof(*gc));
	if (error != 0)
		return X11_BAD_ALLOC;
	gc = calloc(1U, sizeof(*gc));
	if (gc == NULL) {
		x11_memory_release(server, index, sizeof(*gc));
		return X11_BAD_ALLOC;
	}
	gc->id = id;
	gc->owner = index;
	gc->depth = depth;
	gc_defaults(gc);

	/* Its values, every one checked before any is kept. */
	code = gc_read(server, gc, client->order, request + 16, mask, &values);
	if (code != 0U) {
		x11_gc_free(server, gc);
		return code;
	}

	/* The values kept, with the copies they name. */
	code = gc_apply(server, gc, &values);
	if (code != 0U) {
		x11_gc_free(server, gc);
		return code;
	}

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_GC, gc);
	if (error != 0) {
		x11_gc_free(server, gc);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the context is made. */
	return 0U;
}

/*
 * ChangeGC: new values of a graphics context, kept only when every one
 * is right.
 */
unsigned
x11_request_change_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct gc_values values;
	struct x11_client *client;
	struct x11_gc *gc;
	uint32_t mask;
	unsigned code;
	int fits;

	/* The context, and one value for each bit of the mask. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 8, client->order);
	if ((mask >> GC_COUNT) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 12U, length);
	if (!fits)
		return X11_BAD_LENGTH;
	gc = x11_gc_find(server, x11_read32(request + 4, client->order));
	if (gc == NULL)
		return X11_BAD_GC;

	/* The values, checked. */
	code = gc_read(server, gc, client->order, request + 12, mask, &values);
	if (code != 0U)
		return code;

	/* The values kept (BadAlloc leaves the context as it was). */
	code = gc_apply(server, gc, &values);
	if (code != 0U)
		return code;

	/* Succeeded: the context is changed. */
	return 0U;
}

/*
 * FreeGC: the context goes.
 */
unsigned
x11_request_free_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_gc *gc;

	/* The context of the id. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	gc = x11_gc_find(server, x11_read32(request + 4, server->clients[index].order));
	if (gc == NULL)
		return X11_BAD_GC;

	/* Succeeded: its id goes, and it with its copies and charge. */
	x11_resource_remove(server, gc->id);
	x11_gc_free(server, gc);
	return 0U;
}

/*
 * CopyGC: values of one graphics context copied into another of the same
 * depth, the tile, stipple, clip and dashes as copies of their own.
 */
unsigned
x11_request_copy_gc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_gc *source;
	struct x11_gc *target;
	uint32_t mask;
	unsigned code;
	int msb;

	/* The two contexts and the values named. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	source = x11_gc_find(server, x11_read32(request + 4, msb));
	if (source == NULL)
		return X11_BAD_GC;
	target = x11_gc_find(server, x11_read32(request + 8, msb));
	if (target == NULL)
		return X11_BAD_GC;
	mask = x11_read32(request + 12, msb);
	if ((mask >> GC_COUNT) != 0U)
		return X11_BAD_VALUE;

	/* Contexts of different depths cannot share values. */
	if (source->depth != target->depth)
		return X11_BAD_MATCH;

	/* The values copied (BadAlloc leaves the target as it was). */
	code = gc_copy(server, target, source, mask);
	if (code != 0U)
		return code;

	/* Succeeded: copied. */
	return 0U;
}

/*
 * SetDashes: the dash offset and a list of dash lengths, none of them
 * zero.
 */
unsigned
x11_request_set_dashes(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_gc *gc;
	uint8_t *list;
	size_t count;
	size_t at;
	int error;
	int msb;

	/* The context, and the lengths filling the request to its last word. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	count = x11_read16(request + 10, msb);
	if (length != 12U + ((count + 3U) & ~(size_t)3U))
		return X11_BAD_LENGTH;
	gc = x11_gc_find(server, x11_read32(request + 4, msb));
	if (gc == NULL)
		return X11_BAD_GC;

	/* At least one length, and no length of zero. */
	if (count == 0U)
		return X11_BAD_VALUE;
	for (at = 0U; at < count; at++) {
		if (request[12U + at] == 0U)
			return X11_BAD_VALUE;
	}

	/* The list's copy, charged to the context's owner. */
	error = x11_memory_charge(server, gc->owner, count);
	if (error != 0)
		return X11_BAD_ALLOC;
	list = malloc(count);
	if (list == NULL) {
		x11_memory_release(server, gc->owner, count);
		return X11_BAD_ALLOC;
	}
	memcpy(list, request + 12, count);

	/* Succeeded: the offset and the list replace the dashes, and the charge follows them. */
	free(gc->dash_list);
	gc->dash_list = list;
	gc->dash_count = count;
	gc->dash_offset = x11_read16(request + 8, msb);
	gc_recharge(server, gc, count);
	return 0U;
}

/*
 * SetClipRectangles: the clip is the rectangles (on the clip origin),
 * and the clip origin; no rectangle clips everything away.
 */
unsigned
x11_request_set_clip_rectangles(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_region clip;
	struct x11_box box;
	struct x11_gc *gc;
	size_t offset;
	size_t added;
	int error;
	int msb;

	/* The ordering, the context and whole rectangles. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 8U != 0U)
		return X11_BAD_LENGTH;
	if (request[1] > GC_ORDERING_MAX)
		return X11_BAD_VALUE;
	gc = x11_gc_find(server, x11_read32(request + 4, msb));
	if (gc == NULL)
		return X11_BAD_GC;

	/* The rectangles' region (the client says they do not overlap; overlapping ones are joined). */
	x11_region_init(&clip);
	error = 0;
	for (offset = 12U; offset + 8U <= length && error == 0; offset += 8U) {
		box.x0 = (int16_t)x11_read16(request + offset, msb);
		box.y0 = (int16_t)x11_read16(request + offset + 2U, msb);
		box.x1 = box.x0 + (int)x11_read16(request + offset + 4U, msb);
		box.y1 = box.y0 + (int)x11_read16(request + offset + 6U, msb);
		if (box.x0 >= box.x1 || box.y0 >= box.y1)
			continue;
		error = x11_region_union_box(&clip, &box);
	}
	if (error != 0) {
		x11_region_fini(&clip);
		return X11_BAD_ALLOC;
	}

	/* The region charged to the context's owner. */
	added = gc_region_bytes(&clip);
	error = x11_memory_charge(server, gc->owner, added);
	if (error != 0) {
		x11_region_fini(&clip);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the region and the origin are the clip, and the charge follows them. */
	x11_region_fini(&gc->clip);
	gc->clip = clip;
	gc->clip_set = 1;
	gc->clip_x = (int16_t)x11_read16(request + 8, msb);
	gc->clip_y = (int16_t)x11_read16(request + 10, msb);
	gc_recharge(server, gc, added);
	return 0U;
}

/*
 * Gives a context's dash lengths and how many there are.
 */
const uint8_t *
x11_gc_dashes(
	const struct x11_gc *gc,
	size_t *count)
{
	/* The list SetDashes gave. */
	*count = gc->dash_count;
	if (gc->dash_list != NULL)
		return gc->dash_list;

	/* The one length ChangeGC gave. */
	return &gc->dash_one;
}

/*
 * Frees a graphics context that no id names: its tile, stipple and clip,
 * and what its owner is charged for it.
 */
void
x11_gc_free(
	struct x11server *server,
	struct x11_gc *gc)
{
	/* The copies and the dashes, and the context, with their charge. */
	x11_image_free(gc->tile);
	x11_image_free(gc->stipple);
	x11_region_fini(&gc->clip);
	free(gc->dash_list);
	x11_memory_release(server, gc->owner, gc->charged + sizeof(*gc));
	free(gc);
}

/*
 * Reads the values a mask names and checks each: BadValue for an
 * enumerated value out of range or zero dashes, BadPixmap for a tile,
 * stipple or clip mask that is no pixmap, BadMatch for one of the wrong
 * depth, BadFont for a font that is not open.  Returns 0, or the error.
 */
static unsigned
gc_read(
	struct x11server *server,
	const struct x11_gc *gc,
	int msb,
	const uint8_t *values,
	uint32_t mask,
	struct gc_values *read)
{
	struct x11_pixmap *pixmap;
	uint32_t value;
	unsigned bit;
	unsigned type;

	/* Each value in bit order. */
	memset(read, 0, sizeof(*read));
	read->mask = mask;
	for (bit = 0U; bit < GC_COUNT; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;
		value = x11_read32(values, msb);
		values += 4;
		read->value[bit] = value;

		/* Its check. */
		switch (bit) {
		case GC_FUNCTION:
			/* One of the sixteen functions. */
			if (value > GC_FUNCTION_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_LINE_STYLE:
			/* Solid, OnOffDash or DoubleDash. */
			if (value > GC_LINE_STYLE_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_CAP_STYLE:
			/* NotLast, Butt, Round or Projecting. */
			if (value > GC_CAP_STYLE_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_JOIN_STYLE:
			/* Miter, Round or Bevel. */
			if (value > GC_JOIN_STYLE_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_FILL_STYLE:
			/* Solid, Tiled, Stippled or OpaqueStippled. */
			if (value > GC_FILL_STYLE_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_FILL_RULE:
		case GC_SUBWINDOW_MODE:
		case GC_EXPOSURES:
		case GC_ARC_MODE:
			/* One of two. */
			if (value > GC_BOOLEAN_MAX)
				return X11_BAD_VALUE;
			break;
		case GC_TILE:
			/* A pixmap of the context's depth. */
			pixmap = x11_pixmap_find(server, value);
			if (pixmap == NULL)
				return X11_BAD_PIXMAP;
			if (pixmap->depth != gc->depth)
				return X11_BAD_MATCH;
			read->tile = pixmap;
			break;
		case GC_STIPPLE:
			/* A bitmap. */
			pixmap = x11_pixmap_find(server, value);
			if (pixmap == NULL)
				return X11_BAD_PIXMAP;
			if (pixmap->depth != 1U)
				return X11_BAD_MATCH;
			read->stipple = pixmap;
			break;
		case GC_FONT:
			/* An open font. */
			type = x11_resource_type(server, value);
			if (type != X11_RESOURCE_FONT)
				return X11_BAD_FONT;
			break;
		case GC_CLIP_MASK:
			/* None, or a bitmap. */
			if (value == GC_NONE)
				break;
			pixmap = x11_pixmap_find(server, value);
			if (pixmap == NULL)
				return X11_BAD_PIXMAP;
			if (pixmap->depth != 1U)
				return X11_BAD_MATCH;
			read->clip = pixmap;
			break;
		case GC_DASHES:
			/* A dash of no length is no dash. */
			if (value == 0U || value > 0xffU)
				return X11_BAD_VALUE;
			break;
		default:
			/* A pixel, a mask, a width, an origin or an offset: any value. */
			break;
		}
	}

	/* Succeeded: every value may be kept. */
	return 0U;
}

/*
 * Keeps values read and checked: the copies of the tile, stipple and clip
 * mask are made first, so that BadAlloc leaves the context as it was.
 * Returns 0, or BadAlloc.
 */
static unsigned
gc_apply(
	struct x11server *server,
	struct x11_gc *gc,
	const struct gc_values *values)
{
	struct x11_image *tile;
	struct x11_image *stipple;
	struct x11_region clip;
	size_t added;
	int error;

	/* The new copies. */
	tile = NULL;
	stipple = NULL;
	x11_region_init(&clip);
	added = 0U;
	if (values->tile != NULL) {
		error = x11_image_copy(&values->tile->image, &tile);
		if (error != 0)
			goto failed;
		added += x11_image_bytes(tile->width, tile->height);
	}
	if (values->stipple != NULL) {
		error = x11_image_copy(&values->stipple->image, &stipple);
		if (error != 0)
			goto failed;
		added += x11_image_bytes(stipple->width, stipple->height);
	}
	if (values->clip != NULL) {
		error = gc_clip_region(&values->clip->image, &clip);
		if (error != 0)
			goto failed;
		added += gc_region_bytes(&clip);
	}

	/* The owner is charged for them. */
	error = x11_memory_charge(server, gc->owner, added);
	if (error != 0)
		goto failed;

	/* The pixels, the function and the planes. */
	if ((values->mask & (1U << GC_FUNCTION)) != 0U)
		gc->function = values->value[GC_FUNCTION];
	if ((values->mask & (1U << GC_PLANE_MASK)) != 0U)
		gc->plane_mask = values->value[GC_PLANE_MASK];
	if ((values->mask & (1U << GC_FOREGROUND)) != 0U)
		gc->foreground = values->value[GC_FOREGROUND];
	if ((values->mask & (1U << GC_BACKGROUND)) != 0U)
		gc->background = values->value[GC_BACKGROUND];

	/* The lines and the fills. */
	if ((values->mask & (1U << GC_LINE_WIDTH)) != 0U)
		gc->line_width = (uint16_t)values->value[GC_LINE_WIDTH];
	if ((values->mask & (1U << GC_LINE_STYLE)) != 0U)
		gc->line_style = (uint8_t)values->value[GC_LINE_STYLE];
	if ((values->mask & (1U << GC_CAP_STYLE)) != 0U)
		gc->cap_style = (uint8_t)values->value[GC_CAP_STYLE];
	if ((values->mask & (1U << GC_JOIN_STYLE)) != 0U)
		gc->join_style = (uint8_t)values->value[GC_JOIN_STYLE];
	if ((values->mask & (1U << GC_FILL_STYLE)) != 0U)
		gc->fill_style = (uint8_t)values->value[GC_FILL_STYLE];
	if ((values->mask & (1U << GC_FILL_RULE)) != 0U)
		gc->fill_rule = (uint8_t)values->value[GC_FILL_RULE];
	if ((values->mask & (1U << GC_ARC_MODE)) != 0U)
		gc->arc_mode = (uint8_t)values->value[GC_ARC_MODE];
	if ((values->mask & (1U << GC_DASH_OFFSET)) != 0U)
		gc->dash_offset = (uint16_t)values->value[GC_DASH_OFFSET];
	if ((values->mask & (1U << GC_DASHES)) != 0U) {
		free(gc->dash_list);
		gc->dash_list = NULL;
		gc->dash_one = (uint8_t)values->value[GC_DASHES];
		gc->dash_count = 1U;
	}

	/* The tile and the stipple, their copies replacing the old ones, and their origin. */
	if (tile != NULL) {
		x11_image_free(gc->tile);
		gc->tile = tile;
	}
	if (stipple != NULL) {
		x11_image_free(gc->stipple);
		gc->stipple = stipple;
	}
	if ((values->mask & (1U << GC_TILE_X)) != 0U)
		gc->tile_x = (int16_t)values->value[GC_TILE_X];
	if ((values->mask & (1U << GC_TILE_Y)) != 0U)
		gc->tile_y = (int16_t)values->value[GC_TILE_Y];

	/* The font, the subwindow mode and the exposures. */
	if ((values->mask & (1U << GC_FONT)) != 0U)
		gc->font = values->value[GC_FONT];
	if ((values->mask & (1U << GC_SUBWINDOW_MODE)) != 0U)
		gc->subwindow_mode = (uint8_t)values->value[GC_SUBWINDOW_MODE];
	if ((values->mask & (1U << GC_EXPOSURES)) != 0U)
		gc->graphics_exposures = (uint8_t)values->value[GC_EXPOSURES];

	/* The clip's origin, and the clip mask's region (None: no clip). */
	if ((values->mask & (1U << GC_CLIP_X)) != 0U)
		gc->clip_x = (int16_t)values->value[GC_CLIP_X];
	if ((values->mask & (1U << GC_CLIP_Y)) != 0U)
		gc->clip_y = (int16_t)values->value[GC_CLIP_Y];
	if ((values->mask & (1U << GC_CLIP_MASK)) != 0U) {
		x11_region_fini(&gc->clip);
		gc->clip = clip;
		gc->clip_set = 0;
		if (values->clip != NULL)
			gc->clip_set = 1;
		x11_region_init(&clip);
	}

	/* The charge of what the context keeps now; the copies replaced are no longer charged. */
	gc_recharge(server, gc, added);

	/* Succeeded: the values are kept. */
	x11_region_fini(&clip);
	return 0U;

failed:
	/* No memory: the new copies go, and the context is as it was. */
	x11_image_free(tile);
	x11_image_free(stipple);
	x11_region_fini(&clip);
	return X11_BAD_ALLOC;
}

/*
 * Makes the region of a bitmap's set pixels, row by row: each run of set
 * pixels in a row is one box.  Returns 0, or ENOMEM.
 */
static int
gc_clip_region(
	const struct x11_image *bitmap,
	struct x11_region *region)
{
	struct x11_box box;
	uint32_t bit;
	int row;
	int column;
	int start;
	int error;

	/* Each row's runs. */
	for (row = 0; row < bitmap->height; row++) {
		start = -1;
		for (column = 0; column <= bitmap->width; column++) {
			/* Past the last column, a run ends. */
			bit = 0U;
			if (column < bitmap->width)
				bit = bitmap->pixels[(size_t)row * bitmap->stride + (size_t)column] & 1U;

			/* A run starts at a set pixel after a clear one. */
			if (bit != 0U) {
				if (start < 0)
					start = column;
				continue;
			}

			/* A run ends at a clear pixel: one box (the rows' runs never overlap). */
			if (start < 0)
				continue;
			box.x0 = start;
			box.y0 = row;
			box.x1 = column;
			box.y1 = row + 1;
			error = x11_region_append_box(region, &box);
			if (error != 0)
				return error;
			start = -1;
		}
	}

	/* Succeeded: the region. */
	return 0;
}

/* Gives a new context the protocol's default values. */
static void
gc_defaults(
	struct x11_gc *gc)
{
	/* Copy into every plane, a foreground of 0 and a background of 1. */
	gc->function = X11_FUNCTION_COPY;
	gc->plane_mask = 0xffffffffU;
	gc->foreground = 0U;
	gc->background = 1U;

	/* Thin solid lines with butt caps and mitred joins, solid even-odd fills, pie-slice arcs. */
	gc->line_width = 0U;
	gc->line_style = 0U;
	gc->cap_style = 1U;
	gc->join_style = 0U;
	gc->fill_style = X11_FILL_SOLID;
	gc->fill_rule = 0U;
	gc->arc_mode = 1U;

	/*
	 * No tile or stipple copied: a missing tile is the foreground and a
	 * missing stipple is all set, as the protocol's defaults are.
	 */
	gc->tile = NULL;
	gc->stipple = NULL;
	gc->tile_x = 0;
	gc->tile_y = 0;

	/* Clipped by children, with graphics exposures, no clip mask, dashes of four. */
	gc->subwindow_mode = X11_CLIP_BY_CHILDREN;
	gc->graphics_exposures = 1U;
	gc->clip_x = 0;
	gc->clip_y = 0;
	gc->clip_set = 0;
	x11_region_init(&gc->clip);
	gc->dash_offset = 0U;
	gc->dash_one = GC_DEFAULT_DASHES;
	gc->dash_list = NULL;
	gc->dash_count = 1U;
	gc->charged = 0U;
}

/* Gives the bytes a region's boxes are charged as. */
static size_t
gc_region_bytes(
	const struct x11_region *region)
{
	/* The boxes it holds. */
	return region->count * sizeof(struct x11_box);
}

/* Gives the bytes a context's copies and dash list are charged as. */
static size_t
gc_kept_bytes(
	const struct x11_gc *gc)
{
	size_t bytes;

	/* The tile, the stipple, the clip and the list of dashes. */
	bytes = 0U;
	if (gc->tile != NULL)
		bytes += x11_image_bytes(gc->tile->width, gc->tile->height);
	if (gc->stipple != NULL)
		bytes += x11_image_bytes(gc->stipple->width, gc->stipple->height);
	bytes += gc_region_bytes(&gc->clip);
	if (gc->dash_list != NULL)
		bytes += gc->dash_count;

	/* The bytes. */
	return bytes;
}

/*
 * Charges a context's owner for what the context keeps now: the bytes
 * just charged (added) and those charged before stand, less what is no
 * longer kept.
 */
static void
gc_recharge(
	struct x11server *server,
	struct x11_gc *gc,
	size_t added)
{
	size_t kept;

	/* What was charged and is not kept any more is released. */
	kept = gc_kept_bytes(gc);
	x11_memory_release(server, gc->owner, gc->charged + added - kept);
	gc->charged = kept;
}

/*
 * Copies the values a mask names from one context to another: new copies
 * of the tile, stipple, clip and dash list are made first, so BadAlloc
 * leaves the target as it was.  Returns 0, or BadAlloc.
 */
static unsigned
gc_copy(
	struct x11server *server,
	struct x11_gc *target,
	const struct x11_gc *source,
	uint32_t mask)
{
	struct x11_image *tile;
	struct x11_image *stipple;
	struct x11_region clip;
	uint8_t *dashes;
	size_t added;
	int error;

	/* The new copies. */
	tile = NULL;
	stipple = NULL;
	dashes = NULL;
	x11_region_init(&clip);
	added = 0U;
	if ((mask & (1U << GC_TILE)) != 0U && source->tile != NULL) {
		error = x11_image_copy(source->tile, &tile);
		if (error != 0)
			goto failed;
		added += x11_image_bytes(tile->width, tile->height);
	}
	if ((mask & (1U << GC_STIPPLE)) != 0U && source->stipple != NULL) {
		error = x11_image_copy(source->stipple, &stipple);
		if (error != 0)
			goto failed;
		added += x11_image_bytes(stipple->width, stipple->height);
	}
	if ((mask & (1U << GC_CLIP_MASK)) != 0U) {
		error = x11_region_copy(&clip, &source->clip);
		if (error != 0)
			goto failed;
		added += gc_region_bytes(&clip);
	}
	if ((mask & (1U << GC_DASHES)) != 0U && source->dash_list != NULL) {
		dashes = malloc(source->dash_count);
		if (dashes == NULL)
			goto failed;
		memcpy(dashes, source->dash_list, source->dash_count);
		added += source->dash_count;
	}

	/* The target's owner is charged for them. */
	error = x11_memory_charge(server, target->owner, added);
	if (error != 0)
		goto failed;

	/* The pixels, the function and the planes. */
	if ((mask & (1U << GC_FUNCTION)) != 0U)
		target->function = source->function;
	if ((mask & (1U << GC_PLANE_MASK)) != 0U)
		target->plane_mask = source->plane_mask;
	if ((mask & (1U << GC_FOREGROUND)) != 0U)
		target->foreground = source->foreground;
	if ((mask & (1U << GC_BACKGROUND)) != 0U)
		target->background = source->background;

	/* The lines and the fills. */
	if ((mask & (1U << GC_LINE_WIDTH)) != 0U)
		target->line_width = source->line_width;
	if ((mask & (1U << GC_LINE_STYLE)) != 0U)
		target->line_style = source->line_style;
	if ((mask & (1U << GC_CAP_STYLE)) != 0U)
		target->cap_style = source->cap_style;
	if ((mask & (1U << GC_JOIN_STYLE)) != 0U)
		target->join_style = source->join_style;
	if ((mask & (1U << GC_FILL_STYLE)) != 0U)
		target->fill_style = source->fill_style;
	if ((mask & (1U << GC_FILL_RULE)) != 0U)
		target->fill_rule = source->fill_rule;
	if ((mask & (1U << GC_ARC_MODE)) != 0U)
		target->arc_mode = source->arc_mode;

	/* The tile and the stipple (a missing one stays missing) and their origin. */
	if ((mask & (1U << GC_TILE)) != 0U) {
		x11_image_free(target->tile);
		target->tile = tile;
		tile = NULL;
	}
	if ((mask & (1U << GC_STIPPLE)) != 0U) {
		x11_image_free(target->stipple);
		target->stipple = stipple;
		stipple = NULL;
	}
	if ((mask & (1U << GC_TILE_X)) != 0U)
		target->tile_x = source->tile_x;
	if ((mask & (1U << GC_TILE_Y)) != 0U)
		target->tile_y = source->tile_y;

	/* The font, the subwindow mode and the exposures. */
	if ((mask & (1U << GC_FONT)) != 0U)
		target->font = source->font;
	if ((mask & (1U << GC_SUBWINDOW_MODE)) != 0U)
		target->subwindow_mode = source->subwindow_mode;
	if ((mask & (1U << GC_EXPOSURES)) != 0U)
		target->graphics_exposures = source->graphics_exposures;

	/* The clip's origin and the clip. */
	if ((mask & (1U << GC_CLIP_X)) != 0U)
		target->clip_x = source->clip_x;
	if ((mask & (1U << GC_CLIP_Y)) != 0U)
		target->clip_y = source->clip_y;
	if ((mask & (1U << GC_CLIP_MASK)) != 0U) {
		x11_region_fini(&target->clip);
		target->clip = clip;
		target->clip_set = source->clip_set;
		x11_region_init(&clip);
	}

	/* The dashes. */
	if ((mask & (1U << GC_DASH_OFFSET)) != 0U)
		target->dash_offset = source->dash_offset;
	if ((mask & (1U << GC_DASHES)) != 0U) {
		free(target->dash_list);
		target->dash_list = dashes;
		target->dash_one = source->dash_one;
		target->dash_count = source->dash_count;
		dashes = NULL;
	}

	/* Succeeded: copied, and the charge follows what the target keeps. */
	gc_recharge(server, target, added);
	return 0U;

failed:
	/* No memory: the new copies go, and the target is as it was. */
	x11_image_free(tile);
	x11_image_free(stipple);
	x11_region_fini(&clip);
	free(dashes);
	return X11_BAD_ALLOC;
}
