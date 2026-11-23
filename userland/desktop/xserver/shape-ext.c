/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SHAPE extension (version 1.1): windows that are not rectangles.
 *
 * A window may have three regions, each relative to its origin (inside
 * its border): the bounding region, outside which neither the window nor
 * its border is there; the clip region, outside which the inside of the
 * window is not drawn; and the input region, outside which the pointer
 * passes through it.  A window without one has the default: the window
 * with its border for the bounding and the input region, the window's
 * inside for the clip region.  What a window shows is its region cut to
 * the default (the effective region), so a region kept outlives a resize.
 *
 * The regions are kept as a client set them (in X's banded form, which
 * GetRectangles gives back), and the effective regions are worked out
 * when the window's size or its regions change.  draw.c clips drawing to
 * them, window.c finds the pointer's window with them, and rootless.c
 * gives a top-level window's bounding region to the backend, which clips
 * the system's window to it (Windows' SetWindowRgn).  A client that
 * selected ShapeNotify on a window hears of each change.
 *
 * Wire layouts follow the public SHAPE extension protocol (version 1.1);
 * where the protocol leaves a choice, the answers are those of the X.Org
 * server, so that a client sees the same as with Xvfb.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The requests' minor opcodes. */
#define SHAPE_QUERY_VERSION	0U
#define SHAPE_RECTANGLES	1U
#define SHAPE_MASK		2U
#define SHAPE_COMBINE		3U
#define SHAPE_OFFSET		4U
#define SHAPE_QUERY_EXTENTS	5U
#define SHAPE_SELECT_INPUT	6U
#define SHAPE_INPUT_SELECTED	7U
#define SHAPE_GET_RECTANGLES	8U

/* The version the extension speaks. */
#define SHAPE_VERSION_MAJOR	1U
#define SHAPE_VERSION_MINOR	1U

/* The operations: the region becomes, is joined by, cut to, cut by, or the inverse within the source. */
#define SHAPE_SET		0U
#define SHAPE_UNION		1U
#define SHAPE_INTERSECT		2U
#define SHAPE_SUBTRACT		3U
#define SHAPE_INVERT		4U

/* The orders a client says its rectangles come in. */
#define SHAPE_UNSORTED		0U
#define SHAPE_Y_SORTED		1U
#define SHAPE_YX_SORTED		2U
#define SHAPE_YX_BANDED		3U

/* The kinds of region. */
#define SHAPE_KINDS		3U

/* The most rectangles a region of a bitmap is made of before it is refused. */
#define SHAPE_BITMAP_BOXES_MAX	((size_t)1U << 20)

/*
 * A window's shapes: for each kind whether a client set a region and the
 * region (from the window's origin, in X's banded form); the effective
 * regions worked out for a size and border (outside: the bounding region
 * cut to the window with its border; inside: that cut to the clip region
 * and the window's inside; input: the input region cut to the outside),
 * valid while the window keeps the size they were worked out for; and
 * the clients that asked to hear of changes (a slot each).
 */
struct x11_shape {
	int set[SHAPE_KINDS];
	struct x11_region region[SHAPE_KINDS];
	int valid;
	uint16_t valid_width;
	uint16_t valid_height;
	uint16_t valid_border;
	struct x11_region outside;
	struct x11_region inside;
	struct x11_region input;
	unsigned *selects;
	unsigned select_count;
	unsigned select_capacity;
};

static unsigned shape_query_version(struct x11_ext_request *request);
static unsigned shape_rectangles(struct x11_ext_request *request);
static unsigned shape_mask(struct x11_ext_request *request);
static unsigned shape_combine(struct x11_ext_request *request);
static unsigned shape_offset(struct x11_ext_request *request);
static unsigned shape_query_extents(struct x11_ext_request *request);
static unsigned shape_select_input(struct x11_ext_request *request);
static unsigned shape_input_selected(struct x11_ext_request *request);
static unsigned shape_get_rectangles(struct x11_ext_request *request);
static unsigned shape_find_window(struct x11_ext_request *request, uint32_t id, struct x11_window **window);
static int shape_order_kept(const uint8_t *rects, size_t count, unsigned ordering, int msb);
static unsigned shape_operate(struct x11server *server, struct x11_window *window, unsigned kind, unsigned operation, struct x11_region *source, int x_offset, int y_offset);
static struct x11_shape *shape_of(struct x11_window *window);
static void shape_default(const struct x11_window *window, unsigned kind, struct x11_box *box);
static int shape_effective(const struct x11_window *window);
static void shape_extents(const struct x11_window *window, unsigned kind, struct x11_box *extents, int *shaped);
static void shape_changed(struct x11server *server, struct x11_window *window, unsigned kind);
static void shape_notify(struct x11server *server, struct x11_window *window, unsigned kind);
static int shape_bitmap(const struct x11_image *image, struct x11_region *region);
static int shape_point_in(const struct x11_region *region, int x, int y);
static void shape_free(struct x11_shape *shape);

/*
 * Answers a SHAPE request by its minor opcode.  Returns 0, or the error
 * to send (request->bad names the value that was wrong).
 */
unsigned
x11_shape_request(
	struct x11_ext_request *request)
{
	unsigned code;

