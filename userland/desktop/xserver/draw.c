/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pixels of the windows, their clips and exposures, and the drawing
 * requests.
 *
 * A window has no pixels of its own.  A viewable top-level InputOutput
 * window has one image, which its surface shows; it and its inferiors draw
 * into it, each inside its clip: the part of it not covered by a sibling
 * above it or above an ancestor, inside every ancestor, and (unless a
 * graphics context includes inferiors) not covered by its own mapped
 * children.  The clip is worked out from the tree each time it is needed.
 * The root window of the rootless server has no pixels: drawing there is
 * dropped, and its image reads as its background.
 *
 * A change to a tree (a window mapped, unmapped, moved, resized,
 * restacked) is bracketed by x11_draw_begin and x11_draw_end: the first
 * keeps what each window could show and the pixels of the window that
 * moves or changes size, the second keeps the pixels that are still
 * there (moved with their window, or by the bit gravity of the resized
 * one), paints each newly visible part with its window's border and
 * background, and sends Expose for it.  There is no backing store.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The requests named in GraphicsExpose and NoExpose, and those that share a handler. */
#define REQUEST_COPY_AREA		62U
#define REQUEST_IMAGE_TEXT16		77U

/* The events of a copy whose source could or could not be read. */
#define EVENT_GRAPHICS_EXPOSE		13U
#define EVENT_NO_EXPOSE			14U

/* The image formats. */
#define FORMAT_XY_BITMAP		0U
#define FORMAT_XY_PIXMAP		1U
#define FORMAT_Z_PIXMAP			2U

/* The coordinate mode relative to the previous point, and FillPoly's last shape hint (Convex). */
#define COORDINATES_PREVIOUS		1U
#define POLYGON_CONVEX			2U

/* The line styles, and the pie-slice arc mode. */
#define LINE_SOLID			0U
#define LINE_ON_OFF_DASH		1U
#define LINE_DOUBLE_DASH		2U
#define ARC_PIE_SLICE			1U

/* The bit gravities: Forget, and Static (the others are the nine places). */
#define GRAVITY_FORGET			0U
#define GRAVITY_STATIC			10U

/* What an image not yet painted shows, and the colour text is drawn in without a graphics context. */
#define DRAW_UNPAINTED			0x607080U

/* How far from the origin a relative point may walk (further is as far). */
#define DRAW_FAR			131072

/* The rows and columns a top-level window's pixels are given room in. */
#define DRAW_IMAGE_STEP			64

/*
 * What a drawing request draws into: the image (NULL: nothing is drawn,
 * for the root window or a window that cannot be seen), where the
 * drawable's origin is on it, the clip on the image, the drawable (a
 * window or a pixmap), its size and depth, and the top-level window whose
 * image it is.
 */
struct draw_target {
	struct x11_image *image;
	int x;
	int y;
	struct x11_region clip;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	int width;
	int height;
	unsigned depth;
	struct x11_window *top;
};

/*
 * A shape request on its way: what it draws into, its context, the
 * paints of its even dashes (and of a solid line or a fill) and of a
 * double dash's odd dashes, the spans of the shape being made, the box
 * around what has been painted, and whether memory ran out.
 */
struct draw_shape {
	struct draw_target target;
	struct x11_gc *gc;
	struct x11_paint paint;
	struct x11_paint odd_paint;
	struct x11_spans even;
	struct x11_spans odd;
	struct x11_box bounds;
	int painted;
	int failed;
};

static int draw_top_sync(struct x11server *server, struct x11_window *top);
static void draw_top_free(struct x11server *server, struct x11_window *top);
static int draw_clip(struct x11server *server, const struct x11_window *window, int border, int children, struct x11_region *clip);
static int draw_outline(struct x11server *server, const struct x11_window *window, struct x11_region *outline);
static void draw_frame_free(struct x11_draw_frame *frame);
static int draw_frame_record(struct x11server *server, struct x11_draw_frame *frame);
static void draw_frame_save(struct x11server *server, struct x11_draw_frame *frame);
static struct x11_draw_record *draw_frame_find(struct x11_draw_frame *frame, const struct x11_window *window);
static int draw_window_after(struct x11server *server, struct x11_draw_frame *frame, struct x11_window *window);
static void draw_content_offset(const struct x11_draw_frame *frame, const struct x11_draw_record *record, const struct x11_window *window, int *dx, int *dy, int *kept);
static void draw_paint_background(struct x11server *server, struct x11_window *window, const struct x11_region *region);
static void draw_paint_border(struct x11server *server, struct x11_window *window, const struct x11_region *region);
static void draw_expose(struct x11server *server, struct x11_window *window, struct x11_region *region);
static unsigned draw_target(struct x11server *server, uint32_t id, unsigned subwindow_mode, struct draw_target *target);
static unsigned draw_target_gc(struct x11server *server, uint32_t id, const struct x11_gc *gc, struct draw_target *target);
static void draw_target_fini(struct draw_target *target);
static unsigned draw_exists(struct x11server *server, uint32_t id);
static void draw_paint_of(const struct x11_gc *gc, const struct draw_target *target, struct x11_paint *paint);
static void draw_dirty(struct x11server *server, const struct draw_target *target, const struct x11_box *bounds);
static void draw_dirty_window(struct x11server *server, const struct x11_window *top);
static void draw_graphics_expose(struct x11server *server, struct x11_client *client, uint32_t drawable, struct x11_region *region, uint8_t major);
static unsigned draw_put_rows(struct x11server *server, struct draw_target *target, const struct x11_gc *gc, const uint8_t *request, size_t length, int msb);
static uint32_t draw_bit(const uint8_t *row, unsigned index, int msb);
static void draw_set_bit(uint8_t *row, unsigned index, int msb);
static size_t draw_bitmap_row(unsigned bits);
static struct x11_window *draw_next_over(struct x11_window *window, const struct x11_window *top);
static unsigned draw_copy(struct x11server *server, unsigned index, const uint8_t *request, uint32_t plane);
static unsigned draw_shape_begin(struct x11server *server, unsigned index, const uint8_t *request, struct draw_shape *shape);
static void draw_shape_fini(struct draw_shape *shape);
static void draw_shape_paint(struct draw_shape *shape, int merge);
static void draw_shape_widen(struct draw_shape *shape, const struct x11_box *box);
static unsigned draw_shape_end(struct x11server *server, struct draw_shape *shape);
static void draw_shape_path(struct draw_shape *shape, const int *points, size_t count);
static int *draw_points(struct draw_shape *shape, const uint8_t *bytes, size_t count, int relative, int msb);
static int draw_far(int value);
static int draw_line_merged(const struct x11_gc *gc);
static void draw_arc_of(const struct draw_shape *shape, const uint8_t *bytes, int msb, struct x11_arc *arc);

/*
 * Gives the bytes an image of a size is charged as.
 */
size_t
x11_image_bytes(
	int width,
	int height)
{
	/* Four bytes a pixel. */
	return (size_t)width * (size_t)height * sizeof(uint32_t);
}

/*
 * Copies an image into a new one of its own (rows of its width).
 * Returns 0 with *copy set, or ENOMEM.
 */
int
x11_image_copy(
	const struct x11_image *source,
	struct x11_image **copy)
{
	struct x11_image *image;
	int row;

	/* The image. */
	image = malloc(sizeof(*image));
	if (image == NULL)
		return ENOMEM;

	/* Its pixels. */
	image->pixels = malloc(x11_image_bytes(source->width, source->height));
	if (image->pixels == NULL) {
		free(image);
		return ENOMEM;
	}

	/* The source's size and depth, and its pixels row by row. */
	image->width = source->width;
	image->height = source->height;
	image->stride = (size_t)source->width;
	image->depth = source->depth;
	for (row = 0; row < source->height; row++)
		memcpy(image->pixels + (size_t)row * image->stride, source->pixels + (size_t)row * source->stride, (size_t)source->width * sizeof(uint32_t));

	/* Succeeded: the copy. */
	*copy = image;
	return 0;
}

/*
 * Frees an image made by x11_image_copy (NULL: nothing).
 */
void
x11_image_free(
	struct x11_image *image)
{
	/* Nothing to free. */
	if (image == NULL)
		return;

	/* The pixels, and the image. */
	free(image->pixels);
	free(image);
}

/*
 * Starts a change to the tree a window is in: what each window of its
 * top-level window could show is kept, and the pixels of the window that
 * moves or changes size (NULL: none).  Changes nest; the outermost one
 * of a top-level window keeps the state.
 */
void
x11_draw_begin(
	struct x11server *server,
	struct x11_window *window,
	struct x11_window *changing)
{
	struct x11_draw_frame *frame;
	struct x11_window *top;
	int error;

	/* The top-level window the change is in (none for the root window itself). */
	top = NULL;
	if (window != server->root)
		top = x11_window_top_level(window);

	/* A change inside one already under way in the same tree, or past the frames there are, joins it. */
	if (server->frame_count != 0U) {
		frame = &server->frames[server->frame_count - 1U];
		if (frame->top == top || server->frame_count == X11_DRAW_FRAMES) {
			frame->depth++;
			return;
		}
	}

	/* A new frame for the tree. */
	frame = &server->frames[server->frame_count];
	server->frame_count++;
	memset(frame, 0, sizeof(*frame));
	frame->top = top;
	frame->changing = changing;
	frame->depth = 1U;
	if (top == NULL)
		return;
	frame->top_x = top->x;
	frame->top_y = top->y;

	/* A tree without pixels keeps nothing: all of it is new afterwards. */
	if (top->image.pixels == NULL)
		return;

	/* What each window could show, and the changing window's pixels; without memory, all is new afterwards. */
	error = draw_frame_record(server, frame);
	if (error != 0) {
		draw_frame_free(frame);
		return;
	}
	frame->top_had_image = 1;
	draw_frame_save(server, frame);
}

/*
 * Ends a change to a tree: the top-level window's pixels follow it (made,
 * freed or resized), the pixels still there are kept, and each newly
 * visible part of a window is painted and exposed.
 */
void
x11_draw_end(
	struct x11server *server)
{
	struct x11_draw_frame *frame;
	struct x11_window *top;
	struct x11_window *window;
	int changed;
	int painted;
	int fresh;

	/* A change nested inside another ends with it. */
	if (server->frame_count == 0U)
		return;
	frame = &server->frames[server->frame_count - 1U];
	frame->depth--;
	if (frame->depth != 0U)
		return;

	/* A tree still under the root window, whose pixels follow it. */
	top = frame->top;
	changed = 0;
	if (top != NULL && top->parent == server->root) {
		fresh = draw_top_sync(server, top);
		frame->fresh = fresh;

		/* Each viewable window of it that shows something, before its children. */
		window = top;
		while (window != NULL && top->image.pixels != NULL) {
			if (!window->mapped) {
				window = draw_next_over(window, top);
				continue;
			}
			if (window->window_class == X11_INPUT_OUTPUT) {
				painted = draw_window_after(server, frame, window);
				if (painted)
					changed = 1;
			}
			window = x11_window_next(window, top);
		}
	}

	/* The surface shows the tree again. */
	if (changed)
		draw_dirty_window(server, top);

	/* Succeeded: the frame goes. */
	draw_frame_free(frame);
	server->frame_count--;
}

/*
 * Frees what a window going away holds for drawing: a top-level
 * window's pixels, and its background and border copies, with their
 * charge.
 */