	/* The request of the minor opcode. */
	switch (request->bytes[1]) {
	case SHAPE_QUERY_VERSION:
		code = shape_query_version(request);
		break;
	case SHAPE_RECTANGLES:
		code = shape_rectangles(request);
		break;
	case SHAPE_MASK:
		code = shape_mask(request);
		break;
	case SHAPE_COMBINE:
		code = shape_combine(request);
		break;
	case SHAPE_OFFSET:
		code = shape_offset(request);
		break;
	case SHAPE_QUERY_EXTENTS:
		code = shape_query_extents(request);
		break;
	case SHAPE_SELECT_INPUT:
		code = shape_select_input(request);
		break;
	case SHAPE_INPUT_SELECTED:
		code = shape_input_selected(request);
		break;
	case SHAPE_GET_RECTANGLES:
		code = shape_get_rectangles(request);
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Forgets a closed client's wish to hear of shapes on every window.
 */
void
x11_shape_forget_client(
	struct x11server *server,
	unsigned index)
{
	struct x11_window *window;
	struct x11_shape *shape;
	unsigned slot;

	/* Before the root window is there, or after it is gone. */
	if (server->root == NULL)
		return;

	/* Each window that has a shape's state. */
	for (window = server->root;
	     window != NULL;
	     window = x11_window_next(window, server->root)) {
		shape = window->shape;
		if (shape == NULL)
			continue;

		/* The client's slot in the list, the last taking its place. */
		for (slot = 0U; slot < shape->select_count; slot++) {
			if (shape->selects[slot] != index)
				continue;
			shape->selects[slot] = shape->selects[shape->select_count - 1U];
			shape->select_count--;
			break;
		}
	}
}

/*
 * Frees a window's shapes when the window goes.
 */
void
x11_shape_forget_window(
	struct x11server *server,
	struct x11_window *window)
{
	(void)server;

	/* Nothing kept. */
	if (window->shape == NULL)
		return;

	/* Succeeded: freed. */
	shape_free(window->shape);
	window->shape = NULL;
}

/*
 * Tells whether a window has a region of its own of a kind that clips
 * what it shows (bounding or clip).
 */
int
x11_shape_has(
	const struct x11_window *window)
{
	/* No shape's state. */
	if (window->shape == NULL)
		return 0;

	/* A bounding or a clip region. */
	if (window->shape->set[X11_SHAPE_BOUNDING])
		return 1;
	if (window->shape->set[X11_SHAPE_CLIP])
		return 1;

	/* Only events asked for, or an input region. */
	return 0;
}

/*
 * Tells whether a place on the root window, inside a window's rectangle,
 * takes the pointer for the window: inside its effective bounding and
 * input regions.  A window without regions takes all of its rectangle.
 */
int
x11_shape_contains(
	const struct x11_window *window,
	int x,
	int y)
{
	int inside;
	int error;

	/* Unshaped: all of it. */
	if (window->shape == NULL)
		return 1;
	if (!window->shape->set[X11_SHAPE_BOUNDING] && !window->shape->set[X11_SHAPE_INPUT])
		return 1;

	/* The effective regions; without memory for them, all of it. */
	error = shape_effective(window);
	if (error != 0)
		return 1;

	/* Succeeded: inside the input region (which is inside the bounding one). */
	inside = shape_point_in(&window->shape->input, x - window->x, y - window->y);
	return inside;
}

/*
 * Cuts a window's own clip (on the root window) to its effective regions:
 * the bounding region with its border, else the bounding and clip regions
 * of its inside.  A window without regions leaves it as it is.  Returns
 * 0, or ENOMEM.
 */
int
x11_shape_clip_window(
	const struct x11_window *window,
	int border,
	struct x11_region *clip)
{
	struct x11_region shaped;
	int shaped_window;
	int error;

	/* Unshaped. */
	shaped_window = x11_shape_has(window);
	if (!shaped_window)
		return 0;

	/* The effective region of the part asked for. */
	error = shape_effective(window);
	if (error != 0)
		return error;
	x11_region_init(&shaped);
	if (border) {
		error = x11_region_copy(&shaped, &window->shape->outside);
	} else {
		error = x11_region_copy(&shaped, &window->shape->inside);
	}
	if (error != 0) {
		x11_region_fini(&shaped);
		return error;
	}

	/* On the root window, the clip cut to it. */
	x11_region_translate(&shaped, window->x, window->y);
	error = x11_region_intersect(clip, &shaped);
	x11_region_fini(&shaped);
	if (error != 0)
		return error;

	/* Succeeded: cut. */
	return 0;
}

/*
 * Cuts a clip (on the root window) to the inside of an ancestor's
 * effective regions (its children are not seen outside them).  Returns 0,
 * or ENOMEM.
 */
int
x11_shape_clip_inside(
	const struct x11_window *window,
	struct x11_region *clip)
{
	/* Succeeded: the same cut as the window's own inside. */
	return x11_shape_clip_window(window, 0, clip);
}

/*
 * Takes what a window covers (its effective bounding region, or its
 * rectangle with its border) out of a clip on the root window.  Returns
 * 0, or ENOMEM.
 */
int
x11_shape_subtract_outside(
	const struct x11_window *window,
	struct x11_region *clip)
{
	struct x11_region shaped;
	struct x11_box box;
	int error;

	/* Unshaped: the rectangle with its border. */
	if (window->shape == NULL || !window->shape->set[X11_SHAPE_BOUNDING]) {
		box.x0 = window->x - (int)window->border;
		box.y0 = window->y - (int)window->border;
		box.x1 = window->x + (int)window->width + (int)window->border;
		box.y1 = window->y + (int)window->height + (int)window->border;
		error = x11_region_subtract_box(clip, &box);
		if (error != 0)
			return error;
		return 0;
	}

	/* The effective bounding region on the root window. */
	error = shape_effective(window);
	if (error != 0)
		return error;
	x11_region_init(&shaped);
	error = x11_region_copy(&shaped, &window->shape->outside);
	if (error != 0) {
		x11_region_fini(&shaped);
		return error;
	}
	x11_region_translate(&shaped, window->x, window->y);

	/* Succeeded: taken out. */
	error = x11_region_subtract(clip, &shaped);
	x11_region_fini(&shaped);
	if (error != 0)
		return error;
	return 0;
}

/*
 * Gives a window's region of a kind as a client reads it (from the
 * window's origin): the region set, else the default.  Returns 0, or
 * ENOMEM.
 */
int
x11_shape_region(
	const struct x11_window *window,
	unsigned kind,
	struct x11_region *region)
{
	struct x11_box box;
	int error;

	/* A region set. */
	if (window->shape != NULL && window->shape->set[kind]) {
		error = x11_region_copy(region, &window->shape->region[kind]);
		if (error != 0)
			return error;
		return 0;
	}

	/* Succeeded: the default. */
	shape_default(window, kind, &box);
	error = x11_region_set(region, &box);
	if (error != 0)
		return error;
	return 0;
}

/*
 * Sets a window's region of a kind (from the window's origin; NULL: none,
 * the default), as XFIXES's SetWindowShapeRegion does, and tells those
 * who asked.  The region given is the caller's still.  Returns 0, or
 * BadAlloc.
 */
unsigned
x11_shape_set(
	struct x11server *server,
	struct x11_window *window,
	unsigned kind,
	struct x11_region *region)
{
	struct x11_region copy;
	unsigned code;
	int error;

	/* None: the region set goes. */
	if (region == NULL) {
		code = shape_operate(server, window, kind, SHAPE_SET, NULL, 0, 0);
		return code;
	}

	/* A copy the shape keeps. */
	x11_region_init(&copy);
	error = x11_region_copy(&copy, region);
	if (error != 0) {
		x11_region_fini(&copy);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: set. */
	code = shape_operate(server, window, kind, SHAPE_SET, &copy, 0, 0);
	x11_region_fini(&copy);
	return code;
}

/*
 * Gives a top-level window's effective bounding region on its pixels
 * (from its corner, cut to its size) as rectangles for its surface, in a
 * malloc'd array the caller frees.  *rects is NULL when the window has no
 * bounding region (all of it is shown).  Returns 0, or ENOMEM.
 */
int
x11_shape_surface(
	const struct x11_window *window,
	struct x11_rect **rects,
	unsigned *count)
{
	const struct x11_region *outside;
	struct x11_box box;
	struct x11_box cut;
	struct x11_rect *list;
	size_t index;
	unsigned made;
	int meets;
	int error;

	/* No bounding region. */
	*rects = NULL;
	*count = 0U;
	if (window->shape == NULL || !window->shape->set[X11_SHAPE_BOUNDING])
		return 0;

	/* The effective region. */
	error = shape_effective(window);
	if (error != 0)
		return error;
	outside = &window->shape->outside;

	/* Room for each box (one more, so that an empty region is an array of none). */
	list = malloc((outside->count + 1U) * sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* Each box cut to the window's pixels. */
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = (int)window->width;
	box.y1 = (int)window->height;
	made = 0U;
	for (index = 0U; index < outside->count; index++) {
		meets = x11_box_intersect(&outside->boxes[index], &box, &cut);
		if (!meets)
			continue;
		list[made].x = cut.x0;
		list[made].y = cut.y0;
		list[made].width = (unsigned)(cut.x1 - cut.x0);
		list[made].height = (unsigned)(cut.y1 - cut.y0);
		made++;
	}

	/* Succeeded: the rectangles. */
	*rects = list;
	*count = made;
	return 0;
}

/* QueryVersion: 1.1. */
static unsigned
shape_query_version(
	struct x11_ext_request *request)
{
	uint8_t reply[32];

	/* The header alone. */
	if (request->length != 4U)
		return X11_BAD_LENGTH;

	/* Succeeded: the version. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, SHAPE_VERSION_MAJOR, request->msb);
	x11_write16(reply + 10, SHAPE_VERSION_MINOR, request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/*
 * Rectangles: an operation of a list of rectangles (in the order the
 * client says they come in) on a window's region of a kind.
 */
static unsigned
shape_rectangles(
	struct x11_ext_request *request)
{
	struct x11_region source;
	struct x11_window *window;
	struct x11_box box;
	const uint8_t *rect;
	size_t count;
	size_t index;
	unsigned operation;
	unsigned kind;
	unsigned ordering;
	unsigned code;
	unsigned width;
	unsigned height;
	int kept;
	int error;

	/* The fixed part, and whole rectangles after it. */
	if (request->length < 16U)
		return X11_BAD_LENGTH;

	/* The window. */
	code = shape_find_window(request, x11_read32(request->bytes + 8, request->msb), &window);
	if (code != 0U)
		return code;

	/* The kind, and the order the rectangles come in. */
	operation = request->bytes[4];
	kind = request->bytes[5];
	ordering = request->bytes[6];
	if (kind >= SHAPE_KINDS) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}
	if (ordering > SHAPE_YX_BANDED) {
		request->bad = ordering;
		return X11_BAD_VALUE;
	}
	if (((request->length - 16U) & 4U) != 0U)
		return X11_BAD_LENGTH;
	count = (request->length - 16U) / 8U;

	/* Rectangles out of the order said are refused. */
	kept = shape_order_kept(request->bytes + 16, count, ordering, request->msb);
	if (!kept)
		return X11_BAD_MATCH;

	/* The region of the rectangles (empty ones are nothing). */
	x11_region_init(&source);
	for (index = 0U; index < count; index++) {
		rect = request->bytes + 16U + index * 8U;
		width = x11_read16(rect + 4, request->msb);
		height = x11_read16(rect + 6, request->msb);
		if (width == 0U || height == 0U)
			continue;
		box.x0 = (int16_t)x11_read16(rect, request->msb);
		box.y0 = (int16_t)x11_read16(rect + 2, request->msb);
		box.x1 = box.x0 + (int)width;
		box.y1 = box.y0 + (int)height;
		error = x11_region_union_box(&source, &box);
		if (error != 0) {
			x11_region_fini(&source);
			return X11_BAD_ALLOC;
		}
	}

	/* Succeeded: the operation. */
	code = shape_operate(request->server, window, kind, operation, &source, (int16_t)x11_read16(request->bytes + 12, request->msb), (int16_t)x11_read16(request->bytes + 14, request->msb));
	x11_region_fini(&source);
	if (code == X11_BAD_VALUE)
		request->bad = operation;
	return code;
}

/*
 * Mask: an operation of a bitmap's set pixels (None: the region set goes)
 * on a window's region of a kind.
 */
static unsigned
shape_mask(
	struct x11_ext_request *request)
{
	struct x11_region source;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	uint32_t pixmap_id;
	unsigned operation;
	unsigned kind;
	unsigned code;
	int error;

	/* The request's fixed size, the window and the kind. */
	if (request->length != 20U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 8, request->msb), &window);
	if (code != 0U)
		return code;
	operation = request->bytes[4];
	kind = request->bytes[5];
	if (kind >= SHAPE_KINDS) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}

	/* None: the region goes, whatever the operation. */
	pixmap_id = x11_read32(request->bytes + 16, request->msb);
	if (pixmap_id == 0U) {
		code = shape_operate(request->server, window, kind, operation, NULL, 0, 0);
		return code;
	}

	/* A bitmap. */
	pixmap = x11_pixmap_find(request->server, pixmap_id);
	if (pixmap == NULL) {
		request->bad = pixmap_id;
		return X11_BAD_PIXMAP;
	}
	if (pixmap->depth != 1U)
		return X11_BAD_MATCH;

	/* The region of its set pixels. */
	x11_region_init(&source);
	error = shape_bitmap(&pixmap->image, &source);
	if (error != 0) {
		x11_region_fini(&source);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the operation. */
	code = shape_operate(request->server, window, kind, operation, &source, (int16_t)x11_read16(request->bytes + 12, request->msb), (int16_t)x11_read16(request->bytes + 14, request->msb));
	x11_region_fini(&source);
	if (code == X11_BAD_VALUE)
		request->bad = operation;
	return code;
}

/*
 * Combine: an operation of another window's region of a kind (as it is
 * kept, or its default) on a window's region of a kind.
 */
static unsigned
shape_combine(
	struct x11_ext_request *request)
{
	struct x11_region source;
	struct x11_window *window;
	struct x11_window *from;
	unsigned operation;
	unsigned kind;
	unsigned source_kind;
	unsigned code;
	int error;

	/* The request's fixed size, the window and the kind. */
	if (request->length != 20U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 8, request->msb), &window);
	if (code != 0U)
		return code;
	operation = request->bytes[4];
	kind = request->bytes[5];
	if (kind >= SHAPE_KINDS) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}

	/* The source window and its kind. */
	code = shape_find_window(request, x11_read32(request->bytes + 16, request->msb), &from);
	if (code != 0U)
		return code;
	source_kind = request->bytes[6];
	if (source_kind >= SHAPE_KINDS) {
		request->bad = source_kind;
		return X11_BAD_VALUE;
	}

	/* The source's region. */
	x11_region_init(&source);
	error = x11_shape_region(from, source_kind, &source);
	if (error != 0) {
		x11_region_fini(&source);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the operation. */
	code = shape_operate(request->server, window, kind, operation, &source, (int16_t)x11_read16(request->bytes + 12, request->msb), (int16_t)x11_read16(request->bytes + 14, request->msb));
	x11_region_fini(&source);
	if (code == X11_BAD_VALUE)
		request->bad = operation;
	return code;
}

/*
 * Offset: a window's region of a kind moves (a window without one keeps
 * the default); those who asked hear of it either way.
 */
static unsigned
shape_offset(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	struct x11_shape *shape;
	unsigned kind;
	unsigned code;

	/* The request's fixed size, the window and the kind. */
	if (request->length != 16U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 8, request->msb), &window);
	if (code != 0U)
		return code;
	kind = request->bytes[4];
	if (kind >= SHAPE_KINDS) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}