void
x11_draw_forget(
	struct x11server *server,
	struct x11_window *window)
{
	unsigned index;

	/* Its pixels. */
	draw_top_free(server, window);

	/* Its background and border copies. */
	x11_image_free(window->background_tile);
	x11_image_free(window->border_tile);
	window->background_tile = NULL;
	window->border_tile = NULL;
	x11_memory_release(server, window->owner, window->tiles_bytes);
	window->tiles_bytes = 0U;

	/* A frame of a change under way no longer names it as its tree or its changing window. */
	for (index = 0U; index < server->frame_count; index++) {
		if (server->frames[index].top == window)
			server->frames[index].top = NULL;
		if (server->frames[index].changing == window)
			server->frames[index].changing = NULL;
	}
}

/*
 * Paints the part of a window's border that can be seen (its border
 * changed), and has it shown again.
 */
void
x11_draw_border(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_region outline;
	struct x11_window *top;
	int error;

	/* The border that can be seen, on its top-level window's pixels. */
	x11_region_init(&outline);
	error = draw_outline(server, window, &outline);
	top = x11_window_top_level(window);

	/* Painted and shown, when there is any. */
	if (error == 0 && outline.count != 0U) {
		draw_paint_border(server, window, &outline);
		draw_dirty_window(server, top);
	}
	x11_region_fini(&outline);
}

/*
 * Copies a pixmap for a window's background or border, charged to its
 * owner: *tile replaces the copy there was (whose charge *charged loses).
 * Returns 0, BadPixmap, BadMatch for a pixmap of another depth, or
 * BadAlloc.
 */
unsigned
x11_draw_set_tile(
	struct x11server *server,
	unsigned owner,
	uint32_t pixmap_id,
	struct x11_image **tile,
	size_t *charged)
{
	struct x11_pixmap *pixmap;
	struct x11_image *copy;
	size_t old_bytes;
	size_t bytes;
	int error;

	/* A pixmap of the screen's depth. */
	pixmap = x11_pixmap_find(server, pixmap_id);
	if (pixmap == NULL)
		return X11_BAD_PIXMAP;
	if (pixmap->depth != X11_DEPTH)
		return X11_BAD_MATCH;

	/* The owner is charged for the copy. */
	bytes = x11_image_bytes(pixmap->width, pixmap->height);
	error = x11_memory_charge(server, owner, bytes);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* The copy. */
	error = x11_image_copy(&pixmap->image, &copy);
	if (error != 0) {
		x11_memory_release(server, owner, bytes);
		return X11_BAD_ALLOC;
	}

	/* The old copy's charge goes. */
	if (*tile != NULL) {
		old_bytes = x11_image_bytes((*tile)->width, (*tile)->height);
		x11_memory_release(server, owner, old_bytes);
		*charged -= old_bytes;
	}

	/* Succeeded: the copy replaces the old one. */
	x11_image_free(*tile);
	*tile = copy;
	*charged += bytes;
	return 0U;
}

/*
 * CreatePixmap: pixels of 0 of a size, of depth 1 or the screen's,
 * charged to the client.
 */
unsigned
x11_request_create_pixmap(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_client *client;
	struct x11_pixmap *pixmap;
	uint16_t width;
	uint16_t height;
	uint32_t id;
	size_t bytes;
	unsigned code;
	unsigned taken;
	int chosen;
	int error;
	int msb;

	/* An id of the client's range not in use, a drawable, a size and a depth. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, msb);
	chosen = x11_id_chosen(client, id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;
	code = draw_target(server, x11_read32(request + 8, msb), X11_CLIP_BY_CHILDREN, &target);
	draw_target_fini(&target);
	if (code != 0U && code != X11_BAD_MATCH)
		return code;
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	if (width == 0U || height == 0U)
		return X11_BAD_VALUE;
	if (request[1] != 1U && request[1] != X11_DEPTH)
		return X11_BAD_VALUE;

	/* The client holds the pixmap and its pixels. */
	bytes = sizeof(*pixmap) + x11_image_bytes(width, height);
	error = x11_memory_charge(server, index, bytes);
	if (error != 0)
		return X11_BAD_ALLOC;
	pixmap = calloc(1U, sizeof(*pixmap));
	if (pixmap == NULL) {
		x11_memory_release(server, index, bytes);
		return X11_BAD_ALLOC;
	}
	pixmap->image.pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (pixmap->image.pixels == NULL) {
		x11_memory_release(server, index, bytes);
		free(pixmap);
		return X11_BAD_ALLOC;
	}

	/* Its size and depth. */
	pixmap->id = id;
	pixmap->owner = index;
	pixmap->depth = request[1];
	pixmap->width = width;
	pixmap->height = height;
	pixmap->image.width = width;
	pixmap->image.height = height;
	pixmap->image.stride = width;
	pixmap->image.depth = request[1];

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_PIXMAP, pixmap);
	if (error != 0) {
		x11_pixmap_free(server, pixmap);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the pixmap is made. */
	return 0U;
}

/*
 * FreePixmap: the pixmap and its pixels go (the copies graphics contexts
 * and windows took of it stay).
 */
unsigned
x11_request_free_pixmap(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_pixmap *pixmap;

	/* The pixmap of the id. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	pixmap = x11_pixmap_find(server, x11_read32(request + 4, server->clients[index].order));
	if (pixmap == NULL)
		return X11_BAD_PIXMAP;

	/* Succeeded: its id goes, and it with its pixels and charge. */
	x11_resource_remove(server, pixmap->id);
	x11_pixmap_free(server, pixmap);
	return 0U;
}

/*
 * Frees a pixmap that no id names, with its pixels and charge.
 */
void
x11_pixmap_free(
	struct x11server *server,
	struct x11_pixmap *pixmap)
{
	/* The charge, the pixels and the pixmap. */
	x11_memory_release(server, pixmap->owner, sizeof(*pixmap) + x11_image_bytes(pixmap->width, pixmap->height));
	free(pixmap->image.pixels);
	free(pixmap);
}

/*
 * ClearArea: a rectangle of a window painted with its background (a
 * width or height of 0 reaching to the window's edge), and Expose for
 * the part that can be seen when exposures are asked for.
 */
unsigned
x11_request_clear_area(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_window *top;
	struct x11_region region;
	struct x11_box box;
	int width;
	int height;
	int x;
	int y;
	int error;
	int msb;

	/* The window, which must show something. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	if (request[1] > 1U)
		return X11_BAD_VALUE;
	window = x11_window_find(server, x11_read32(request + 4, msb));
	if (window == NULL)
		return X11_BAD_WINDOW;
	if (window->window_class == X11_INPUT_ONLY)
		return X11_BAD_MATCH;

	/* The rectangle, a zero side reaching to the window's edge. */
	x = (int16_t)x11_read16(request + 8, msb);
	y = (int16_t)x11_read16(request + 10, msb);
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	if (width == 0)
		width = (int)window->width - x;
	if (height == 0)
		height = (int)window->height - y;
	if (width <= 0 || height <= 0)
		return 0U;

	/* The part of it that can be seen (none for the root window, which has no pixels). */
	x11_region_init(&region);
	error = draw_clip(server, window, 0, 1, &region);
	if (error != 0) {
		x11_region_fini(&region);
		return X11_BAD_ALLOC;
	}
	if (region.count == 0U) {
		x11_region_fini(&region);
		return 0U;
	}
	top = x11_window_top_level(window);
	box.x0 = window->x - top->x + x;
	box.y0 = window->y - top->y + y;
	box.x1 = box.x0 + width;
	box.y1 = box.y0 + height;
	x11_region_intersect_box(&region, &box);

	/* Painted, shown again, and exposed when asked. */
	draw_paint_background(server, window, &region);
	if (region.count != 0U)
		draw_dirty_window(server, top);
	if (request[1] != 0U)
		draw_expose(server, window, &region);

	/* Succeeded: cleared. */
	x11_region_fini(&region);
	return 0U;
}

/*
 * CopyArea: a rectangle of one drawable onto another of the same depth,
 * inside the destination's clip.  The parts of the source that cannot be
 * read (outside it, or covered when it is a window) are not copied, and,
 * with graphics exposures, are told as GraphicsExpose on the destination
 * (or NoExpose when there are none).
 */
unsigned
x11_request_copy_area(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	unsigned code;

	/* The rectangle copied as it is. */
	if (length != 28U)
		return X11_BAD_LENGTH;
	code = draw_copy(server, index, request, 0U);
	if (code != 0U)
		return code;

	/* Succeeded: copied. */
	return 0U;
}

/*
 * CopyPlane: one plane of a rectangle of a drawable (of any depth) onto
 * another as a bitmap, the context's foreground where the plane is set
 * and its background where it is clear; what could not be read is told
 * as for CopyArea.
 */
unsigned
x11_request_copy_plane(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t plane;
	unsigned code;

	/* The rectangle and the plane, which must be one bit. */
	if (length != 32U)
		return X11_BAD_LENGTH;
	plane = x11_read32(request + 28, server->clients[index].order);
	if (plane == 0U || (plane & (plane - 1U)) != 0U)
		return X11_BAD_VALUE;

	/* Copied as a bitmap. */
	code = draw_copy(server, index, request, plane);
	if (code != 0U)
		return code;

	/* Succeeded: copied. */
	return 0U;
}

/*
 * PolyFillRectangle: rectangles of a drawable filled with the context's
 * paint, in the order given.
 */
unsigned
x11_request_poly_fill_rectangle(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_paint paint;
	struct x11_box bounds;
	struct x11_box box;
	struct x11_gc *gc;
	size_t offset;
	unsigned code;
	int msb;

	/* The context, the drawable, and whole rectangles. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 8U != 0U)
		return X11_BAD_LENGTH;
	code = draw_exists(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	if (gc == NULL)
		return X11_BAD_GC;
	code = draw_target_gc(server, x11_read32(request + 4, msb), gc, &target);
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}

	/* Nothing is drawn where nothing can be. */
	if (target.image == NULL) {
		draw_target_fini(&target);
		return 0U;
	}

	/* Each rectangle, on the image, and the box around them all. */
	draw_paint_of(gc, &target, &paint);
	memset(&bounds, 0, sizeof(bounds));
	for (offset = 12U; offset + 8U <= length; offset += 8U) {
		box.x0 = target.x + (int16_t)x11_read16(request + offset, msb);
		box.y0 = target.y + (int16_t)x11_read16(request + offset + 2U, msb);
		box.x1 = box.x0 + (int)x11_read16(request + offset + 4U, msb);
		box.y1 = box.y0 + (int)x11_read16(request + offset + 6U, msb);
		x11_render_boxes(target.image, &target.clip, &paint, &box, 1U);
		if (offset == 12U)
			bounds = box;
		if (box.x0 < bounds.x0)
			bounds.x0 = box.x0;
		if (box.y0 < bounds.y0)
			bounds.y0 = box.y0;
		if (box.x1 > bounds.x1)
			bounds.x1 = box.x1;
		if (box.y1 > bounds.y1)
			bounds.y1 = box.y1;
	}

	/* Succeeded: filled, and shown again. */
	draw_dirty(server, &target, &bounds);
	draw_target_fini(&target);
	return 0U;
}