	/* A region set moves, with what it shows. */
	shape = window->shape;
	if (shape != NULL && shape->set[kind]) {
		x11_draw_begin(request->server, window, NULL);
		x11_region_translate(&shape->region[kind], (int16_t)x11_read16(request->bytes + 12, request->msb), (int16_t)x11_read16(request->bytes + 14, request->msb));
		shape->valid = 0;
		x11_draw_end(request->server);
		shape_changed(request->server, window, kind);
	}

	/* Succeeded: those who asked are told. */
	shape_notify(request->server, window, kind);
	return 0U;
}

/* QueryExtents: whether the bounding and clip regions are set, and the box around each. */
static unsigned
shape_query_extents(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	struct x11_box bounding;
	struct x11_box clip;
	uint8_t reply[32];
	unsigned code;
	int bounding_shaped;
	int clip_shaped;

	/* The window. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 4, request->msb), &window);
	if (code != 0U)
		return code;

	/* The boxes. */
	shape_extents(window, X11_SHAPE_BOUNDING, &bounding, &bounding_shaped);
	shape_extents(window, X11_SHAPE_CLIP, &clip, &clip_shaped);

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[8] = (uint8_t)bounding_shaped;
	reply[9] = (uint8_t)clip_shaped;
	x11_write16(reply + 12, (uint16_t)bounding.x0, request->msb);
	x11_write16(reply + 14, (uint16_t)bounding.y0, request->msb);
	x11_write16(reply + 16, (uint16_t)(bounding.x1 - bounding.x0), request->msb);
	x11_write16(reply + 18, (uint16_t)(bounding.y1 - bounding.y0), request->msb);
	x11_write16(reply + 20, (uint16_t)clip.x0, request->msb);
	x11_write16(reply + 22, (uint16_t)clip.y0, request->msb);
	x11_write16(reply + 24, (uint16_t)(clip.x1 - clip.x0), request->msb);
	x11_write16(reply + 26, (uint16_t)(clip.y1 - clip.y0), request->msb);
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/* SelectInput: the client hears of the window's shapes, or no longer. */
static unsigned
shape_select_input(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	struct x11_shape *shape;
	unsigned *grown;
	unsigned capacity;
	unsigned slot;
	unsigned code;
	uint8_t enable;

	/* The window, and true or false. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 4, request->msb), &window);
	if (code != 0U)
		return code;
	enable = request->bytes[8];
	if (enable > 1U) {
		request->bad = enable;
		return X11_BAD_VALUE;
	}

	/* The client's slot in the list, if it is there. */
	shape = window->shape;
	slot = 0U;
	if (shape != NULL) {
		for (slot = 0U; slot < shape->select_count; slot++) {
			if (shape->selects[slot] == request->index)
				break;
		}
	}

	/* No longer: out of the list. */
	if (!enable) {
		if (shape == NULL || slot == shape->select_count)
			return 0U;
		shape->selects[slot] = shape->selects[shape->select_count - 1U];
		shape->select_count--;
		return 0U;
	}

	/* Already in it. */
	if (shape != NULL && slot < shape->select_count)
		return 0U;

	/* Into the list, grown when full. */
	shape = shape_of(window);
	if (shape == NULL)
		return X11_BAD_ALLOC;
	if (shape->select_count == shape->select_capacity) {
		capacity = shape->select_capacity * 2U;
		if (capacity == 0U)
			capacity = 4U;
		grown = realloc(shape->selects, capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		shape->selects = grown;
		shape->select_capacity = capacity;
	}

	/* Succeeded: the client hears of changes. */
	shape->selects[shape->select_count] = request->index;
	shape->select_count++;
	return 0U;
}

/* InputSelected: whether the client hears of the window's shapes. */
static unsigned
shape_input_selected(
	struct x11_ext_request *request)
{
	struct x11_window *window;
	uint8_t reply[32];
	unsigned slot;
	unsigned code;
	int enabled;

	/* The window. */
	if (request->length != 8U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 4, request->msb), &window);
	if (code != 0U)
		return code;

	/* The client in the list. */
	enabled = 0;
	if (window->shape != NULL) {
		for (slot = 0U; slot < window->shape->select_count; slot++) {
			if (window->shape->selects[slot] == request->index)
				enabled = 1;
		}
	}

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)enabled;
	x11_reply(request->server, request->client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetRectangles: a window's region of a kind as rectangles in X's banded
 * order (the default as one rectangle).
 */
static unsigned
shape_get_rectangles(
	struct x11_ext_request *request)
{
	struct x11_region region;
	struct x11_window *window;
	uint8_t *reply;
	uint8_t *rect;
	size_t size;
	size_t index;
	unsigned kind;
	unsigned code;
	int unshaped;
	int error;

	/* The window and the kind. */
	if (request->length != 12U)
		return X11_BAD_LENGTH;
	code = shape_find_window(request, x11_read32(request->bytes + 4, request->msb), &window);
	if (code != 0U)
		return code;
	kind = request->bytes[8];
	if (kind >= SHAPE_KINDS) {
		request->bad = kind;
		return X11_BAD_VALUE;
	}

	/* The region, as kept or the default. */
	x11_region_init(&region);
	error = x11_shape_region(window, kind, &region);
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/*
	 * The X.Org server gives the default bounding and input rectangle the
	 * width and height with one border, not two (QueryExtents gives two);
	 * clients see the same here.
	 */
	unshaped = 1;
	if (window->shape != NULL && window->shape->set[kind])
		unshaped = 0;
	if (unshaped && kind != X11_SHAPE_CLIP && region.count == 1U) {
		region.boxes[0].x1 -= (int)window->border;
		region.boxes[0].y1 -= (int)window->border;
	}

	/* The reply, room for every rectangle. */
	size = 32U + region.count * 8U;
	reply = calloc(1U, size);
	if (reply == NULL) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}

	/* The rectangles, each its corner and size. */
	reply[1] = SHAPE_YX_BANDED;
	x11_write32(reply + 8, (uint32_t)region.count, request->msb);
	for (index = 0U; index < region.count; index++) {
		rect = reply + 32U + index * 8U;
		x11_write16(rect, (uint16_t)region.boxes[index].x0, request->msb);
		x11_write16(rect + 2, (uint16_t)region.boxes[index].y0, request->msb);
		x11_write16(rect + 4, (uint16_t)(region.boxes[index].x1 - region.boxes[index].x0), request->msb);
		x11_write16(rect + 6, (uint16_t)(region.boxes[index].y1 - region.boxes[index].y0), request->msb);
	}

	/* Succeeded: sent. */
	x11_reply(request->server, request->client, reply, size);
	free(reply);
	x11_region_fini(&region);
	return 0U;
}

/* Finds the window a request names.  Returns 0, or BadWindow (naming the id). */
static unsigned
shape_find_window(
	struct x11_ext_request *request,
	uint32_t id,
	struct x11_window **window)
{
	/* The window. */
	*window = x11_window_find(request->server, id);
	if (*window == NULL) {
		request->bad = id;
		return X11_BAD_WINDOW;
	}

	/* Succeeded: found. */
	return 0U;
}

/*
 * Tells whether a client's rectangles come in the order it said (the
 * X.Org server's checks): each by y, by y then x, or in bands of the same
 * y and height whose rectangles do not overlap.
 */
static int
shape_order_kept(
	const uint8_t *rects,
	size_t count,
	unsigned ordering,
	int msb)
{
	size_t index;
	int previous_x;
	int previous_y;
	int next_x;
	int next_y;
	unsigned previous_width;
	unsigned previous_height;
	unsigned next_height;

	/* Any order, or too few to have one. */
	if (ordering == SHAPE_UNSORTED || count < 2U)
		return 1;

	/* Each rectangle after the one before it. */
	for (index = 1U; index < count; index++) {
		previous_x = (int16_t)x11_read16(rects + (index - 1U) * 8U, msb);
		previous_y = (int16_t)x11_read16(rects + (index - 1U) * 8U + 2U, msb);
		previous_width = x11_read16(rects + (index - 1U) * 8U + 4U, msb);
		previous_height = x11_read16(rects + (index - 1U) * 8U + 6U, msb);
		next_x = (int16_t)x11_read16(rects + index * 8U, msb);
		next_y = (int16_t)x11_read16(rects + index * 8U + 2U, msb);
		next_height = x11_read16(rects + index * 8U + 6U, msb);

		/* Never above the one before. */
		if (next_y < previous_y)
			return 0;
		if (ordering == SHAPE_Y_SORTED)
			continue;

		/* On the same row, never to its left. */
		if (ordering == SHAPE_YX_SORTED) {
			if (next_y == previous_y && next_x < previous_x)
				return 0;
			continue;
		}

		/* Banded: a new band below the last, or the same band after its rectangle. */
		if (next_y != previous_y && next_y < previous_y + (int)previous_height)
			return 0;
		if (next_y == previous_y) {
			if (next_height != previous_height)
				return 0;
			if (next_x < previous_x + (int)previous_width)
				return 0;
		}
	}

	/* Succeeded: in order. */
	return 1;
}

/*
 * Does an operation on a window's region of a kind, as the X.Org server
 * does: the source (NULL: None) is moved by the offset; None makes the
 * window unshaped of the kind (a window already unshaped hears nothing);
 * a union with no region leaves none (the default is all of the window),
 * an intersection or a subtraction starts from the default, and an
 * inversion with no region makes it empty.  The root window is never
 * shaped.  The window's newly seen parts are exposed, and those who asked
 * are told.  The source is the caller's still.  Returns 0, BadValue for
 * an operation it does not know, or BadAlloc.
 */
static unsigned
shape_operate(
	struct x11server *server,
	struct x11_window *window,
	unsigned kind,
	unsigned operation,
	struct x11_region *source,
	int x_offset,
	int y_offset)
{
	struct x11_region result;
	struct x11_region inverse;
	struct x11_shape *shape;
	struct x11_box box;
	int error;

	/* The root window keeps its shape. */
	if (window == server->root)
		return 0U;

	/* None: the window's region of the kind goes, if it has one. */
	if (source == NULL) {
		if (window->shape == NULL || !window->shape->set[kind])
			return 0U;
		x11_draw_begin(server, window, NULL);
		x11_region_clear(&window->shape->region[kind]);
		window->shape->set[kind] = 0;
		window->shape->valid = 0;
		x11_draw_end(server);
		shape_changed(server, window, kind);
		shape_notify(server, window, kind);
		return 0U;
	}

	/* An operation it knows. */
	if (operation > SHAPE_INVERT)
		return X11_BAD_VALUE;

	/* The source at its place. */
	x11_region_translate(source, x_offset, y_offset);

	/* The window's region of the kind, or the start the operation takes without one. */
	shape = shape_of(window);
	if (shape == NULL)
		return X11_BAD_ALLOC;
	x11_region_init(&result);
	x11_region_init(&inverse);
	error = 0;
	if (shape->set[kind]) {
		error = x11_region_copy(&result, &shape->region[kind]);
	} else if (operation == SHAPE_SUBTRACT) {
		shape_default(window, kind, &box);
		error = x11_region_set(&result, &box);
	}

	/* The operation on it. */
	if (error == 0) {
		switch (operation) {
		case SHAPE_SET:
			error = x11_region_copy(&result, source);
			break;
		case SHAPE_UNION:
			if (shape->set[kind])
				error = x11_region_union(&result, source);
			break;
		case SHAPE_INTERSECT:
			if (shape->set[kind]) {
				error = x11_region_intersect(&result, source);
			} else {
				error = x11_region_copy(&result, source);
			}
			break;
		case SHAPE_SUBTRACT:
			error = x11_region_subtract(&result, source);
			break;
		default:
			/* Invert: the source less the region (empty without one, as the X.Org server has it). */
			if (shape->set[kind]) {
				error = x11_region_copy(&inverse, source);
				if (error == 0)
					error = x11_region_subtract(&inverse, &result);
				if (error == 0)
					error = x11_region_copy(&result, &inverse);
			} else {
				x11_region_clear(&result);
			}
			break;
		}
	}

	x11_region_fini(&inverse);

	/* A union with no region leaves the window unshaped of the kind; those who asked hear of it still. */
	if (error == 0 && operation == SHAPE_UNION && !shape->set[kind]) {
		x11_region_fini(&result);
		shape_changed(server, window, kind);
		shape_notify(server, window, kind);
		return 0U;
	}

	/* In X's form, so that GetRectangles gives what Xvfb gives. */
	if (error == 0)
		error = x11_region_canonical(&result);
	if (error != 0) {
		x11_region_fini(&result);
		return X11_BAD_ALLOC;
	}

	/* The region kept, what the window shows following it. */
	x11_draw_begin(server, window, NULL);
	x11_region_fini(&shape->region[kind]);
	shape->region[kind] = result;
	shape->set[kind] = 1;
	shape->valid = 0;
	x11_draw_end(server);

	/* Succeeded: the window's surface and those who asked are told. */
	shape_changed(server, window, kind);
	shape_notify(server, window, kind);
	return 0U;
}

/* Returns a window's shape's state, made empty when it has none; NULL without memory. */
static struct x11_shape *
shape_of(
	struct x11_window *window)
{
	struct x11_shape *shape;
	unsigned kind;

	/* Already there. */
	if (window->shape != NULL)
		return window->shape;

	/* Succeeded: a new, unshaped state. */
	shape = calloc(1U, sizeof(*shape));
	if (shape == NULL)
		return NULL;
	for (kind = 0U; kind < SHAPE_KINDS; kind++)
		x11_region_init(&shape->region[kind]);
	x11_region_init(&shape->outside);
	x11_region_init(&shape->inside);
	x11_region_init(&shape->input);
	window->shape = shape;
	return shape;
}

/*
 * Gives a window's default region of a kind, from its origin: the window
 * with its border (bounding, input), or its inside (clip).
 */
static void
shape_default(
	const struct x11_window *window,
	unsigned kind,
	struct x11_box *box)
{
	/* The inside. */
	if (kind == X11_SHAPE_CLIP) {
		box->x0 = 0;
		box->y0 = 0;
		box->x1 = (int)window->width;
		box->y1 = (int)window->height;
		return;
	}

	/* Succeeded: with the border. */
	box->x0 = -(int)window->border;
	box->y0 = -(int)window->border;
	box->x1 = (int)window->width + (int)window->border;
	box->y1 = (int)window->height + (int)window->border;
}

/*
 * Works out a window's effective regions for its size and border, unless
 * they are worked out already.  Returns 0, or ENOMEM.
 */
static int
shape_effective(
	const struct x11_window *window)
{
	struct x11_shape *shape;
	struct x11_box box;
	int error;

	/* Still valid for the window's size. */
	shape = window->shape;
	if (shape->valid &&
	    shape->valid_width == window->width &&
	    shape->valid_height == window->height &&
	    shape->valid_border == window->border)
		return 0;

	/* Outside: the bounding region cut to the window with its border. */
	shape_default(window, X11_SHAPE_BOUNDING, &box);
	if (shape->set[X11_SHAPE_BOUNDING]) {
		error = x11_region_copy(&shape->outside, &shape->region[X11_SHAPE_BOUNDING]);
		if (error != 0)
			return error;
		x11_region_intersect_box(&shape->outside, &box);
	} else {
		error = x11_region_set(&shape->outside, &box);
		if (error != 0)
			return error;
	}

	/* Inside: the outside cut to the window's inside and to the clip region. */
	error = x11_region_copy(&shape->inside, &shape->outside);
	if (error != 0)
		return error;
	shape_default(window, X11_SHAPE_CLIP, &box);
	x11_region_intersect_box(&shape->inside, &box);
	if (shape->set[X11_SHAPE_CLIP]) {
		error = x11_region_intersect(&shape->inside, &shape->region[X11_SHAPE_CLIP]);
		if (error != 0)
			return error;
	}

	/* Input: the input region cut to the outside. */
	error = x11_region_copy(&shape->input, &shape->outside);
	if (error != 0)
		return error;
	if (shape->set[X11_SHAPE_INPUT]) {
		error = x11_region_intersect(&shape->input, &shape->region[X11_SHAPE_INPUT]);
		if (error != 0)
			return error;
	}

	/* Succeeded: valid for this size. */
	shape->valid = 1;
	shape->valid_width = window->width;
	shape->valid_height = window->height;
	shape->valid_border = window->border;
	return 0;
}

/*
 * Gives the box around a window's region of a kind as kept (the default's
 * when none is set) and whether one is set.
 */
static void
shape_extents(
	const struct x11_window *window,
	unsigned kind,
	struct x11_box *extents,
	int *shaped)
{
	/* A region set. */
	if (window->shape != NULL && window->shape->set[kind]) {
		x11_region_extents(&window->shape->region[kind], extents);
		*shaped = 1;
		return;
	}

	/* Succeeded: the default's. */
	shape_default(window, kind, extents);
	*shaped = 0;
}

/*
 * Notes that a window's region of a kind changed: a top-level window's
 * surface follows its bounding region.
 */
static void
shape_changed(
	struct x11server *server,
	struct x11_window *window,
	unsigned kind)
{
	/* The clip region is drawing's alone. */
	if (kind == X11_SHAPE_CLIP)
		return;

	/* Succeeded: the window manager tells the surface. */
	x11_wm_shape_changed(server, window);
}

/*
 * Sends ShapeNotify to every client that asked on a window: the kind, the
 * box around the region (as QueryExtents gives it), the time and whether
 * it is set.
 */
static void
shape_notify(
	struct x11server *server,
	struct x11_window *window,
	unsigned kind)
{
	struct x11_client *client;
	struct x11_box extents;
	uint8_t event[32];
	unsigned slot;
	int shaped;
	int msb;

	/* Nobody asked. */
	if (window->shape == NULL || window->shape->select_count == 0U)
		return;

	/* The box; the input region's default is the bounding one's. */
	shape_extents(window, kind, &extents, &shaped);

	/* Each client that asked, in its byte order. */
	for (slot = 0U; slot < window->shape->select_count; slot++) {
		client = x11_client_of(server, window->shape->selects[slot]);
		if (client == NULL)
			continue;
		msb = client->order;
		memset(event, 0, sizeof(event));
		event[0] = (uint8_t)X11_SHAPE_EVENT;
		event[1] = (uint8_t)kind;
		x11_write32(event + 4, window->id, msb);
		x11_write16(event + 8, (uint16_t)extents.x0, msb);
		x11_write16(event + 10, (uint16_t)extents.y0, msb);
		x11_write16(event + 12, (uint16_t)(extents.x1 - extents.x0), msb);
		x11_write16(event + 14, (uint16_t)(extents.y1 - extents.y0), msb);
		x11_write32(event + 16, server->now, msb);
		event[20] = (uint8_t)shaped;
		x11_extension_event(server, client, event);
	}
}

/*
 * Makes the region of a bitmap's set pixels in X's banded form: each
 * row's runs, a row with the same runs as the band above lengthening it.
 * Returns 0, or ENOMEM.
 */
static int
shape_bitmap(
	const struct x11_image *image,
	struct x11_region *region)
{
	const uint32_t *row;
	struct x11_box box;
	size_t band_start;
	size_t band_count;
	size_t previous_start;
	size_t previous_count;
	size_t index;
	int same;
	int x;
	int y;
	int start;
	int error;

	/* Each row, from the top. */
	x11_region_clear(region);
	previous_start = 0U;
	previous_count = 0U;
	for (y = 0; y < image->height; y++) {
		row = image->pixels + (size_t)y * image->stride;

		/* The row's runs of set pixels, left to right. */
		band_start = region->count;
		x = 0;
		while (x < image->width) {
			if (row[x] == 0U) {
				x++;
				continue;
			}
			start = x;
			while (x < image->width && row[x] != 0U)
				x++;
			box.x0 = start;
			box.y0 = y;
			box.x1 = x;
			box.y1 = y + 1;
			if (region->count >= SHAPE_BITMAP_BOXES_MAX)
				return ENOMEM;
			error = x11_region_append_box(region, &box);
			if (error != 0)
				return error;
		}
		band_count = region->count - band_start;
		if (band_count == 0U)
			continue;

		/* The same runs as the band just above: that band grows instead. */
		same = 0;
		if (band_count == previous_count && region->boxes[previous_start].y1 == y) {
			same = 1;
			for (index = 0U; index < band_count; index++) {
				if (region->boxes[previous_start + index].x0 != region->boxes[band_start + index].x0)
					same = 0;
				if (region->boxes[previous_start + index].x1 != region->boxes[band_start + index].x1)
					same = 0;
			}
		}
		if (same) {
			for (index = 0U; index < band_count; index++)
				region->boxes[previous_start + index].y1 = y + 1;
			region->count = band_start;
			continue;
		}
		previous_start = band_start;
		previous_count = band_count;
	}

	/* Succeeded: the region. */
	return 0;
}

/* Tells whether a place (from a window's origin) is in a region. */
static int
shape_point_in(
	const struct x11_region *region,
	int x,
	int y)
{
	size_t index;

	/* Each box. */
	for (index = 0U; index < region->count; index++) {
		if (x < region->boxes[index].x0 || x >= region->boxes[index].x1)
			continue;
		if (y < region->boxes[index].y0 || y >= region->boxes[index].y1)
			continue;
		return 1;
	}

	/* In none. */
	return 0;
}

/* Frees a window's shape's state. */
static void
shape_free(
	struct x11_shape *shape)
{
	unsigned kind;

	/* The regions, the list, the state. */
	for (kind = 0U; kind < SHAPE_KINDS; kind++)
		x11_region_fini(&shape->region[kind]);
	x11_region_fini(&shape->outside);
	x11_region_fini(&shape->inside);
	x11_region_fini(&shape->input);
	free(shape->selects);
	free(shape);
}