/*
 * PutImage: an image of the request onto a drawable, by the context's
 * function and plane mask, inside its clip.  XYBitmap paints the
 * foreground where a bit is set and the background where it is clear;
 * XYPixmap gives each plane as a bitmap, the most significant first;
 * ZPixmap gives whole pixels (32 bits for depth 24, one bit for depth 1).
 * Bitmaps are rows padded to 32 bits after left-pad bits, in the bit
 * order the setup gave the client (its own byte order).
 */
unsigned
x11_request_put_image(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_gc *gc;
	unsigned format;
	unsigned depth;
	unsigned code;
	int msb;

	/* The format, the context and the drawable. */
	msb = server->clients[index].order;
	if (length < 24U)
		return X11_BAD_LENGTH;
	format = request[1];
	if (format > FORMAT_Z_PIXMAP)
		return X11_BAD_VALUE;
	code = draw_exists(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	if (gc == NULL)
		return X11_BAD_GC;
	code = draw_target_gc(server, x11_read32(request + 4, msb), gc, &target);
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}

	/* The depth the format allows: 1 for a bitmap, the drawable's for the others; ZPixmap has no left pad. */
	depth = request[21];
	code = 0U;
	if (format == FORMAT_XY_BITMAP && depth != 1U)
		code = X11_BAD_MATCH;
	if (format != FORMAT_XY_BITMAP && depth != target.depth)
		code = X11_BAD_MATCH;
	if (format == FORMAT_Z_PIXMAP && request[20] != 0U)
		code = X11_BAD_MATCH;
	if (code == 0U)
		code = draw_put_rows(server, &target, gc, request, length, msb);

	/* Succeeded: put, or the error. */
	draw_target_fini(&target);
	return code;
}

/*
 * GetImage: a rectangle of a drawable in XYPixmap (the planes the mask
 * names, each a bitmap) or ZPixmap (whole pixels, each masked).  A window
 * must be viewable and the rectangle inside it and its border, and inside
 * its top-level window's pixels; a pixmap's rectangle inside it.  What is
 * covered reads as whatever the pixels hold; the root window reads as its
 * background.
 */
unsigned
x11_request_get_image(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_client *client;
	struct x11_box rectangle;
	struct x11_box bounds;
	struct x11_box inside;
	uint32_t plane_mask;
	uint32_t *row_pixels;
	uint32_t pixel;
	uint8_t *reply;
	uint8_t *data;
	size_t row_bytes;
	size_t plane_bytes;
	size_t size;
	unsigned format;
	unsigned planes;
	unsigned plane;
	unsigned code;
	int width;
	int height;
	int x;
	int y;
	int row;
	int column;
	int contained;
	int msb;

	/* The format and the drawable. */
	client = &server->clients[index];
	msb = client->order;
	if (length != 20U)
		return X11_BAD_LENGTH;
	format = request[1];
	if (format != FORMAT_XY_PIXMAP && format != FORMAT_Z_PIXMAP)
		return X11_BAD_VALUE;
	code = draw_target(server, x11_read32(request + 4, msb), X11_INCLUDE_INFERIORS, &target);
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}
	x = (int16_t)x11_read16(request + 8, msb);
	y = (int16_t)x11_read16(request + 10, msb);
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	plane_mask = x11_read32(request + 16, msb);

	/* The rectangle inside the drawable (a window's border included), and inside the pixels it is read from. */
	code = 0U;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = target.width;
	bounds.y1 = target.height;
	if (target.window != NULL) {
		bounds.x0 = -(int)target.window->border;
		bounds.y0 = -(int)target.window->border;
		bounds.x1 += (int)target.window->border;
		bounds.y1 += (int)target.window->border;
		contained = x11_window_viewable(target.window);
		if (!contained)
			code = X11_BAD_MATCH;
	}
	if (x < bounds.x0 || y < bounds.y0 || x + width > bounds.x1 || y + height > bounds.y1)
		code = X11_BAD_MATCH;
	rectangle.x0 = target.x + x;
	rectangle.y0 = target.y + y;
	rectangle.x1 = rectangle.x0 + width;
	rectangle.y1 = rectangle.y0 + height;
	if (target.image != NULL && target.window != NULL) {
		bounds.x0 = 0;
		bounds.y0 = 0;
		bounds.x1 = target.image->width;
		bounds.y1 = target.image->height;
		contained = x11_box_intersect(&rectangle, &bounds, &inside);
		if (width != 0 && height != 0 && (!contained || inside.x0 != rectangle.x0 || inside.y0 != rectangle.y0 || inside.x1 != rectangle.x1 || inside.y1 != rectangle.y1))
			code = X11_BAD_MATCH;
	}
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}

	/* The reply's size: a bitmap a plane, or a word a pixel of depth 24 (a bit for depth 1). */
	row_bytes = draw_bitmap_row((unsigned)width);
	plane_bytes = row_bytes * (size_t)height;
	planes = 0U;
	for (plane = 0U; plane < target.depth; plane++) {
		if ((plane_mask & ((uint32_t)1U << plane)) != 0U)
			planes++;
	}
	size = plane_bytes * planes;
	if (format == FORMAT_Z_PIXMAP) {
		size = plane_bytes;
		if (target.depth != 1U)
			size = (size_t)width * (size_t)height * 4U;
	}
	if (size > X11_OUTPUT_CAP - 32U) {
		draw_target_fini(&target);
		return X11_BAD_ALLOC;
	}

	/* The reply, and a row of pixels to read into. */
	reply = calloc(1U, 32U + size);
	if (reply == NULL) {
		draw_target_fini(&target);
		return X11_BAD_ALLOC;
	}
	row_pixels = malloc((size_t)(width + 1) * sizeof(*row_pixels));
	if (row_pixels == NULL) {
		free(reply);
		draw_target_fini(&target);
		return X11_BAD_ALLOC;
	}

	/* Each row: read (the root window's is its background), then written in the format. */
	data = reply + 32;
	for (row = 0; row < height; row++) {
		if (target.image != NULL) {
			x11_render_read(target.image, rectangle.x0, rectangle.y0 + row, width, 1, row_pixels, (size_t)width);
		} else {
			for (column = 0; column < width; column++)
				row_pixels[column] = server->root->background;
		}
		for (column = 0; column < width; column++) {
			pixel = row_pixels[column] & plane_mask;
			if (format == FORMAT_Z_PIXMAP && target.depth != 1U) {
				x11_write32(data + ((size_t)row * (size_t)width + (size_t)column) * 4U, pixel, msb);
				continue;
			}
			if (format == FORMAT_Z_PIXMAP) {
				if ((pixel & 1U) != 0U)
					draw_set_bit(data + (size_t)row * row_bytes, (unsigned)column, msb);
				continue;
			}

			/* XYPixmap: each plane asked for, the most significant first. */
			planes = 0U;
			for (plane = target.depth; plane > 0U; plane--) {
				if ((plane_mask & ((uint32_t)1U << (plane - 1U))) == 0U)
					continue;
				if ((pixel & ((uint32_t)1U << (plane - 1U))) != 0U)
					draw_set_bit(data + planes * plane_bytes + (size_t)row * row_bytes, (unsigned)column, msb);
				planes++;
			}
		}
	}

	/* The depth, and the visual of a window (none for a pixmap). */
	reply[1] = (uint8_t)target.depth;
	if (target.window != NULL)
		x11_write32(reply + 8, X11_VISUAL_XID, msb);
	x11_reply(server, client, reply, 32U + size);

	/* Succeeded: answered. */
	free(row_pixels);
	free(reply);
	draw_target_fini(&target);
	return 0U;
}

/*
 * ImageText8 and ImageText16: characters in the core font from a
 * baseline, in the context's foreground.
 */
unsigned
x11_request_image_text(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_paint paint;
	struct x11_glyph glyph;
	struct x11_box bounds;
	struct x11_gc *gc;
	uint32_t codepoint;
	size_t count;
	size_t bytes;
	size_t character;
	unsigned code;
	int missing;
	int wide;
	int x;
	int y;
	int top;
	int msb;

	/* The context and the drawable. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	code = draw_exists(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	gc = x11_gc_find(server, x11_read32(request + 8, msb));
	if (gc == NULL)
		return X11_BAD_GC;
	code = draw_target_gc(server, x11_read32(request + 4, msb), gc, &target);
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}

	/* The characters fill the request to its last word (two bytes each for ImageText16). */
	count = request[1];
	wide = 0;
	if (request[0] == REQUEST_IMAGE_TEXT16)
		wide = 1;
	bytes = count;
	if (wide)
		bytes = count * 2U;
	if (16U + ((bytes + 3U) & ~(size_t)3U) != length) {
		draw_target_fini(&target);
		return X11_BAD_LENGTH;
	}
	if (target.image == NULL) {
		draw_target_fini(&target);
		return 0U;
	}

	/* The glyphs in the foreground, copied into the planes of the mask. */
	draw_paint_of(gc, &target, &paint);
	paint.function = X11_FUNCTION_COPY;
	paint.fill_style = X11_FILL_SOLID;
	x = target.x + (int16_t)x11_read16(request + 12, msb);
	y = target.y + (int16_t)x11_read16(request + 14, msb);
	bounds.x0 = x;
	bounds.y0 = y;
	bounds.x1 = x;
	bounds.y1 = y;
	for (character = 0U; character < count; character++) {
		codepoint = request[16U + character];
		if (wide)
			codepoint = ((uint32_t)request[16U + character * 2U] << 8) | (uint32_t)request[16U + character * 2U + 1U];

		/* Its glyph, standing on the baseline; a character without one is skipped. */
		missing = x11_glyph(&server->glyphs, codepoint, &glyph);
		if (missing != 0)
			continue;
		top = y - (int)glyph.height;
		x11_render_mask(target.image, &target.clip, &paint, x, top, (int)glyph.width, (int)glyph.height, glyph.bitmap, glyph.stride);
		if (top < bounds.y0)
			bounds.y0 = top;
		x += (int)glyph.advance;
		if (x > bounds.x1)
			bounds.x1 = x;
	}

	/* Succeeded: drawn, and shown again. */
	draw_dirty(server, &target, &bounds);
	draw_target_fini(&target);
	return 0U;
}

/*
 * zedBSD's PutImageRGB24: three bytes a pixel (red, green, blue) into a
 * window or a pixmap, copied into every plane (what GLX shows its frames
 * with).
 */
unsigned
x11_request_put_image_rgb24(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_target target;
	struct x11_box bounds;
	const uint8_t *rgb;
	uint32_t *row_pixels;
	size_t count;
	unsigned code;
	int x;
	int y;
	int width;
	int height;
	int row;
	int column;
	int msb;

	/* The drawable. */
	msb = server->clients[index].order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	code = draw_target(server, x11_read32(request + 4, msb), X11_CLIP_BY_CHILDREN, &target);
	if (code != 0U) {
		draw_target_fini(&target);
		return code;
	}

	/* The rectangle, whose pixels must be in the request. */
	x = (int16_t)x11_read16(request + 8, msb);
	y = (int16_t)x11_read16(request + 10, msb);
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	count = (size_t)width * (size_t)height;
	if (count == 0U || count > (length - 16U) / 3U) {
		draw_target_fini(&target);
		return X11_BAD_LENGTH;
	}
	if (target.image == NULL) {
		draw_target_fini(&target);
		return 0U;
	}

	/* A row of pixels at a time. */
	row_pixels = malloc((size_t)width * sizeof(*row_pixels));
	if (row_pixels == NULL) {
		draw_target_fini(&target);
		return X11_BAD_ALLOC;
	}
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++) {
			rgb = request + 16U + ((size_t)row * (size_t)width + (size_t)column) * 3U;
			row_pixels[column] = ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
		}
		x11_render_put(target.image, &target.clip, target.x + x, target.y + y + row, width, 1, row_pixels, (size_t)width, X11_FUNCTION_COPY, 0xffffffffU);
	}

	/* Succeeded: put, and shown again. */
	bounds.x0 = target.x + x;
	bounds.y0 = target.y + y;
	bounds.x1 = bounds.x0 + width;
	bounds.y1 = bounds.y0 + height;
	draw_dirty(server, &target, &bounds);
	free(row_pixels);
	draw_target_fini(&target);
	return 0U;
}

/*
 * PolyPoint: points (relative to the one before in the Previous mode),
 * each pixel painted with the foreground (points take no fill style).
 */
unsigned
x11_request_poly_point(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	size_t offset;
	unsigned code;
	int last_x;
	int last_y;
	int x;
	int y;
	int msb;

	/* The coordinate mode, the drawable, the context, and whole points. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 4U != 0U)
		return X11_BAD_LENGTH;
	if (request[1] > COORDINATES_PREVIOUS)
		return X11_BAD_VALUE;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* A point is the foreground alone, whatever the fill style. */
	shape.paint.fill_style = X11_FILL_SOLID;

	/* Each point, painted as it comes (a point given twice is painted twice). */
	last_x = 0;
	last_y = 0;
	for (offset = 12U; offset + 4U <= length && shape.target.image != NULL; offset += 4U) {
		x = (int16_t)x11_read16(request + offset, msb);
		y = (int16_t)x11_read16(request + offset + 2U, msb);
		if (offset != 12U && request[1] == COORDINATES_PREVIOUS) {
			x = draw_far(x + last_x);
			y = draw_far(y + last_y);
		}
		x11_spans_add(&shape.even, shape.target.y + y, shape.target.x + x, shape.target.x + x + 1);
		last_x = x;
		last_y = y;
	}
	draw_shape_paint(&shape, 0);

	/* Succeeded unless memory ran out: painted, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * PolyLine: lines through points (relative to the one before in the
 * Previous mode).  Thin lines paint each point once (the last one not
 * with a NotLast cap, nor when it closes the path); wide lines are one
 * shape with joins where the lines meet, each pixel painted once.
 */
unsigned
x11_request_poly_line(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	size_t count;
	unsigned code;
	int *points;
	int msb;

	/* The coordinate mode, the drawable, the context, and whole points. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 4U != 0U)
		return X11_BAD_LENGTH;
	if (request[1] > COORDINATES_PREVIOUS)
		return X11_BAD_VALUE;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* The points on the image. */
	count = (length - 12U) / 4U;
	points = draw_points(&shape, request + 12, count, request[1] == COORDINATES_PREVIOUS, msb);

	/* The path, drawn as one shape. */
	if (points != NULL && count > 0U) {
		draw_shape_path(&shape, points, count);
		draw_shape_paint(&shape, draw_line_merged(shape.gc));
	}
	free(points);

	/* Succeeded unless memory ran out: drawn, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * PolySegment: lines, each from its own first point to its own second,
 * each drawn as a line of its own (its dashes start again).
 */
unsigned
x11_request_poly_segment(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	size_t offset;
	unsigned code;
	int points[4];
	int msb;

	/* The drawable, the context, and whole segments. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 8U != 0U)
		return X11_BAD_LENGTH;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* Each segment, painted on its own. */
	for (offset = 12U; offset + 8U <= length && shape.target.image != NULL; offset += 8U) {
		points[0] = shape.target.x + (int16_t)x11_read16(request + offset, msb);
		points[1] = shape.target.y + (int16_t)x11_read16(request + offset + 2U, msb);
		points[2] = shape.target.x + (int16_t)x11_read16(request + offset + 4U, msb);
		points[3] = shape.target.y + (int16_t)x11_read16(request + offset + 6U, msb);
		draw_shape_path(&shape, points, 2U);
		draw_shape_paint(&shape, draw_line_merged(shape.gc));
	}

	/* Succeeded unless memory ran out: drawn, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * PolyRectangle: the outlines of rectangles, each a closed path through
 * its corners (width + 1 by height + 1 pixels for thin lines), drawn on
 * its own.
 */
unsigned
x11_request_poly_rectangle(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	size_t offset;
	unsigned code;
	int points[10];
	int x;
	int y;
	int width;
	int height;
	int msb;

	/* The drawable, the context, and whole rectangles. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 8U != 0U)
		return X11_BAD_LENGTH;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* Each rectangle's path around its corners, back to the first. */
	for (offset = 12U; offset + 8U <= length && shape.target.image != NULL; offset += 8U) {
		x = shape.target.x + (int16_t)x11_read16(request + offset, msb);
		y = shape.target.y + (int16_t)x11_read16(request + offset + 2U, msb);
		width = x11_read16(request + offset + 4U, msb);
		height = x11_read16(request + offset + 6U, msb);
		points[0] = x;
		points[1] = y;
		points[2] = x + width;
		points[3] = y;
		points[4] = x + width;
		points[5] = y + height;
		points[6] = x;
		points[7] = y + height;
		points[8] = x;
		points[9] = y;
		draw_shape_path(&shape, points, 5U);
		draw_shape_paint(&shape, draw_line_merged(shape.gc));
	}

	/* Succeeded unless memory ran out: drawn, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * PolyArc: arcs of ellipses drawn as lines, each on its own and each
 * pixel of one painted once.
 */
unsigned
x11_request_poly_arc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	struct x11_arc arc;
	size_t offset;
	unsigned code;
	int msb;

	/* The drawable, the context, and whole arcs. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 12U != 0U)
		return X11_BAD_LENGTH;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* Each arc, painted on its own. */
	for (offset = 12U; offset + 12U <= length && shape.target.image != NULL; offset += 12U) {
		draw_arc_of(&shape, request + offset, msb, &arc);
		x11_shape_arc_line(&shape.even, &arc, shape.gc->line_width);
		draw_shape_paint(&shape, 1);
	}

	/* Succeeded unless memory ran out: drawn, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * FillPoly: a polygon through points (relative to the one before in the
 * Previous mode, its last point joined to its first) filled by the
 * context's fill rule.  The shape the client names is only a hint.
 */
unsigned
x11_request_fill_poly(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	double *corners;
	size_t count;
	size_t at;
	unsigned code;
	int *points;
	int msb;

	/* The shape and the coordinate mode, the drawable, the context, and whole points. */
	msb = server->clients[index].order;
	if (length < 16U || (length - 16U) % 4U != 0U)
		return X11_BAD_LENGTH;
	if (request[12] > POLYGON_CONVEX)
		return X11_BAD_VALUE;
	if (request[13] > COORDINATES_PREVIOUS)
		return X11_BAD_VALUE;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* The points on the image. */
	count = (length - 16U) / 4U;
	points = draw_points(&shape, request + 16, count, request[13] == COORDINATES_PREVIOUS, msb);

	/* The polygon, filled. */
	corners = NULL;
	if (points != NULL && count > 0U)
		corners = malloc(count * 2U * sizeof(*corners));
	if (corners != NULL) {
		for (at = 0U; at < count * 2U; at++)
			corners[at] = (double)points[at];
		x11_shape_polygon(&shape.even, corners, count, shape.gc->fill_rule != 0U);
		draw_shape_paint(&shape, 0);
	} else if (count > 0U) {
		shape.even.failed = 1;
	}
	free(corners);
	free(points);

	/* Succeeded unless memory ran out: filled, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * PolyFillArc: arcs of ellipses filled as pie slices or chords (the
 * context's arc mode), each on its own.
 */
unsigned
x11_request_poly_fill_arc(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct draw_shape shape;
	struct x11_arc arc;
	size_t offset;
	unsigned code;
	int msb;

	/* The drawable, the context, and whole arcs. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 12U != 0U)
		return X11_BAD_LENGTH;
	code = draw_shape_begin(server, index, request, &shape);
	if (code != 0U)
		return code;

	/* Each arc, filled on its own. */
	for (offset = 12U; offset + 12U <= length && shape.target.image != NULL; offset += 12U) {
		draw_arc_of(&shape, request + offset, msb, &arc);
		x11_shape_arc_fill(&shape.even, &arc, shape.gc->arc_mode == ARC_PIE_SLICE);
		draw_shape_paint(&shape, 0);
	}

	/* Succeeded unless memory ran out: filled, and shown again. */
	code = draw_shape_end(server, &shape);
	if (code != 0U)
		return code;

	return 0U;
}

/*
 * Brings a top-level window's pixels in step with it: none unless it is a
 * viewable InputOutput window, else its size (each side at most
 * X11_IMAGE_SIDE_MAX), in room of whole steps that a smaller size keeps
 * using until it is less than half.  Returns nonzero when the pixels are
 * new (what they showed is gone), 0 when they are as they were.  Pixels
 * that cannot be had (memory) leave the window without any: what is
 * drawn into it is dropped.
 */
static int
draw_top_sync(
	struct x11server *server,
	struct x11_window *top)
{
	uint32_t *pixels;
	size_t bytes;
	size_t index;
	int viewable;
	int width;
	int height;
	int columns;
	int rows;
	int error;

	/* A window that cannot be seen has no pixels. */
	viewable = x11_window_viewable(top);
	if (!viewable || top->window_class != X11_INPUT_OUTPUT) {
		draw_top_free(server, top);
		return 0;
	}

	/* The size, and nothing to do when the pixels have it. */
	width = top->width;
	if (width > X11_IMAGE_SIDE_MAX)
		width = X11_IMAGE_SIDE_MAX;
	height = top->height;
	if (height > X11_IMAGE_SIDE_MAX)
		height = X11_IMAGE_SIDE_MAX;
	if (top->image.pixels != NULL && top->image.width == width && top->image.height == height)
		return 0;

	/* Room that still fits and is not more than twice too big is used again, what it shows staying where it is. */
	if (top->image.pixels != NULL &&
	    (size_t)width <= top->image.stride &&
	    height <= top->image_rows &&
	    (size_t)width * 2U >= top->image.stride &&
	    height * 2 >= top->image_rows) {
		top->image.width = width;
		top->image.height = height;
		return 0;
	}

	/* New room, in whole steps, charged to the owner instead of the old. */
	draw_top_free(server, top);
	columns = (width + DRAW_IMAGE_STEP - 1) / DRAW_IMAGE_STEP * DRAW_IMAGE_STEP;
	rows = (height + DRAW_IMAGE_STEP - 1) / DRAW_IMAGE_STEP * DRAW_IMAGE_STEP;
	bytes = x11_image_bytes(columns, rows);
	error = x11_memory_charge(server, top->owner, bytes);
	if (error != 0) {
		x11_log(server, "X11SERVER PIXELS none window=0x%x width=%d height=%d (memory)", (unsigned)top->id, width, height);
		return 1;
	}
	pixels = malloc(bytes);
	if (pixels == NULL) {
		x11_memory_release(server, top->owner, bytes);
		x11_log(server, "X11SERVER PIXELS none window=0x%x width=%d height=%d (memory)", (unsigned)top->id, width, height);
		return 1;
	}

	/* Each pixel unpainted until its window paints it. */
	for (index = 0U; index < bytes / sizeof(*pixels); index++)
		pixels[index] = DRAW_UNPAINTED;

	/* Succeeded: the new pixels. */
	top->image.pixels = pixels;
	top->image.stride = (size_t)columns;
	top->image.width = width;
	top->image.height = height;
	top->image.depth = X11_DEPTH;
	top->image_rows = rows;
	top->image_bytes = bytes;
	return 1;
}

/* Frees a top-level window's pixels and their charge. */
static void
draw_top_free(
	struct x11server *server,
	struct x11_window *top)
{
	/* Nothing held. */
	if (top->image.pixels == NULL)
		return;

	/* The pixels and their charge go. */
	free(top->image.pixels);
	x11_memory_release(server, top->owner, top->image_bytes);
	memset(&top->image, 0, sizeof(top->image));
	top->image_rows = 0;
	top->image_bytes = 0U;
}

/*
 * Works out the part of a window that can be drawn, on its top-level
 * window's pixels: inside it (and its border when asked), inside each
 * ancestor, not under a mapped InputOutput sibling above it or above an
 * ancestor, and not under its own mapped InputOutput children when asked.
 * A window that cannot be seen, an InputOnly one, the root window, and a
 * tree without pixels have none.  Returns 0, or ENOMEM.
 */
static int
draw_clip(
	struct x11server *server,
	const struct x11_window *window,
	int border,
	int children,
	struct x11_region *clip)
{
	const struct x11_window *ancestor;
	const struct x11_window *sibling;
	struct x11_window *top;
	struct x11_box box;
	int viewable;
	int error;

	/* Nothing to draw on. */
	x11_region_clear(clip);
	if (window == server->root || window->window_class == X11_INPUT_ONLY)
		return 0;
	viewable = x11_window_viewable(window);
	if (!viewable)
		return 0;
	top = x11_window_top_level((struct x11_window *)window);
	if (top->image.pixels == NULL)
		return 0;

	/* The window (with its border, but a top-level window's border is not drawn). */
	box.x0 = window->x;
	box.y0 = window->y;
	box.x1 = window->x + (int)window->width;
	box.y1 = window->y + (int)window->height;
	if (border && window != top) {
		box.x0 -= (int)window->border;
		box.y0 -= (int)window->border;
		box.x1 += (int)window->border;
		box.y1 += (int)window->border;
	}
	error = x11_region_set(clip, &box);
	if (error != 0)
		return error;

	/* Inside each ancestor up to the top-level window. */
	for (ancestor = window->parent; ancestor != NULL && ancestor != server->root; ancestor = ancestor->parent) {
		box.x0 = ancestor->x;
		box.y0 = ancestor->y;
		box.x1 = ancestor->x + (int)ancestor->width;
		box.y1 = ancestor->y + (int)ancestor->height;
		x11_region_intersect_box(clip, &box);
	}

	/* Not under a sibling above it or above an ancestor below the top-level window (top-level windows are the system's to stack). */
	for (ancestor = window; ancestor != top; ancestor = ancestor->parent) {
		for (sibling = ancestor->above; sibling != NULL; sibling = sibling->above) {
			if (!sibling->mapped || sibling->window_class != X11_INPUT_OUTPUT)
				continue;
			box.x0 = sibling->x - (int)sibling->border;
			box.y0 = sibling->y - (int)sibling->border;
			box.x1 = sibling->x + (int)sibling->width + (int)sibling->border;
			box.y1 = sibling->y + (int)sibling->height + (int)sibling->border;
			error = x11_region_subtract_box(clip, &box);
			if (error != 0)
				return error;
		}
	}

	/* Not under its own children, when they clip it. */
	for (sibling = window->first_child; children && sibling != NULL; sibling = sibling->above) {
		if (!sibling->mapped || sibling->window_class != X11_INPUT_OUTPUT)
			continue;
		box.x0 = sibling->x - (int)sibling->border;
		box.y0 = sibling->y - (int)sibling->border;
		box.x1 = sibling->x + (int)sibling->width + (int)sibling->border;
		box.y1 = sibling->y + (int)sibling->height + (int)sibling->border;
		error = x11_region_subtract_box(clip, &box);
		if (error != 0)
			return error;
	}

	/* On the top-level window's pixels, inside them. */
	x11_region_translate(clip, -top->x, -top->y);
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = top->image.width;
	box.y1 = top->image.height;
	x11_region_intersect_box(clip, &box);

	/* Succeeded: the clip. */
	return 0;
}

/*
 * Works out the part of a window's border that can be seen, on its
 * top-level window's pixels (none for a top-level window or no border).
 * Returns 0, or ENOMEM.
 */
static int
draw_outline(
	struct x11server *server,
	const struct x11_window *window,
	struct x11_region *outline)
{
	struct x11_window *top;
	struct x11_box inside;
	int error;

	/* No border to draw. */
	x11_region_clear(outline);
	if (window->border == 0U || window == server->root || window->parent == server->root)
		return 0;

	/* The window with its border that can be seen, less the inside. */
	error = draw_clip(server, window, 1, 0, outline);
	if (error != 0)
		return error;
	top = x11_window_top_level((struct x11_window *)window);
	inside.x0 = window->x - top->x;
	inside.y0 = window->y - top->y;
	inside.x1 = inside.x0 + (int)window->width;
	inside.y1 = inside.y0 + (int)window->height;
	error = x11_region_subtract_box(outline, &inside);
	if (error != 0)
		return error;

	/* Succeeded: the border. */
	return 0;
}

/* Frees what a frame holds and empties it. */
static void
draw_frame_free(
	struct x11_draw_frame *frame)
{
	size_t index;

	/* Each record's regions, the records, and the pixels kept. */
	for (index = 0U; index < frame->count; index++) {
		x11_region_fini(&frame->records[index].inner);
		x11_region_fini(&frame->records[index].outline);
	}
	free(frame->records);
	frame->records = NULL;
	frame->count = 0U;
	free(frame->saved.pixels);
	memset(&frame->saved, 0, sizeof(frame->saved));
}

/*
 * Keeps, for each viewable InputOutput window of a frame's tree, where it
 * is on the pixels, its size, and what of it and its border can be seen.
 * Returns 0, or ENOMEM.
 */
static int
draw_frame_record(
	struct x11server *server,
	struct x11_draw_frame *frame)
{
	struct x11_draw_record *record;
	struct x11_window *window;
	size_t count;
	int error;

	/* How many windows there are. */
	count = 0U;
	for (window = frame->top; window != NULL; window = x11_window_next(window, frame->top))
		count++;
	frame->records = calloc(count, sizeof(*frame->records));
	if (frame->records == NULL)
		return ENOMEM;

	/* Each mapped one that shows something, not into an unmapped one. */
	window = frame->top;
	while (window != NULL) {
		if (!window->mapped) {
			window = draw_next_over(window, frame->top);
			continue;
		}
		if (window->window_class == X11_INPUT_OUTPUT) {
			record = &frame->records[frame->count];
			frame->count++;
			record->window = window;
			record->x = window->x - frame->top->x;
			record->y = window->y - frame->top->y;
			record->width = window->width;
			record->height = window->height;
			record->border = window->border;
			x11_region_init(&record->inner);
			x11_region_init(&record->outline);
			error = draw_clip(server, window, 0, 1, &record->inner);
			if (error != 0)
				return error;
			error = draw_outline(server, window, &record->outline);
			if (error != 0)
				return error;
		}
		window = x11_window_next(window, frame->top);
	}

	/* Succeeded: recorded. */
	return 0;
}

/*
 * Keeps the pixels under a frame's changing window (with its border, on
 * the top-level window's pixels): what it and its inferiors show, which
 * may move with it.  Without memory nothing is kept and they are drawn
 * again.
 */
static void
draw_frame_save(
	struct x11server *server,
	struct x11_draw_frame *frame)
{
	struct x11_window *window;
	struct x11_region area;
	struct x11_box box;
	int error;

	/* No window that moves or is resized, or one that cannot be seen. */
	window = frame->changing;
	if (window == NULL || window == server->root)
		return;
	x11_region_init(&area);
	error = draw_clip(server, window, 1, 0, &area);
	if (error != 0 || area.count == 0U) {
		x11_region_fini(&area);
		return;
	}

	/* The box around what can be seen of it. */
	x11_region_extents(&area, &box);
	x11_region_fini(&area);

	/* Its pixels. */
	frame->saved.pixels = malloc(x11_image_bytes(box.x1 - box.x0, box.y1 - box.y0));
	if (frame->saved.pixels == NULL)
		return;
	frame->saved.width = box.x1 - box.x0;
	frame->saved.height = box.y1 - box.y0;
	frame->saved.stride = (size_t)frame->saved.width;
	frame->saved.depth = X11_DEPTH;
	frame->saved_x = box.x0;
	frame->saved_y = box.y0;
	x11_render_read(&frame->top->image, box.x0, box.y0, frame->saved.width, frame->saved.height, frame->saved.pixels, frame->saved.stride);
}

/* Finds the record of a window in a frame, or NULL. */
static struct x11_draw_record *
draw_frame_find(
	struct x11_draw_frame *frame,
	const struct x11_window *window)
{
	size_t index;

	/* Each record. */
	for (index = 0U; index < frame->count; index++) {
		if (frame->records[index].window == window)
			return &frame->records[index];
	}

	/* None: the window was not seen before the change. */
	return NULL;
}

/*
 * Brings one viewable window of a frame's tree in step after the change:
 * what it showed before and still can is kept (in place, or moved from
 * the kept pixels), and the rest of what it shows now is painted with its
 * border and background and exposed.  Returns nonzero when its pixels
 * changed.
 */
static int
draw_window_after(
	struct x11server *server,
	struct x11_draw_frame *frame,
	struct x11_window *window)
{
	struct x11_draw_record *record;
	struct x11_region inner;
	struct x11_region outline;
	struct x11_region kept;
	struct x11_box saved;
	int moving;
	int retain;
	int dx;
	int dy;
	int changed;
	int error;

	/* What it shows now. */
	x11_region_init(&inner);
	x11_region_init(&outline);
	x11_region_init(&kept);
	error = draw_clip(server, window, 0, 1, &inner);
	if (error == 0)
		error = draw_outline(server, window, &outline);

	/* What it showed before, when its tree had pixels. */
	record = NULL;
	if (frame->top_had_image)
		record = draw_frame_find(frame, window);
	moving = 0;
	if (frame->changing != NULL)
		moving = x11_window_inferior(window, frame->changing);
	changed = 0;

	/*
	 * The inside that is still there: in place where what the window
	 * shows did not move on the pixels (nothing else is written there),
	 * else from the kept pixels.
	 */
	if (error == 0 && record != NULL) {
		draw_content_offset(frame, record, window, &dx, &dy, &retain);
		if (retain && dx == 0 && dy == 0 && !frame->fresh) {
			error = x11_region_copy(&kept, &record->inner);
			if (error == 0)
				error = x11_region_intersect(&kept, &inner);
		} else if (retain && frame->saved.pixels != NULL) {
			error = x11_region_copy(&kept, &record->inner);
			saved.x0 = frame->saved_x;
			saved.y0 = frame->saved_y;
			saved.x1 = frame->saved_x + frame->saved.width;
			saved.y1 = frame->saved_y + frame->saved.height;
			x11_region_intersect_box(&kept, &saved);
			x11_region_translate(&kept, dx, dy);
			if (error == 0)
				error = x11_region_intersect(&kept, &inner);
			if (error == 0)
				error = x11_render_copy(&frame->top->image, &kept, 0, 0, &frame->saved, -dx - frame->saved_x, -dy - frame->saved_y, X11_FUNCTION_COPY, 0xffffffffU);
			if (kept.count != 0U)
				changed = 1;
		}
	}

	/* The border, painted again unless it is just as it was. */
	if (error == 0 && record != NULL && !moving && !frame->fresh && record->border == window->border) {
		if (record->width == window->width && record->height == window->height)
			error = x11_region_subtract(&outline, &record->outline);
	}
	if (error == 0 && outline.count != 0U) {
		draw_paint_border(server, window, &outline);
		changed = 1;
	}

	/* The rest of the inside, painted and exposed (all of it when memory ran out). */
	if (error != 0) {
		x11_region_clear(&kept);
		error = draw_clip(server, window, 0, 1, &inner);
	}
	if (error == 0)
		error = x11_region_subtract(&inner, &kept);
	if (error == 0 && inner.count != 0U) {
		draw_paint_background(server, window, &inner);
		draw_expose(server, window, &inner);
		changed = 1;
	}

	/* Succeeded: done with the window. */
	x11_region_fini(&inner);
	x11_region_fini(&outline);
	x11_region_fini(&kept);
	return changed;
}

/*
 * Gives how far what a window showed moved on its top-level window's
 * pixels: with the window, and, for the window that changed size, by its
 * bit gravity (Forget keeps nothing; Static keeps it where it was on the
 * root window).  *kept is 0 when nothing of it is kept.
 */
static void
draw_content_offset(
	const struct x11_draw_frame *frame,
	const struct x11_draw_record *record,
	const struct x11_window *window,
	int *dx,
	int *dy,
	int *kept)
{
	static const int across[11] = { 0, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 };
	static const int down[11] = { 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 0 };
	int grow_x;
	int grow_y;

	/* With the window. */
	*dx = (window->x - frame->top->x) - record->x;
	*dy = (window->y - frame->top->y) - record->y;
	*kept = 1;
	if (window != frame->changing)
		return;
	if (record->width == window->width && record->height == window->height)
		return;

	/* A resized window by its bit gravity. */
	grow_x = (int)window->width - (int)record->width;
	grow_y = (int)window->height - (int)record->height;
	if (window->bit_gravity == GRAVITY_FORGET) {
		*kept = 0;
		return;
	}
	if (window->bit_gravity == GRAVITY_STATIC) {
		*dx = frame->top_x - frame->top->x;
		*dy = frame->top_y - frame->top->y;
		return;
	}
	*dx += across[window->bit_gravity] * grow_x / 2;
	*dy += down[window->bit_gravity] * grow_y / 2;
}

/*
 * Paints a part of a window (on its top-level window's pixels) with its
 * background: its pixel, its pixmap tiled from its origin, or its
 * parent's (ParentRelative, the parent's origin); None paints nothing.
 */
static void
draw_paint_background(
	struct x11server *server,
	struct x11_window *window,
	const struct x11_region *region)
{
	struct x11_window *owner;
	struct x11_window *top;
	struct x11_paint paint;
	struct x11_box box;

	/* The window whose background it is, up through ParentRelative. */
	owner = window;
	while (owner->background_kind == X11_BACKGROUND_PARENT && owner->parent != NULL && owner->parent != server->root)
		owner = owner->parent;
	if (owner->background_kind == X11_BACKGROUND_NONE)
		return;
	if (owner->background_kind == X11_BACKGROUND_PARENT)
		return;

	/* Its pixel, or its tile from its origin, copied into every plane. */
	top = x11_window_top_level(window);
	memset(&paint, 0, sizeof(paint));
	paint.function = X11_FUNCTION_COPY;
	paint.plane_mask = 0xffffffffU;
	paint.foreground = owner->background;
	paint.fill_style = X11_FILL_SOLID;
	if (owner->background_kind == X11_BACKGROUND_PIXMAP && owner->background_tile != NULL) {
		paint.fill_style = X11_FILL_TILED;
		paint.tile = owner->background_tile;
		paint.origin_x = owner->x - top->x;
		paint.origin_y = owner->y - top->y;
	}

	/* Every box of the part. */
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = top->image.width;
	box.y1 = top->image.height;
	x11_render_boxes(&top->image, region, &paint, &box, 1U);
}

/*
 * Paints a part of a window's border (on its top-level window's pixels):
 * its pixel, or its pixmap tiled from the window's origin.
 */
static void
draw_paint_border(
	struct x11server *server,
	struct x11_window *window,
	const struct x11_region *region)
{
	struct x11_window *top;
	struct x11_paint paint;
	struct x11_box box;

	/* The border's pixel, or its tile, copied into every plane. */
	(void)server;
	top = x11_window_top_level(window);
	memset(&paint, 0, sizeof(paint));
	paint.function = X11_FUNCTION_COPY;
	paint.plane_mask = 0xffffffffU;
	paint.foreground = window->border_pixel;
	paint.fill_style = X11_FILL_SOLID;
	if (window->border_tile != NULL) {
		paint.fill_style = X11_FILL_TILED;
		paint.tile = window->border_tile;
		paint.origin_x = window->x - top->x;
		paint.origin_y = window->y - top->y;
	}

	/* Every box of the part. */
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = top->image.width;
	box.y1 = top->image.height;
	x11_render_boxes(&top->image, region, &paint, &box, 1U);
}

/*
 * Sends Expose for a part of a window (on its top-level window's pixels;
 * the region is moved onto the window and sorted).
 */
static void
draw_expose(
	struct x11server *server,
	struct x11_window *window,
	struct x11_region *region)
{
	struct x11_window *top;

	/* Nothing to tell. */
	if (region->count == 0U)
		return;

	/* On the window, top to bottom. */
	top = x11_window_top_level(window);
	x11_region_translate(region, top->x - window->x, top->y - window->y);
	(void)x11_region_canonical(region);

	/* Told, and the region put back on the pixels. */
	x11_event_expose(server, window, region);
	x11_region_translate(region, window->x - top->x, window->y - top->y);
}

/*
 * Finds what a drawing request draws into: a window (an InputOnly one is
 * BadMatch) with its clip by a subwindow mode, or a pixmap (all of it).
 * Returns 0, BadDrawable, BadMatch or BadAlloc; the target is to be
 * ended with draw_target_fini whatever is returned.
 */
static unsigned
draw_target(
	struct x11server *server,
	uint32_t id,
	unsigned subwindow_mode,
	struct draw_target *target)
{
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	struct x11_window *top;
	struct x11_box box;
	int error;

	/* Nothing yet. */
	memset(target, 0, sizeof(*target));
	x11_region_init(&target->clip);

	/* A window: its top-level window's pixels and its clip (none for the root window). */
	window = x11_window_find(server, id);
	if (window != NULL) {
		if (window->window_class == X11_INPUT_ONLY)
			return X11_BAD_MATCH;
		target->window = window;
		target->width = window->width;
		target->height = window->height;
		target->depth = X11_DEPTH;
		if (window == server->root)
			return 0U;
		top = x11_window_top_level(window);
		if (top->image.pixels == NULL)
			return 0U;
		error = draw_clip(server, window, 0, subwindow_mode == X11_CLIP_BY_CHILDREN, &target->clip);
		if (error != 0)
			return X11_BAD_ALLOC;
		target->image = &top->image;
		target->top = top;
		target->x = window->x - top->x;
		target->y = window->y - top->y;
		return 0U;
	}

	/* A pixmap, all of it. */
	pixmap = x11_pixmap_find(server, id);
	if (pixmap == NULL)
		return X11_BAD_DRAWABLE;
	target->pixmap = pixmap;
	target->image = &pixmap->image;
	target->width = pixmap->width;
	target->height = pixmap->height;
	target->depth = pixmap->depth;
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = pixmap->width;
	box.y1 = pixmap->height;
	error = x11_region_set(&target->clip, &box);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: the pixmap. */
	return 0U;
}

/*
 * Finds what a drawing request draws into with a graphics context: its
 * subwindow mode, its depth (BadMatch for a drawable of another), and its
 * clip mask on the clip origin.  Returns 0 or the error, as draw_target.
 */
static unsigned
draw_target_gc(
	struct x11server *server,
	uint32_t id,
	const struct x11_gc *gc,
	struct draw_target *target)
{
	struct x11_region clip;
	unsigned code;
	int error;

	/* The drawable, of the context's depth. */
	code = draw_target(server, id, gc->subwindow_mode, target);
	if (code != 0U)
		return code;
	if (target->depth != gc->depth)
		return X11_BAD_MATCH;

	/* No clip mask, or nothing to draw on. */
	if (!gc->clip_set || target->image == NULL)
		return 0U;

	/* The clip mask on the drawable from the clip origin, inside the drawable's clip. */
	x11_region_init(&clip);
	error = x11_region_copy(&clip, &gc->clip);
	if (error == 0) {
		x11_region_translate(&clip, target->x + gc->clip_x, target->y + gc->clip_y);
		error = x11_region_intersect(&target->clip, &clip);
	}
	x11_region_fini(&clip);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: clipped by the mask too. */
	return 0U;
}

/* Tells whether an id names a drawable: 0, or BadDrawable. */
static unsigned
draw_exists(
	struct x11server *server,
	uint32_t id)
{
	struct x11_window *window;
	struct x11_pixmap *pixmap;

	/* A window. */
	window = x11_window_find(server, id);
	if (window != NULL)
		return 0U;

	/* A pixmap. */
	pixmap = x11_pixmap_find(server, id);
	if (pixmap != NULL)
		return 0U;

	/* Neither. */
	return X11_BAD_DRAWABLE;
}

/* Ends what finding a target made. */
static void
draw_target_fini(
	struct draw_target *target)
{
	/* The clip's boxes. */
	x11_region_fini(&target->clip);
}

/*
 * Gives the paint of a graphics context on a target: its function, plane
 * mask, pixels and fill style, a missing tile being the foreground and a
 * missing stipple all set, the tile and stipple origin on the image.
 */
static void
draw_paint_of(
	const struct x11_gc *gc,
	const struct draw_target *target,
	struct x11_paint *paint)
{
	/* The function, the planes and the pixels. */
	memset(paint, 0, sizeof(*paint));
	paint->function = gc->function;
	paint->plane_mask = gc->plane_mask;
	paint->foreground = gc->foreground;
	paint->background = gc->background;

	/* The fill style, solid where a pattern is missing. */
	paint->fill_style = gc->fill_style;
	paint->tile = gc->tile;
	paint->stipple = gc->stipple;
	if (gc->fill_style == X11_FILL_TILED && gc->tile == NULL)
		paint->fill_style = X11_FILL_SOLID;
	if (gc->fill_style != X11_FILL_TILED && gc->stipple == NULL)
		paint->fill_style = X11_FILL_SOLID;

	/* The patterns' origin on the image. */
	paint->origin_x = target->x + gc->tile_x;
	paint->origin_y = target->y + gc->tile_y;
}

/*
 * Has the part of a target's pixels a drawing may have changed shown
 * again: a box on the image, inside the clip (nothing for a pixmap).
 */
static void
draw_dirty(
	struct x11server *server,
	const struct draw_target *target,
	const struct x11_box *bounds)
{
	struct x11_box extents;
	struct x11_box part;
	int inside;

	/* A pixmap is not shown. */
	if (target->top == NULL)
		return;

	/* The part inside the clip, on the root window. */
	x11_region_extents(&target->clip, &extents);
	inside = x11_box_intersect(&extents, bounds, &part);
	if (!inside)
		return;

	/* Shown again. */
	x11_mark_dirty(server, target->top->x + part.x0, target->top->y + part.y0, part.x1 - part.x0, part.y1 - part.y0);
}

/* Has all of a top-level window shown again. */
static void
draw_dirty_window(
	struct x11server *server,
	const struct x11_window *top)
{
	/* Its pixels, on the root window. */
	x11_mark_dirty(server, top->x, top->y, top->image.width, top->image.height);
}

/*
 * Tells the client of a copy the parts of the destination its source
 * could not give (on the drawable; GraphicsExpose for each box, the last
 * with count 0), or NoExpose when there are none.
 */
static void
draw_graphics_expose(
	struct x11server *server,
	struct x11_client *client,
	uint32_t drawable,
	struct x11_region *region,
	uint8_t major)
{
	uint8_t event[32];
	size_t index;

	/* Nothing lost: NoExpose. */
	memset(event, 0, sizeof(event));
	if (region->count == 0U) {
		event[0] = EVENT_NO_EXPOSE;
		x11_write32(event + 4, drawable, 0);
		event[10] = major;
		x11_event_send(server, client, event);
		return;
	}

	/* Each box, top to bottom, with how many follow. */
	(void)x11_region_canonical(region);
	for (index = 0U; index < region->count; index++) {
		memset(event, 0, sizeof(event));
		event[0] = EVENT_GRAPHICS_EXPOSE;
		x11_write32(event + 4, drawable, 0);
		x11_write16(event + 8, (uint16_t)region->boxes[index].x0, 0);
		x11_write16(event + 10, (uint16_t)region->boxes[index].y0, 0);
		x11_write16(event + 12, (uint16_t)(region->boxes[index].x1 - region->boxes[index].x0), 0);
		x11_write16(event + 14, (uint16_t)(region->boxes[index].y1 - region->boxes[index].y0), 0);
		x11_write16(event + 18, (uint16_t)(region->count - 1U - index), 0);
		event[20] = major;
		x11_event_send(server, client, event);
	}
}

/*
 * Puts a PutImage's rows onto its target, one row at a time, after
 * checking that the request holds just the image it describes.  Returns
 * 0, BadLength or BadAlloc.
 */
static unsigned
draw_put_rows(
	struct x11server *server,
	struct draw_target *target,
	const struct x11_gc *gc,
	const uint8_t *request,
	size_t length,
	int msb)
{
	const uint8_t *data;
	const uint8_t *line;
	struct x11_box bounds;
	uint32_t *row_pixels;
	uint32_t pixel;
	size_t row_bytes;
	size_t plane_bytes;
	size_t size;
	unsigned format;
	unsigned depth;
	unsigned pad;
	unsigned plane;
	int width;
	int height;
	int x;
	int y;
	int row;
	int column;

	/* The rectangle, the left pad and the depth. */
	format = request[1];
	width = x11_read16(request + 12, msb);
	height = x11_read16(request + 14, msb);
	x = (int16_t)x11_read16(request + 16, msb);
	y = (int16_t)x11_read16(request + 18, msb);
	pad = request[20];
	depth = request[21];

	/* The image's bytes: a bitmap a plane (rows padded to 32 bits), or a word a pixel of depth 24. */
	row_bytes = draw_bitmap_row(pad + (unsigned)width);
	plane_bytes = row_bytes * (size_t)height;
	size = plane_bytes;
	if (format == FORMAT_XY_PIXMAP)
		size = plane_bytes * depth;
	if (format == FORMAT_Z_PIXMAP && depth != 1U) {
		row_bytes = (size_t)width * 4U;
		size = row_bytes * (size_t)height;
	}
	if (24U + ((size + 3U) & ~(size_t)3U) != length)
		return X11_BAD_LENGTH;

	/* Nothing is drawn where nothing can be. */
	if (target->image == NULL || width == 0 || height == 0)
		return 0U;

	/* A row of pixels at a time. */
	row_pixels = malloc((size_t)width * sizeof(*row_pixels));
	if (row_pixels == NULL)
		return X11_BAD_ALLOC;
	data = request + 24;
	for (row = 0; row < height; row++) {
		line = data + (size_t)row * row_bytes;
		for (column = 0; column < width; column++) {
			/* ZPixmap of depth 24: a word in the client's order. */
			if (format == FORMAT_Z_PIXMAP && depth != 1U) {
				row_pixels[column] = x11_read32(line + (size_t)column * 4U, msb) & 0x00ffffffU;
				continue;
			}

			/* A bitmap's bit after the pad: the pixel of depth 1, or the foreground or background. */
			pixel = draw_bit(line, pad + (unsigned)column, msb);
			if (format == FORMAT_XY_BITMAP) {
				row_pixels[column] = gc->background;
				if (pixel != 0U)
					row_pixels[column] = gc->foreground;
				continue;
			}
			if (format == FORMAT_Z_PIXMAP) {
				row_pixels[column] = pixel;
				continue;
			}

			/* XYPixmap: each plane's bit, the most significant plane first. */
			row_pixels[column] = 0U;
			for (plane = 0U; plane < depth; plane++) {
				pixel = draw_bit(data + (size_t)plane * plane_bytes + (size_t)row * row_bytes, pad + (unsigned)column, msb);
				row_pixels[column] |= pixel << (depth - 1U - plane);
			}
		}
		x11_render_put(target->image, &target->clip, target->x + x, target->y + y + row, width, 1, row_pixels, (size_t)width, gc->function, gc->plane_mask);
	}

	/* Succeeded: put, and shown again. */
	free(row_pixels);
	bounds.x0 = target->x + x;
	bounds.y0 = target->y + y;
	bounds.x1 = bounds.x0 + width;
	bounds.y1 = bounds.y0 + height;
	draw_dirty(server, target, &bounds);
	return 0U;
}

/*
 * Reads the bit of a pixel of a bitmap row.  The setup gives the client
 * its own byte order as both the image byte order and the bitmap bit
 * order, so the bytes run left to right and the leftmost pixel of a byte
 * is its most significant bit for a most-significant-first client, its
 * least significant bit for the other.
 */
static uint32_t
draw_bit(
	const uint8_t *row,
	unsigned index,
	int msb)
{
	unsigned bit;

	/* The bit within its byte. */
	bit = index % 8U;
	if (msb)
		bit = 7U - bit;

	/* Its value. */
	return ((uint32_t)row[index / 8U] >> bit) & 1U;
}

/* Sets the bit of a pixel of a bitmap row (the layout draw_bit reads). */
static void
draw_set_bit(
	uint8_t *row,
	unsigned index,
	int msb)
{
	unsigned bit;

	/* The bit within its byte. */
	bit = index % 8U;
	if (msb)
		bit = 7U - bit;

	/* Set. */
	row[index / 8U] |= (uint8_t)(1U << bit);
}

/* Gives the bytes of a bitmap row of a count of bits, padded to 32 bits. */
static size_t
draw_bitmap_row(
	unsigned bits)
{
	/* Whole 32-bit units. */
	return ((size_t)bits + 31U) / 32U * 4U;
}

/* Returns the next window in a walk of a tree that does not go into a window's children, or NULL past the tree. */
static struct x11_window *
draw_next_over(
	struct x11_window *window,
	const struct x11_window *top)
{
	/* The next sibling of the window, or of the nearest ancestor that has one, within the tree. */
	while (window != top) {
		if (window->above != NULL)
			return window->above;
		window = window->parent;
	}

	/* Past the tree. */
	return NULL;
}
/*
 * Copies a rectangle of a request (CopyArea, or CopyPlane when a plane is
 * given) from its source drawable onto its destination, and tells what
 * could not be read.  Returns 0, or the error.
 */
static unsigned
draw_copy(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	uint32_t plane)
{
	struct draw_target source;
	struct x11_paint paint;
	struct draw_target target;
	struct x11_region readable;
	struct x11_region lost;
	struct x11_box rectangle;
	struct x11_box bounds;
	struct x11_gc *gc;
	unsigned code;
	int source_x;
	int source_y;
	int target_x;
	int target_y;
	int width;
	int height;
	int dx;
	int dy;
	int error;
	int msb;

	/* The destination, the context, and the source (of the context's depth for an area). */
	msb = server->clients[index].order;
	code = draw_exists(server, x11_read32(request + 8, msb));
	if (code != 0U)
		return code;
	gc = x11_gc_find(server, x11_read32(request + 12, msb));
	if (gc == NULL)
		return X11_BAD_GC;
	code = draw_target(server, x11_read32(request + 4, msb), gc->subwindow_mode, &source);
	if (code != 0U) {
		draw_target_fini(&source);
		return code;
	}
	code = draw_target_gc(server, x11_read32(request + 8, msb), gc, &target);
	if (code == 0U && plane == 0U && source.depth != target.depth)
		code = X11_BAD_MATCH;
	if (code == 0U && plane != 0U && source.depth < 32U && (plane >> source.depth) != 0U)
		code = X11_BAD_VALUE;
	if (code != 0U) {
		draw_target_fini(&source);
		draw_target_fini(&target);
		return code;
	}

	/* The rectangle, and how far the copy moves a pixel on the images. */
	source_x = (int16_t)x11_read16(request + 16, msb);
	source_y = (int16_t)x11_read16(request + 18, msb);
	target_x = (int16_t)x11_read16(request + 20, msb);
	target_y = (int16_t)x11_read16(request + 22, msb);
	width = x11_read16(request + 24, msb);
	height = x11_read16(request + 26, msb);
	dx = (target.x + target_x) - (source.x + source_x);
	dy = (target.y + target_y) - (source.y + source_y);

	/* What of the source rectangle can be read, on the source's image (nothing when it has none). */
	x11_region_init(&readable);
	x11_region_init(&lost);
	rectangle.x0 = source.x + source_x;
	rectangle.y0 = source.y + source_y;
	rectangle.x1 = rectangle.x0 + width;
	rectangle.y1 = rectangle.y0 + height;
	error = 0;
	if (source.image != NULL) {
		error = x11_region_copy(&readable, &source.clip);
		x11_region_intersect_box(&readable, &rectangle);
	}

	/* Moved onto the destination's image, inside its clip, and copied. */
	x11_region_translate(&readable, dx, dy);
	if (error == 0 && target.image != NULL) {
		error = x11_region_copy(&lost, &readable);
		if (error == 0)
			error = x11_region_intersect(&lost, &target.clip);
		if (error == 0 && plane == 0U)
			error = x11_render_copy(target.image, &lost, 0, 0, source.image, -dx, -dy, gc->function, gc->plane_mask);
		if (error == 0 && plane != 0U) {
			draw_paint_of(gc, &target, &paint);
			error = x11_render_copy_plane(target.image, &lost, 0, 0, source.image, -dx, -dy, plane, &paint);
		}
		x11_region_extents(&lost, &bounds);
		draw_dirty(server, &target, &bounds);
	}

	/* With graphics exposures: the destination rectangle less what could be read, inside the destination's clip. */
	if (error == 0 && gc->graphics_exposures) {
		rectangle.x0 = target.x + target_x;
		rectangle.y0 = target.y + target_y;
		rectangle.x1 = rectangle.x0 + width;
		rectangle.y1 = rectangle.y0 + height;
		error = x11_region_set(&lost, &rectangle);
		if (error == 0)
			error = x11_region_subtract(&lost, &readable);
		if (error == 0)
			error = x11_region_intersect(&lost, &target.clip);
		if (target.image == NULL)
			x11_region_clear(&lost);
		x11_region_translate(&lost, -target.x, -target.y);
		if (error == 0)
			draw_graphics_expose(server, &server->clients[index], x11_read32(request + 8, msb), &lost, request[0]);
	}

	/* Done with the regions and the drawables; a region that found no memory is BadAlloc. */
	x11_region_fini(&readable);
	x11_region_fini(&lost);
	draw_target_fini(&source);
	draw_target_fini(&target);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: copied. */
	return 0U;
}


/*
 * Starts a shape request: its drawable (at byte 4) and context (at byte
 * 8), the paints of its even and odd dashes, and empty lists of spans
 * kept inside the drawable's clip.  Returns 0 (the shape is ended with
 * draw_shape_end), or the error (nothing to end).
 */
static unsigned
draw_shape_begin(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	struct draw_shape *shape)
{
	struct x11_box image;
	struct x11_box extents;
	unsigned code;
	int inside;
	int msb;

	/* The drawable and the context, the drawable of the context's depth. */
	msb = server->clients[index].order;
	memset(shape, 0, sizeof(*shape));
	code = draw_exists(server, x11_read32(request + 4, msb));
	if (code != 0U)
		return code;
	shape->gc = x11_gc_find(server, x11_read32(request + 8, msb));
	if (shape->gc == NULL)
		return X11_BAD_GC;
	code = draw_target_gc(server, x11_read32(request + 4, msb), shape->gc, &shape->target);
	if (code != 0U) {
		draw_target_fini(&shape->target);
		return code;
	}

	/* The spans are kept inside the clip on the image (nothing where nothing is drawn). */
	memset(&extents, 0, sizeof(extents));
	if (shape->target.image != NULL) {
		x11_region_extents(&shape->target.clip, &extents);
		image.x0 = 0;
		image.y0 = 0;
		image.x1 = shape->target.image->width;
		image.y1 = shape->target.image->height;
		inside = x11_box_intersect(&extents, &image, &extents);
		if (!inside)
			memset(&extents, 0, sizeof(extents));
	}
	x11_spans_init(&shape->even, &extents);
	x11_spans_init(&shape->odd, &extents);

	/*
	 * The paint of the even dashes (and of a solid line or a fill), and of
	 * a double dash's odd dashes: the background where a solid or a
	 * stippled fill would take the foreground, otherwise the same.
	 */
	draw_paint_of(shape->gc, &shape->target, &shape->paint);
	shape->odd_paint = shape->paint;
	if (shape->paint.fill_style == X11_FILL_SOLID || shape->paint.fill_style == X11_FILL_STIPPLED)
		shape->odd_paint.foreground = shape->paint.background;

	/* Succeeded: ready to draw. */
	return 0U;
}

/* Ends what starting a shape made, drawing nothing more. */
static void
draw_shape_fini(
	struct draw_shape *shape)
{
	/* The lists and the drawable's clip. */
	x11_spans_fini(&shape->even);
	x11_spans_fini(&shape->odd);
	draw_target_fini(&shape->target);
}

/*
 * Paints the spans a shape has made (each pixel once when they are
 * merged first), keeps the box of what was painted, and empties the
 * lists for the next shape.
 */
static void
draw_shape_paint(
	struct draw_shape *shape,
	int merge)
{
	struct x11_box extents;

	/* A list that ran out of memory leaves the request BadAlloc. */
	if (shape->even.failed || shape->odd.failed)
		shape->failed = 1;

	/* Each pixel once, when the shape asks. */
	if (merge) {
		x11_spans_merge(&shape->even);
		x11_spans_merge(&shape->odd);
	}

	/* The even spans, then the odd ones, painted. */
	if (shape->target.image != NULL) {
		x11_render_spans(shape->target.image, &shape->target.clip, &shape->paint, shape->even.spans, shape->even.count);
		x11_render_spans(shape->target.image, &shape->target.clip, &shape->odd_paint, shape->odd.spans, shape->odd.count);
	}

	/* The box around what was painted. */
	x11_spans_extents(&shape->even, &extents);
	draw_shape_widen(shape, &extents);
	x11_spans_extents(&shape->odd, &extents);
	draw_shape_widen(shape, &extents);

	/* Empty for the next shape. */
	x11_spans_clear(&shape->even);
	x11_spans_clear(&shape->odd);
}

/* Widens the box of what a shape painted by a box (an empty one adds nothing). */
static void
draw_shape_widen(
	struct draw_shape *shape,
	const struct x11_box *box)
{
	/* An empty box. */
	if (box->x0 >= box->x1 || box->y0 >= box->y1)
		return;

	/* The first box painted. */
	if (!shape->painted) {
		shape->bounds = *box;
		shape->painted = 1;
		return;
	}

	/* Around both. */
	if (box->x0 < shape->bounds.x0)
		shape->bounds.x0 = box->x0;
	if (box->y0 < shape->bounds.y0)
		shape->bounds.y0 = box->y0;
	if (box->x1 > shape->bounds.x1)
		shape->bounds.x1 = box->x1;
	if (box->y1 > shape->bounds.y1)
		shape->bounds.y1 = box->y1;
}

/*
 * Ends a shape request: what it painted is shown again.  Returns 0, or
 * BadAlloc when memory ran out on the way (what was painted stays).
 */
static unsigned
draw_shape_end(
	struct x11server *server,
	struct draw_shape *shape)
{
	int failed;

	/* Shown again, and done with the lists. */
	if (shape->painted)
		draw_dirty(server, &shape->target, &shape->bounds);
	failed = shape->failed;
	draw_shape_fini(shape);

	/* Memory ran out. */
	if (failed)
		return X11_BAD_ALLOC;

	/* Succeeded: drawn. */
	return 0U;
}

/*
 * Adds a path through points (on the image) to a shape's lists, as the
 * context draws lines: thin or wide, solid or dashed.  A thin path paints
 * each point once: each line leaves its last point to the next, and the
 * path's last point is not painted with a NotLast cap or when it is the
 * first point again.
 */
static void
draw_shape_path(
	struct draw_shape *shape,
	const int *points,
	size_t count)
{
	struct x11_dash dash;
	struct x11_dash *dashed;
	struct x11_spans *odd;
	const uint8_t *lengths;
	size_t dash_count;
	size_t at;
	unsigned cap;
	int closed;
	int last;

	/* The dashes start at the offset; a double dash draws its odd dashes too. */
	dashed = NULL;
	odd = NULL;
	if (shape->gc->line_style != LINE_SOLID) {
		lengths = x11_gc_dashes(shape->gc, &dash_count);
		x11_dash_start(&dash, lengths, dash_count, shape->gc->dash_offset);
		dashed = &dash;
	}
	if (shape->gc->line_style == LINE_DOUBLE_DASH)
		odd = &shape->odd;

	/* A wide path, its NotLast cap a butt one. */
	cap = shape->gc->cap_style;
	if (shape->gc->line_width != 0U) {
		if (cap == X11_CAP_NOT_LAST)
			cap = X11_CAP_BUTT;
		x11_shape_wide_path(&shape->even, odd, dashed, points, count, shape->gc->line_width, cap, shape->gc->join_style);
		return;
	}

	/* A thin path of one point is that point. */
	if (count == 1U) {
		x11_shape_thin_line(&shape->even, odd, dashed, points[0], points[1], points[0], points[1], cap != X11_CAP_NOT_LAST);
		return;
	}

	/* A thin path that ends where it began closes on its first point. */
	closed = 0;
	if (count > 2U &&
	    points[0] == points[count * 2U - 2U] &&
	    points[1] == points[count * 2U - 1U])
		closed = 1;

	/* Each line, the last one ending on its last point unless that is not painted. */
	for (at = 0U; at + 1U < count; at++) {
		last = 0;
		if (at + 2U == count && cap != X11_CAP_NOT_LAST && !closed)
			last = 1;
		x11_shape_thin_line(&shape->even, odd, dashed, points[at * 2U], points[at * 2U + 1U], points[at * 2U + 2U], points[at * 2U + 3U], last);
	}
}

/*
 * Reads a request's points (relative to the one before when asked) onto
 * a shape's image, as x, y pairs (malloc'd).  Returns them, or NULL when
 * there are none or no memory (the shape is marked failed).
 */
static int *
draw_points(
	struct draw_shape *shape,
	const uint8_t *bytes,
	size_t count,
	int relative,
	int msb)
{
	int *points;
	size_t at;
	int last_x;
	int last_y;
	int x;
	int y;

	/* No points. */
	if (count == 0U)
		return NULL;

	/* Room for them. */
	points = malloc(count * 2U * sizeof(*points));
	if (points == NULL) {
		shape->failed = 1;
		return NULL;
	}

	/* Each point, a relative one added to the one before. */
	last_x = 0;
	last_y = 0;
	for (at = 0U; at < count; at++) {
		x = (int16_t)x11_read16(bytes + at * 4U, msb);
		y = (int16_t)x11_read16(bytes + at * 4U + 2U, msb);
		if (at != 0U && relative) {
			x = draw_far(x + last_x);
			y = draw_far(y + last_y);
		}
		points[at * 2U] = shape->target.x + x;
		points[at * 2U + 1U] = shape->target.y + y;
		last_x = x;
		last_y = y;
	}

	/* Succeeded: the points. */
	return points;
}

/*
 * Tells whether a line's spans are merged before they are painted, so
 * that each pixel is painted once: a wide line is one shape, except that
 * the dashes of an on-off dash with projecting caps are each a shape of
 * their own (where their caps overlap, the pixels are painted again, as
 * X.Org's do).  A thin line paints its pixels as it visits them.
 */
static int
draw_line_merged(
	const struct x11_gc *gc)
{
	/* A thin line. */
	if (gc->line_width == 0U)
		return 0;

	/* Dashes capped past their ends, each its own shape. */
	if (gc->line_style == LINE_ON_OFF_DASH && gc->cap_style == X11_CAP_PROJECTING)
		return 0;

	/* One shape. */
	return 1;
}

/* Returns a coordinate walked by relative steps, kept from going so far that it would overflow. */
static int
draw_far(
	int value)
{
	/* As far as DRAW_FAR either way. */
	if (value > DRAW_FAR)
		return DRAW_FAR;
	if (value < -DRAW_FAR)
		return -DRAW_FAR;

	/* Near enough. */
	return value;
}

/* Reads one arc of a request (x, y, width, height, two angles) onto a shape's image. */
static void
draw_arc_of(
	const struct draw_shape *shape,
	const uint8_t *bytes,
	int msb,
	struct x11_arc *arc)
{
	/* The box of the ellipse on the image, and the angles. */
	arc->x = shape->target.x + (int16_t)x11_read16(bytes, msb);
	arc->y = shape->target.y + (int16_t)x11_read16(bytes + 2, msb);
	arc->width = x11_read16(bytes + 4, msb);
	arc->height = x11_read16(bytes + 6, msb);
	arc->angle1 = (int16_t)x11_read16(bytes + 8, msb);
	arc->angle2 = (int16_t)x11_read16(bytes + 10, msb);
}
