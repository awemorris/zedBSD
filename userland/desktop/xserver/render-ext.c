/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RENDER extension (version 0.11): pictures, glyph sets and the
 * requests that composite them.
 *
 * A picture is a drawable seen in one of the screen's picture formats
 * (the depth names the format: a1, a8, x8r8g8b8 or a8r8g8b8), or a
 * source of its own (one colour, or a gradient).  A picture of a pixmap
 * holds the pixmap while it lasts; a picture of a window goes with the
 * window.  Each request resolves its pictures into what the drawing layer
 * reads (render.h's x11_source) and the target's image and clip, and has
 * the layer composite them.  Trapezoids and triangles are first made
 * into an alpha mask (coverage.c), glyphs are composited from the images
 * their glyph set keeps.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The version of RENDER this is. */
#define RENDER_VERSION_MAJOR		0U
#define RENDER_VERSION_MINOR		11U

/* The minor opcodes. */
#define RENDER_QUERY_VERSION		0U
#define RENDER_QUERY_PICT_FORMATS	1U
#define RENDER_QUERY_PICT_INDEX_VALUES	2U
#define RENDER_CREATE_PICTURE		4U
#define RENDER_CHANGE_PICTURE		5U
#define RENDER_SET_PICTURE_CLIP_RECTANGLES	6U
#define RENDER_FREE_PICTURE		7U
#define RENDER_COMPOSITE		8U
#define RENDER_TRAPEZOIDS		10U
#define RENDER_TRIANGLES		11U
#define RENDER_TRI_STRIP		12U
#define RENDER_TRI_FAN			13U
#define RENDER_CREATE_GLYPH_SET		17U
#define RENDER_REFERENCE_GLYPH_SET	18U
#define RENDER_FREE_GLYPH_SET		19U
#define RENDER_ADD_GLYPHS		20U
#define RENDER_FREE_GLYPHS		22U
#define RENDER_COMPOSITE_GLYPHS8	23U
#define RENDER_COMPOSITE_GLYPHS16	24U
#define RENDER_COMPOSITE_GLYPHS32	25U
#define RENDER_FILL_RECTANGLES		26U
#define RENDER_CREATE_CURSOR		27U
#define RENDER_SET_PICTURE_TRANSFORM	28U
#define RENDER_QUERY_FILTERS		29U
#define RENDER_SET_PICTURE_FILTER	30U
#define RENDER_CREATE_ANIM_CURSOR	31U
#define RENDER_ADD_TRAPS		32U
#define RENDER_CREATE_SOLID_FILL	33U
#define RENDER_CREATE_LINEAR_GRADIENT	34U
#define RENDER_CREATE_RADIAL_GRADIENT	35U
#define RENDER_CREATE_CONICAL_GRADIENT	36U

/* The extension's errors. */
#define RENDER_BAD_PICT_FORMAT		(X11_RENDER_ERROR + 0U)
#define RENDER_BAD_PICTURE		(X11_RENDER_ERROR + 1U)
#define RENDER_BAD_GLYPH_SET		(X11_RENDER_ERROR + 3U)
#define RENDER_BAD_GLYPH		(X11_RENDER_ERROR + 4U)

/*
 * The screen's picture formats, by depth, at ids no client chooses: a1,
 * a8, x8r8g8b8 and a8r8g8b8.
 */
#define RENDER_FORMAT_A1		0x00000120U
#define RENDER_FORMAT_A8		0x00000121U
#define RENDER_FORMAT_X8R8G8B8		0x00000122U
#define RENDER_FORMAT_A8R8G8B8		0x00000123U
#define RENDER_FORMATS			4U

/* A format's type: its channels are direct (bits of the pixel). */
#define RENDER_FORMAT_DIRECT		1U

/* The picture attributes of CreatePicture and ChangePicture, by their bits. */
#define PICTURE_REPEAT			0U
#define PICTURE_ALPHA_MAP		1U
#define PICTURE_ALPHA_X			2U
#define PICTURE_ALPHA_Y			3U
#define PICTURE_CLIP_X			4U
#define PICTURE_CLIP_Y			5U
#define PICTURE_CLIP_MASK		6U
#define PICTURE_GRAPHICS_EXPOSURES	7U
#define PICTURE_SUBWINDOW_MODE		8U
#define PICTURE_POLY_EDGE		9U
#define PICTURE_POLY_MODE		10U
#define PICTURE_DITHER			11U
#define PICTURE_COMPONENT_ALPHA		12U
#define PICTURE_ATTRIBUTES		13U

/* The edges of a shape drawn without a mask format: sharp (a1) or smooth (a8). */
#define POLY_EDGE_SHARP			0U

/* The largest kernel a convolution takes (each way). */
#define RENDER_KERNEL_MAX		64

/* The filters QueryFilters names: the three filters, then the aliases of the first two. */
#define RENDER_FILTER_NAMES		6U
#define RENDER_FILTER_ALIAS_NONE	0xffffU

/* A glyph set's table starts with this many slots (a power of two) and keeps under half full. */
#define GLYPHS_FIRST_CAPACITY		64U

/*
 * A picture: what it draws from or into and how.
 *
 * A picture of a drawable names a window or a pixmap (a pixmap is held
 * while the picture lasts) and has the format of its depth; a source
 * picture has none and is one colour or a gradient.  The attributes are
 * those of CreatePicture and ChangePicture: the repeat, the alpha map
 * (kept, not drawn with), the client clip (a region on the clip origin,
 * clip_set 0: none), the subwindow mode, the edges of shapes drawn
 * without a mask format, component alpha.  SetPictureFilter and
 * SetPictureTransform give the filter (with a convolution's kernel) and
 * the transform.  A gradient keeps its geometry and stops.  The owner is
 * charged for what the picture keeps.
 */
struct x11_picture {
	uint32_t id;
	unsigned owner;
	struct x11_picture *next;
	unsigned kind;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	unsigned depth;
	uint32_t format;
	unsigned repeat;
	uint32_t alpha_map;
	int16_t alpha_x;
	int16_t alpha_y;
	int16_t clip_x;
	int16_t clip_y;
	int clip_set;
	struct x11_region clip;
	uint8_t graphics_exposures;
	uint8_t subwindow_mode;
	uint8_t poly_edge;
	uint8_t poly_mode;
	uint32_t dither;
	uint8_t component_alpha;
	unsigned filter;
	int32_t *kernel;
	int kernel_width;
	int kernel_height;
	int transformed;
	int32_t transform[9];
	struct x11_color color;
	int32_t points[4];
	int32_t radii[2];
	int32_t angle;
	struct x11_gradient_stop *stops;
	size_t stop_count;
	size_t charged;
};

/*
 * The attributes CreatePicture or ChangePicture gives, read and checked
 * before any is kept: the mask of those given, their values, and the
 * region of a clip mask pixmap (clip_given: a clip mask or None was
 * given; clip_none: it was None).
 */
struct render_values {
	uint32_t mask;
	uint32_t value[PICTURE_ATTRIBUTES];
	int clip_given;
	int clip_none;
	struct x11_region clip;
};

/*
 * One glyph of a glyph set: its id, its image (of the set's depth) and
 * its metrics: the image's size, where the glyph's origin is in it (x,
 * y from the image's top left), and how far the pen moves after it.
 */
struct render_glyph {
	uint32_t id;
	int16_t x;
	int16_t y;
	int16_t x_off;
	int16_t y_off;
	struct x11_image image;
};

/*
 * A glyph set: glyphs of one format (its depth), kept in an
 * open-addressing table of a power of two slots by id.  Each id that
 * names it (CreateGlyphSet's and ReferenceGlyphSet's) is one reference;
 * it goes with the last.  The owner is charged for the glyphs (the server
 * once the owner has gone and others still name it).
 */
struct render_glyphset {
	unsigned references;
	unsigned owner;
	unsigned depth;
	uint32_t format;
	struct render_glyph **slots;
	size_t capacity;
	size_t count;
	size_t charged;
};

/*
 * One picture as a composite reads it: what the layer reads, the view of
 * its drawable (when it has one) and the picture's client clip (NULL:
 * none) with its origin.
 */
struct render_operand {
	struct x11_source source;
	struct x11_draw_view view;
	int viewed;
	const struct x11_region *clip;
	int clip_x;
	int clip_y;
};

static unsigned render_query_version(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned render_query_formats(struct x11server *server, struct x11_client *client, size_t length);
static unsigned render_query_index_values(struct x11_client *client, const uint8_t *request, size_t length);
static unsigned render_create_picture(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_change_picture(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_set_clip_rectangles(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_free_picture(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_composite(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_fill_rectangles(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_trapezoids(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_triangles(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_add_traps(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_create_glyph_set(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_reference_glyph_set(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_free_glyph_set(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_add_glyphs(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_free_glyphs(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_composite_glyphs(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_set_transform(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_create_cursor(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_create_anim_cursor(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_query_filters(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned render_set_filter(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_create_source(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned render_new_picture(struct x11server *server, unsigned index, uint32_t id, struct x11_picture **made);
static unsigned render_values_read(struct x11server *server, unsigned index, uint32_t mask, const uint8_t *values, struct render_values *read);
static void render_values_apply(struct x11_picture *picture, struct render_values *values);
static struct x11_picture *render_picture_find(struct x11server *server, uint32_t id);
static void render_picture_free(struct x11server *server, struct x11_picture *picture);
static unsigned render_format_depth(uint32_t format);
static int render_op_valid(unsigned op);
static unsigned render_operand_of(struct x11server *server, struct x11_picture *picture, struct render_operand *operand);
static void render_operand_image(struct render_operand *operand, const struct x11_image *image, int component_alpha);
static void render_operand_fini(struct render_operand *operand);
static void render_draw(struct x11server *server, unsigned op, struct x11_picture *target, struct render_operand *source, int source_x, int source_y, struct render_operand *mask, int mask_x, int mask_y, int x, int y, int width, int height);
static int render_image_make(struct x11_image *image, int width, int height, unsigned depth);
static int render_mask_make(struct x11server *server, struct x11_image *image, int width, int height, unsigned depth);
static void render_mask_free(struct x11server *server, struct x11_image *image);
static void render_drawable_box(const struct x11_picture *picture, struct x11_box *box);
static void render_shapes(struct x11server *server, unsigned op, struct x11_picture *source, struct x11_picture *target, uint32_t mask_format, int source_dx, int source_dy, const struct x11_trapezoid *trapezoids, size_t count, size_t per_shape);
static void render_trapezoid_read(const uint8_t *bytes, int msb, struct x11_trapezoid *trapezoid);
static void render_triangle_trapezoids(const int32_t *points, struct x11_trapezoid *trapezoids);
static void render_trapezoid_bounds(const struct x11_trapezoid *trapezoid, struct x11_box *bounds);
static int render_fixed_int(int64_t value);
static int render_fixed_int_ceil(int64_t value);
static struct render_glyphset *render_glyphset_find(struct x11server *server, uint32_t id);
static void render_glyphset_drop(struct x11server *server, struct render_glyphset *set);
static struct render_glyph *render_glyph_find(const struct render_glyphset *set, uint32_t id);
static int render_glyph_put(struct render_glyphset *set, struct render_glyph *glyph);
static void render_glyph_remove(struct render_glyphset *set, uint32_t id);
static size_t render_glyph_slot(const struct render_glyphset *set, uint32_t id);
static size_t render_glyph_bytes(const struct render_glyph *glyph);
static size_t render_row_bytes(unsigned depth, unsigned width);

/*
 * Answers a request of the RENDER extension by its minor opcode.
 * Returns 0, or the error to send.
 */
unsigned
x11_render_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* Every request has its header. */
	client = &server->clients[index];
	if (length < 4U)
		return X11_BAD_LENGTH;

	/* The handler of the minor opcode. */
	switch (request[1]) {
	case RENDER_QUERY_VERSION:
		code = render_query_version(server, client, request, length);
		break;
	case RENDER_QUERY_PICT_FORMATS:
		code = render_query_formats(server, client, length);
		break;
	case RENDER_QUERY_PICT_INDEX_VALUES:
		code = render_query_index_values(client, request, length);
		break;
	case RENDER_CREATE_PICTURE:
		code = render_create_picture(server, index, request, length);
		break;
	case RENDER_CHANGE_PICTURE:
		code = render_change_picture(server, index, request, length);
		break;
	case RENDER_SET_PICTURE_CLIP_RECTANGLES:
		code = render_set_clip_rectangles(server, index, request, length);
		break;
	case RENDER_FREE_PICTURE:
		code = render_free_picture(server, index, request, length);
		break;
	case RENDER_COMPOSITE:
		code = render_composite(server, index, request, length);
		break;
	case RENDER_TRAPEZOIDS:
		code = render_trapezoids(server, index, request, length);
		break;
	case RENDER_TRIANGLES:
	case RENDER_TRI_STRIP:
	case RENDER_TRI_FAN:
		code = render_triangles(server, index, request, length);
		break;
	case RENDER_CREATE_GLYPH_SET:
		code = render_create_glyph_set(server, index, request, length);
		break;
	case RENDER_REFERENCE_GLYPH_SET:
		code = render_reference_glyph_set(server, index, request, length);
		break;
	case RENDER_FREE_GLYPH_SET:
		code = render_free_glyph_set(server, index, request, length);
		break;
	case RENDER_ADD_GLYPHS:
		code = render_add_glyphs(server, index, request, length);
		break;
	case RENDER_FREE_GLYPHS:
		code = render_free_glyphs(server, index, request, length);
		break;
	case RENDER_COMPOSITE_GLYPHS8:
	case RENDER_COMPOSITE_GLYPHS16:
	case RENDER_COMPOSITE_GLYPHS32:
		code = render_composite_glyphs(server, index, request, length);
		break;
	case RENDER_FILL_RECTANGLES:
		code = render_fill_rectangles(server, index, request, length);
		break;
	case RENDER_SET_PICTURE_TRANSFORM:
		code = render_set_transform(server, index, request, length);
		break;
	case RENDER_QUERY_FILTERS:
		code = render_query_filters(server, client, request, length);
		break;
	case RENDER_SET_PICTURE_FILTER:
		code = render_set_filter(server, index, request, length);
		break;
	case RENDER_ADD_TRAPS:
		code = render_add_traps(server, index, request, length);
		break;
	case RENDER_CREATE_CURSOR:
		code = render_create_cursor(server, index, request, length);
		break;
	case RENDER_CREATE_ANIM_CURSOR:
		code = render_create_anim_cursor(server, index, request, length);
		break;
	case RENDER_CREATE_SOLID_FILL:
	case RENDER_CREATE_LINEAR_GRADIENT:
	case RENDER_CREATE_RADIAL_GRADIENT:
	case RENDER_CREATE_CONICAL_GRADIENT:
		code = render_create_source(server, index, request, length);
		break;
	default:
		/* The requests RENDER no longer has (Scale, the coloured shapes, AddGlyphsFromPicture, QueryDithers). */
		code = X11_BAD_IMPLEMENTATION;
		break;
	}

	/* Answered, or the error to send. */
	return code;
}

/*
 * Frees the pictures of a window that is being destroyed (their ids go
 * with them).
 */
void
x11_render_forget_window(
	struct x11server *server,
	const struct x11_window *window)
{
	struct x11_picture **link;
	struct x11_picture *picture;

	/* Each picture of the window. */
	link = &server->pictures;
	while (*link != NULL) {
		picture = *link;
		if (picture->window != window) {
			link = &picture->next;
			continue;
		}
		*link = picture->next;
		picture->next = NULL;
		picture->window = NULL;
		x11_resource_remove(server, picture->id);
		render_picture_free(server, picture);
	}
}

/*
 * Frees what a client that goes holds of RENDER: its pictures, and its
 * names of glyph sets (a set others still name stays, charged to the
 * server).
 */
void
x11_render_release(
	struct x11server *server,
	unsigned owner)
{
	struct x11_picture *picture;
	struct render_glyphset *set;
	uint32_t *ids;
	size_t count;
	size_t at;
	unsigned client;
	int error;

	/* Its pictures. */
	error = x11_resource_owned(server, owner, X11_RESOURCE_PICTURE, &ids, &count);
	if (error == 0) {
		for (at = 0U; at < count; at++) {
			picture = x11_resource_find(server, ids[at], X11_RESOURCE_PICTURE);
			x11_resource_remove(server, ids[at]);
			render_picture_free(server, picture);
		}
		free(ids);
	}

	/* Its names of glyph sets. */
	error = x11_resource_owned(server, owner, X11_RESOURCE_GLYPHSET, &ids, &count);
	if (error == 0) {
		for (at = 0U; at < count; at++) {
			set = x11_resource_find(server, ids[at], X11_RESOURCE_GLYPHSET);
			x11_resource_remove(server, ids[at]);
			render_glyphset_drop(server, set);
		}
		free(ids);
	}

	/* The sets it made that other clients still name: their charge becomes the server's. */
	for (client = 0U; client < server->clients_max; client++) {
		error = x11_resource_owned(server, client, X11_RESOURCE_GLYPHSET, &ids, &count);
		if (error != 0)
			continue;
		for (at = 0U; at < count; at++) {
			set = x11_resource_find(server, ids[at], X11_RESOURCE_GLYPHSET);
			if (set->owner != owner)
				continue;
			x11_memory_release(server, owner, set->charged);
			(void)x11_memory_charge(server, X11_NO_CLIENT, set->charged);
			set->owner = X11_NO_CLIENT;
		}
		free(ids);
	}
}

/*
 * Sets a picture's client clip to a region (NULL: none) on a clip
 * origin, for XFIXES's SetPictureClipRegion.  Returns 0, BadPicture for
 * an id that is no picture (or a picture of its own, which has no clip),
 * or BadAlloc.
 */
unsigned
x11_render_picture_clip(
	struct x11server *server,
	uint32_t id,
	const struct x11_region *region,
	int x,
	int y)
{
	struct x11_picture *picture;
	struct x11_region clip;
	int error;

	/* A picture of a drawable. */
	picture = render_picture_find(server, id);
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	if (picture->kind != X11_SOURCE_IMAGE)
		return RENDER_BAD_PICTURE;

	/* The region's copy. */
	x11_region_init(&clip);
	if (region != NULL) {
		error = x11_region_copy(&clip, region);
		if (error != 0) {
			x11_region_fini(&clip);
			return X11_BAD_ALLOC;
		}
	}

	/* Succeeded: it replaces the clip there was. */
	x11_region_fini(&picture->clip);
	picture->clip = clip;
	picture->clip_set = 0;
	if (region != NULL)
		picture->clip_set = 1;
	picture->clip_x = (int16_t)x;
	picture->clip_y = (int16_t)y;
	return 0U;
}

/*
 * Reads a picture of a drawable as a8r8g8b8 pixels (premultiplied), all
 * of its drawable, for RENDER's CreateCursor: the picture composited with
 * Src onto pixels of its size (a window's that cannot be seen are all
 * clear).  Returns 0 with the pixels (malloc'd, width by height, rows of
 * the width), BadPicture, BadMatch for a picture of its own (a colour or
 * a gradient has no size), or BadAlloc.
 */
unsigned
x11_render_picture_pixels(
	struct x11server *server,
	uint32_t id,
	uint32_t **pixels,
	int *width,
	int *height)
{
	struct render_operand source;
	struct x11_picture *picture;
	struct x11_region all;
	struct x11_image image;
	struct x11_box box;
	unsigned code;
	int error;

	/* A picture of a drawable. */
	picture = render_picture_find(server, id);
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	if (picture->kind != X11_SOURCE_IMAGE)
		return X11_BAD_MATCH;

	/* The drawable as the layer reads it. */
	code = render_operand_of(server, picture, &source);
	if (code != 0U) {
		render_operand_fini(&source);
		return code;
	}

	/* Pixels of its size, all clear. */
	error = render_image_make(&image, source.view.width, source.view.height, X11_DEPTH_ARGB);
	if (error != 0) {
		render_operand_fini(&source);
		return X11_BAD_ALLOC;
	}

	/* The picture copied onto them (its pixels up to date in memory first). */
	if (source.source.image != NULL)
		x11_render_image_memory(source.source.image);
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = image.width;
	box.y1 = image.height;
	x11_region_init(&all);
	error = x11_region_set(&all, &box);
	if (error == 0)
		x11_render_composite(&image, &all, X11_OP_SRC, &source.source, 0, 0, NULL, 0, 0, 0, 0, image.width, image.height);
	x11_region_fini(&all);
	render_operand_fini(&source);
	if (error != 0) {
		free(image.pixels);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the pixels are the caller's. */
	*pixels = image.pixels;
	*width = image.width;
	*height = image.height;
	return 0U;
}

/* QueryVersion: the version both the client and the server know (the lower of the two). */
static unsigned
render_query_version(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint32_t major;
	uint32_t minor;

	/* The client's version. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	major = x11_read32(request + 4, client->order);
	minor = x11_read32(request + 8, client->order);

	/* The lower of the two. */
	if (major > RENDER_VERSION_MAJOR || (major == RENDER_VERSION_MAJOR && minor > RENDER_VERSION_MINOR)) {
		major = RENDER_VERSION_MAJOR;
		minor = RENDER_VERSION_MINOR;
	}

	/* Succeeded: answered. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, major, client->order);
	x11_write32(reply + 12, minor, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * QueryPictFormats: the four formats, the one screen's depths with the
 * format of each visual (x8r8g8b8 for the screen's, a8r8g8b8 for the ARGB
 * one), and the screen's subpixel order (unknown).
 */
static unsigned
render_query_formats(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	static const uint32_t ids[RENDER_FORMATS] = { RENDER_FORMAT_A1, RENDER_FORMAT_A8, RENDER_FORMAT_X8R8G8B8, RENDER_FORMAT_A8R8G8B8 };
	static const uint8_t depths[RENDER_FORMATS] = { 1U, 8U, 24U, 32U };
	uint8_t reply[32U + RENDER_FORMATS * 28U + 8U + 4U * 8U + 2U * 8U + 4U];
	uint8_t *field;
	unsigned at;
	int msb;

	/* The header alone. */
	if (length != 4U)
		return X11_BAD_LENGTH;
	msb = client->order;
	memset(reply, 0, sizeof(reply));

	/* The counts: formats, screens, depths, visuals, subpixel orders. */
	x11_write32(reply + 8, RENDER_FORMATS, msb);
	x11_write32(reply + 12, 1U, msb);
	x11_write32(reply + 16, 4U, msb);
	x11_write32(reply + 20, 2U, msb);
	x11_write32(reply + 24, 1U, msb);

	/* Each format: its id, direct, its depth, and its channels (shift and mask of red, green, blue, alpha). */
	field = reply + 32;
	for (at = 0U; at < RENDER_FORMATS; at++) {
		x11_write32(field, ids[at], msb);
		field[4] = RENDER_FORMAT_DIRECT;
		field[5] = depths[at];
		if (depths[at] == 24U || depths[at] == 32U) {
			x11_write16(field + 8, 16U, msb);
			x11_write16(field + 10, 0xffU, msb);
			x11_write16(field + 12, 8U, msb);
			x11_write16(field + 14, 0xffU, msb);
			x11_write16(field + 16, 0U, msb);
			x11_write16(field + 18, 0xffU, msb);
		}
		if (depths[at] == 32U) {
			x11_write16(field + 20, 24U, msb);
			x11_write16(field + 22, 0xffU, msb);
		}
		if (depths[at] == 8U)
			x11_write16(field + 22, 0xffU, msb);
		if (depths[at] == 1U)
			x11_write16(field + 22, 1U, msb);
		field += 28;
	}

	/* The screen: four depths, its fallback format the screen visual's. */
	x11_write32(field, 4U, msb);
	x11_write32(field + 4, RENDER_FORMAT_X8R8G8B8, msb);
	field += 8;

	/* Depth 24 with the screen's visual. */
	field[0] = X11_DEPTH;
	x11_write16(field + 2, 1U, msb);
	x11_write32(field + 8, X11_VISUAL_XID, msb);
	x11_write32(field + 12, RENDER_FORMAT_X8R8G8B8, msb);
	field += 16;

	/* Depth 1 and depth 8 with no visual. */
	field[0] = 1U;
	field += 8;
	field[0] = X11_DEPTH_ALPHA;
	field += 8;

	/* Depth 32 with the ARGB visual. */
	field[0] = X11_DEPTH_ARGB;
	x11_write16(field + 2, 1U, msb);
	x11_write32(field + 8, X11_VISUAL_ARGB_XID, msb);
	x11_write32(field + 12, RENDER_FORMAT_A8R8G8B8, msb);
	field += 16;

	/* Succeeded: the screen's subpixel order is unknown (0). */
	x11_write32(field, 0U, msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* QueryPictIndexValues: no format of the screen is indexed. */
static unsigned
render_query_index_values(
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	unsigned depth;

	/* A format. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	depth = render_format_depth(x11_read32(request + 4, client->order));
	if (depth == 0U)
		return RENDER_BAD_PICT_FORMAT;

	/* Every format is direct. */
	return X11_BAD_MATCH;
}

/*
 * CreatePicture: a picture of a drawable in a format of its depth, with
 * the attributes the values give.
 */
static unsigned
render_create_picture(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_values values;
	struct x11_picture *picture;
	struct x11_window *window;
	struct x11_pixmap *pixmap;
	uint32_t format;
	uint32_t mask;
	unsigned depth;
	unsigned drawable_depth;
	unsigned code;
	int fits;
	int error;
	int msb;

	/* The fixed part and a value for each bit of the mask. */
	msb = server->clients[index].order;
	if (length < 20U)
		return X11_BAD_LENGTH;
	mask = x11_read32(request + 16, msb);
	if ((mask >> PICTURE_ATTRIBUTES) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 20U, length);
	if (!fits)
		return X11_BAD_LENGTH;

	/* The drawable: a window (not InputOnly) or a pixmap. */
	window = x11_window_find(server, x11_read32(request + 8, msb));
	pixmap = NULL;
	if (window == NULL)
		pixmap = x11_pixmap_find(server, x11_read32(request + 8, msb));
	if (window == NULL && pixmap == NULL)
		return X11_BAD_DRAWABLE;
	if (window != NULL && window->window_class == X11_INPUT_ONLY)
		return X11_BAD_MATCH;
	drawable_depth = 0U;
	if (window != NULL)
		drawable_depth = window->depth;
	if (pixmap != NULL)
		drawable_depth = pixmap->depth;

	/* A format of the drawable's depth. */
	format = x11_read32(request + 12, msb);
	depth = render_format_depth(format);
	if (depth == 0U)
		return RENDER_BAD_PICT_FORMAT;
	if (depth != drawable_depth)
		return X11_BAD_MATCH;

	/* The picture, empty, for the values to be checked against. */
	code = render_new_picture(server, index, x11_read32(request + 4, msb), &picture);
	if (code != 0U)
		return code;
	picture->kind = X11_SOURCE_IMAGE;
	picture->window = window;
	picture->pixmap = pixmap;
	picture->depth = depth;
	picture->format = format;

	/* Its values, every one checked before any is kept (the pixmap is not held yet). */
	code = render_values_read(server, index, mask, request + 20, &values);
	if (code != 0U) {
		x11_region_fini(&values.clip);
		picture->pixmap = NULL;
		render_picture_free(server, picture);
		return code;
	}
	render_values_apply(picture, &values);

	/* Found by its id; a pixmap is held, and the drawables' pictures are listed (a window's go with it). */
	error = x11_resource_add(server, picture->id, X11_RESOURCE_PICTURE, picture);
	if (error != 0) {
		picture->pixmap = NULL;
		render_picture_free(server, picture);
		return X11_BAD_ALLOC;
	}
	if (pixmap != NULL)
		x11_pixmap_hold(pixmap);
	picture->next = server->pictures;
	server->pictures = picture;

	/* Succeeded: the picture is made. */
	return 0U;
}

/* ChangePicture: the attributes the values give, every one checked before any is kept. */
static unsigned
render_change_picture(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_values values;
	struct x11_picture *picture;
	uint32_t mask;
	unsigned code;
	int fits;
	int msb;

	/* The picture, and a value for each bit of the mask. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, msb));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	mask = x11_read32(request + 8, msb);
	if ((mask >> PICTURE_ATTRIBUTES) != 0U)
		return X11_BAD_VALUE;
	fits = x11_values_fit(mask, 12U, length);
	if (!fits)
		return X11_BAD_LENGTH;

	/* Read and checked, then kept. */
	code = render_values_read(server, index, mask, request + 12, &values);
	if (code != 0U) {
		x11_region_fini(&values.clip);
		return code;
	}
	render_values_apply(picture, &values);

	/* Succeeded: changed. */
	return 0U;
}

/*
 * SetPictureClipRectangles: the client clip becomes the rectangles (in
 * any order, overlapping or not) on a clip origin.
 */
static unsigned
render_set_clip_rectangles(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_picture *picture;
	struct x11_region clip;
	struct x11_box box;
	const uint8_t *rectangle;
	size_t count;
	size_t at;
	int error;
	int msb;

	/* A picture of a drawable, and whole rectangles. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 8U != 0U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, msb));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	if (picture->kind != X11_SOURCE_IMAGE)
		return RENDER_BAD_PICTURE;

	/* The rectangles' union. */
	x11_region_init(&clip);
	count = (length - 12U) / 8U;
	error = 0;
	for (at = 0U; at < count && error == 0; at++) {
		rectangle = request + 12U + at * 8U;
		box.x0 = (int16_t)x11_read16(rectangle, msb);
		box.y0 = (int16_t)x11_read16(rectangle + 2, msb);
		box.x1 = box.x0 + (int)x11_read16(rectangle + 4, msb);
		box.y1 = box.y0 + (int)x11_read16(rectangle + 6, msb);
		if (box.x1 > box.x0 && box.y1 > box.y0)
			error = x11_region_union_box(&clip, &box);
	}
	if (error != 0) {
		x11_region_fini(&clip);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: the clip and its origin replace the old. */
	x11_region_fini(&picture->clip);
	picture->clip = clip;
	picture->clip_set = 1;
	picture->clip_x = (int16_t)x11_read16(request + 8, msb);
	picture->clip_y = (int16_t)x11_read16(request + 10, msb);
	return 0U;
}

/* FreePicture: the picture and its id go (a pixmap it held may go with it). */
static unsigned
render_free_picture(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_picture *picture;

	/* The picture. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, server->clients[index].order));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;

	/* Succeeded: its id and it go. */
	x11_resource_remove(server, picture->id);
	render_picture_free(server, picture);
	return 0U;
}

/*
 * Composite: a rectangle of the destination picture joined with the
 * source (through the mask, when there is one) by an operator.
 */
static unsigned
render_composite(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_operand source;
	struct render_operand mask;
	struct render_operand *mask_operand;
	struct x11_picture *source_picture;
	struct x11_picture *mask_picture;
	struct x11_picture *target;
	uint32_t mask_id;
	unsigned op;
	unsigned code;
	int source_x;
	int source_y;
	int mask_x;
	int mask_y;
	int x;
	int y;
	int width;
	int height;
	int valid;
	int msb;

	/* The operator and the three pictures (the mask may be None, the destination is a drawable's). */
	msb = server->clients[index].order;
	if (length != 36U)
		return X11_BAD_LENGTH;
	op = request[4];
	valid = render_op_valid(op);
	if (!valid)
		return X11_BAD_VALUE;
	source_picture = render_picture_find(server, x11_read32(request + 8, msb));
	if (source_picture == NULL)
		return RENDER_BAD_PICTURE;
	mask_id = x11_read32(request + 12, msb);
	mask_picture = NULL;
	if (mask_id != 0U) {
		mask_picture = render_picture_find(server, mask_id);
		if (mask_picture == NULL)
			return RENDER_BAD_PICTURE;
	}
	target = render_picture_find(server, x11_read32(request + 16, msb));
	if (target == NULL)
		return RENDER_BAD_PICTURE;
	if (target->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;

	/* The source and mask as the layer reads them. */
	code = render_operand_of(server, source_picture, &source);
	if (code != 0U) {
		render_operand_fini(&source);
		return code;
	}
	if (mask_picture != NULL) {
		code = render_operand_of(server, mask_picture, &mask);
		if (code != 0U) {
			render_operand_fini(&mask);
			render_operand_fini(&source);
			return code;
		}
	}

	/* The places of the source, the mask and the destination, and the size. */
	source_x = (int16_t)x11_read16(request + 20, msb);
	source_y = (int16_t)x11_read16(request + 22, msb);
	mask_x = (int16_t)x11_read16(request + 24, msb);
	mask_y = (int16_t)x11_read16(request + 26, msb);
	x = (int16_t)x11_read16(request + 28, msb);
	y = (int16_t)x11_read16(request + 30, msb);
	width = x11_read16(request + 32, msb);
	height = x11_read16(request + 34, msb);

	/* Drawn, through the mask when there is one. */
	mask_operand = NULL;
	if (mask_picture != NULL)
		mask_operand = &mask;
	render_draw(server, op, target, &source, source_x, source_y, mask_operand, mask_x, mask_y, x, y, width, height);

	/* Succeeded: what the operands held goes. */
	if (mask_picture != NULL)
		render_operand_fini(&mask);
	render_operand_fini(&source);
	return 0U;
}

/*
 * FillRectangles: each rectangle of the destination joined with one
 * colour (premultiplied) by an operator, one after another.
 */
static unsigned
render_fill_rectangles(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_operand source;
	struct x11_picture *target;
	const uint8_t *rectangle;
	size_t count;
	size_t at;
	unsigned op;
	int x;
	int y;
	int width;
	int height;
	int valid;
	int msb;

	/* The operator, the destination, the colour and whole rectangles. */
	msb = server->clients[index].order;
	if (length < 20U || (length - 20U) % 8U != 0U)
		return X11_BAD_LENGTH;
	op = request[4];
	valid = render_op_valid(op);
	if (!valid)
		return X11_BAD_VALUE;
	target = render_picture_find(server, x11_read32(request + 8, msb));
	if (target == NULL)
		return RENDER_BAD_PICTURE;
	if (target->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;

	/* The colour as a source of its own. */
	memset(&source, 0, sizeof(source));
	x11_region_init(&source.view.clip);
	source.source.kind = X11_SOURCE_SOLID;
	source.source.color.red = x11_read16(request + 12, msb);
	source.source.color.green = x11_read16(request + 14, msb);
	source.source.color.blue = x11_read16(request + 16, msb);
	source.source.color.alpha = x11_read16(request + 18, msb);

	/* Each rectangle. */
	count = (length - 20U) / 8U;
	for (at = 0U; at < count; at++) {
		rectangle = request + 20U + at * 8U;
		x = (int16_t)x11_read16(rectangle, msb);
		y = (int16_t)x11_read16(rectangle + 2, msb);
		width = x11_read16(rectangle + 4, msb);
		height = x11_read16(rectangle + 6, msb);
		render_draw(server, op, target, &source, 0, 0, NULL, 0, 0, x, y, width, height);
	}

	/* Succeeded: filled. */
	render_operand_fini(&source);
	return 0U;
}

/*
 * Trapezoids: the trapezoids made an alpha mask (of the mask format, all
 * in one; or, with no mask format, each in its own, a1 or a8 by the
 * destination's poly edge), and the source composited through it.  The
 * source's (xSrc, ySrc) lies on the first trapezoid's left edge's first
 * point.
 */
static unsigned
render_trapezoids(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_trapezoid *trapezoids;
	struct x11_picture *source;
	struct x11_picture *target;
	uint32_t mask_format;
	size_t count;
	size_t at;
	unsigned op;
	unsigned mask_depth;
	int source_dx;
	int source_dy;
	int valid;
	int msb;

	/* The operator, the source and destination, the mask format, and whole trapezoids. */
	msb = server->clients[index].order;
	if (length < 24U || (length - 24U) % 40U != 0U)
		return X11_BAD_LENGTH;
	op = request[4];
	valid = render_op_valid(op);
	if (!valid)
		return X11_BAD_VALUE;
	source = render_picture_find(server, x11_read32(request + 8, msb));
	if (source == NULL)
		return RENDER_BAD_PICTURE;
	target = render_picture_find(server, x11_read32(request + 12, msb));
	if (target == NULL)
		return RENDER_BAD_PICTURE;
	if (target->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;
	mask_format = x11_read32(request + 16, msb);
	if (mask_format != 0U) {
		mask_depth = render_format_depth(mask_format);
		if (mask_depth == 0U)
			return RENDER_BAD_PICT_FORMAT;
	}
	count = (length - 24U) / 40U;
	if (count == 0U)
		return 0U;

	/* The trapezoids. */
	trapezoids = malloc(count * sizeof(*trapezoids));
	if (trapezoids == NULL)
		return X11_BAD_ALLOC;
	for (at = 0U; at < count; at++)
		render_trapezoid_read(request + 24U + at * 40U, msb, &trapezoids[at]);

	/* Drawn, the source moved by the first trapezoid's first point (whole pixels, rounded down). */
	source_dx = (int16_t)x11_read16(request + 20, msb);
	source_dy = (int16_t)x11_read16(request + 22, msb);
	source_dx -= render_fixed_int(trapezoids[0].left[0]);
	source_dy -= render_fixed_int(trapezoids[0].left[1]);
	render_shapes(server, op, source, target, mask_format, source_dx, source_dy, trapezoids, count, 1U);

	/* Succeeded: drawn. */
	free(trapezoids);
	return 0U;
}

/*
 * Triangles, TriStrip and TriFan: the triangles (each three points; a
 * strip's each point with the two before it; a fan's each pair of points
 * after the first with the first) made two trapezoids each and drawn as
 * Trapezoids draws them, a triangle a shape.  The source's (xSrc, ySrc)
 * lies on the first point.
 */
static unsigned
render_triangles(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_trapezoid *trapezoids;
	struct x11_picture *source;
	struct x11_picture *target;
	int32_t *points;
	int32_t triangle[6];
	uint32_t mask_format;
	size_t point_count;
	size_t triangles;
	size_t at;
	size_t corner;
	size_t first;
	unsigned op;
	unsigned mask_depth;
	int source_dx;
	int source_dy;
	int valid;
	int msb;

	/* The operator, the source and destination, the mask format, and whole points. */
	msb = server->clients[index].order;
	if (length < 24U || (length - 24U) % 8U != 0U)
		return X11_BAD_LENGTH;
	if (request[1] == RENDER_TRIANGLES && (length - 24U) % 24U != 0U)
		return X11_BAD_LENGTH;
	op = request[4];
	valid = render_op_valid(op);
	if (!valid)
		return X11_BAD_VALUE;
	source = render_picture_find(server, x11_read32(request + 8, msb));
	if (source == NULL)
		return RENDER_BAD_PICTURE;
	target = render_picture_find(server, x11_read32(request + 12, msb));
	if (target == NULL)
		return RENDER_BAD_PICTURE;
	if (target->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;
	mask_format = x11_read32(request + 16, msb);
	if (mask_format != 0U) {
		mask_depth = render_format_depth(mask_format);
		if (mask_depth == 0U)
			return RENDER_BAD_PICT_FORMAT;
	}

	/* The triangles the points make. */
	point_count = (length - 24U) / 8U;
	triangles = point_count / 3U;
	if (request[1] != RENDER_TRIANGLES) {
		triangles = 0U;
		if (point_count >= 3U)
			triangles = point_count - 2U;
	}
	if (triangles == 0U)
		return 0U;

	/* The points. */
	points = malloc(point_count * 2U * sizeof(*points));
	if (points == NULL)
		return X11_BAD_ALLOC;
	for (at = 0U; at < point_count * 2U; at++)
		points[at] = (int32_t)x11_read32(request + 24U + at * 4U, msb);

	/* Two trapezoids for each triangle. */
	trapezoids = malloc(triangles * 2U * sizeof(*trapezoids));
	if (trapezoids == NULL) {
		free(points);
		return X11_BAD_ALLOC;
	}
	for (at = 0U; at < triangles; at++) {
		for (corner = 0U; corner < 3U; corner++) {
			/* The corner's point: the triangle's own, the strip's run, or the fan's first and the pair. */
			first = at * 3U + corner;
			if (request[1] == RENDER_TRI_STRIP)
				first = at + corner;
			if (request[1] == RENDER_TRI_FAN) {
				first = at + corner;
				if (corner == 0U)
					first = 0U;
			}
			triangle[corner * 2U] = points[first * 2U];
			triangle[corner * 2U + 1U] = points[first * 2U + 1U];
		}
		render_triangle_trapezoids(triangle, &trapezoids[at * 2U]);
	}

	/* Drawn, the source moved by the first point (whole pixels, rounded down). */
	source_dx = (int16_t)x11_read16(request + 20, msb);
	source_dy = (int16_t)x11_read16(request + 22, msb);
	source_dx -= render_fixed_int(points[0]);
	source_dy -= render_fixed_int(points[1]);
	render_shapes(server, op, source, target, mask_format, source_dx, source_dy, trapezoids, triangles * 2U, 2U);

	/* Succeeded: drawn. */
	free(trapezoids);
	free(points);
	return 0U;
}

/*
 * AddTraps: the coverage of traps (each a top and a bottom span) added to
 * an alpha picture's pixels, moved by an offset.
 */
static unsigned
render_add_traps(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_trapezoid trapezoid;
	struct x11_picture *picture;
	struct x11_image *image;
	struct x11_region all;
	struct x11_box all_box;
	int error;
	const uint8_t *trap;
	size_t count;
	size_t at;
	int x;
	int y;
	int msb;

	/* An alpha picture of a pixmap, and whole traps. */
	msb = server->clients[index].order;
	if (length < 12U || (length - 12U) % 24U != 0U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, msb));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	if (picture->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;
	if (picture->depth != 1U && picture->depth != X11_DEPTH_ALPHA)
		return X11_BAD_MATCH;
	if (picture->pixmap == NULL)
		return 0U;
	x = (int16_t)x11_read16(request + 8, msb);
	y = (int16_t)x11_read16(request + 10, msb);

	/* The pixmap's pixels up to date in memory (it may have a copy on the GPU). */
	image = &picture->pixmap->image;
	x11_render_image_memory(image);

	/* Each trap as the trapezoid between its spans. */
	count = (length - 12U) / 24U;
	for (at = 0U; at < count; at++) {
		trap = request + 12U + at * 24U;
		trapezoid.top = (int32_t)x11_read32(trap + 8, msb);
		trapezoid.bottom = (int32_t)x11_read32(trap + 20, msb);
		trapezoid.left[0] = (int32_t)x11_read32(trap, msb);
		trapezoid.left[1] = trapezoid.top;
		trapezoid.left[2] = (int32_t)x11_read32(trap + 12, msb);
		trapezoid.left[3] = trapezoid.bottom;
		trapezoid.right[0] = (int32_t)x11_read32(trap + 4, msb);
		trapezoid.right[1] = trapezoid.top;
		trapezoid.right[2] = (int32_t)x11_read32(trap + 16, msb);
		trapezoid.right[3] = trapezoid.bottom;
		x11_coverage_trapezoid(image, x, y, &trapezoid);
	}

	/* The pixels put onto the copy on the GPU, when there is one. */
	if (image->gpu != NULL) {
		all_box.x0 = 0;
		all_box.y0 = 0;
		all_box.x1 = image->width;
		all_box.y1 = image->height;
		x11_region_init(&all);
		error = x11_region_set(&all, &all_box);
		if (error == 0)
			x11_render_vulkan_put(image, &all, 0, 0, image->width, image->height, image->pixels, image->stride, X11_FUNCTION_COPY, 0xffffffffU);
		x11_region_fini(&all);
	}

	/* Succeeded: added (a pixmap is not shown). */
	return 0U;
}

/* CreateGlyphSet: an empty glyph set of a format. */
static unsigned
render_create_glyph_set(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_glyphset *set;
	uint32_t id;
	uint32_t format;
	unsigned taken;
	unsigned depth;
	int chosen;
	int error;
	int msb;

	/* An id of the client's range not in use, and a format. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, msb);
	chosen = x11_id_chosen(&server->clients[index], id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;
	format = x11_read32(request + 8, msb);
	depth = render_format_depth(format);
	if (depth == 0U)
		return RENDER_BAD_PICT_FORMAT;

	/* The set, charged to the client. */
	error = x11_memory_charge(server, index, sizeof(*set));
	if (error != 0)
		return X11_BAD_ALLOC;
	set = calloc(1U, sizeof(*set));
	if (set == NULL) {
		x11_memory_release(server, index, sizeof(*set));
		return X11_BAD_ALLOC;
	}
	set->references = 1U;
	set->owner = index;
	set->depth = depth;
	set->format = format;
	set->charged = sizeof(*set);

	/* Found by its id. */
	error = x11_resource_add(server, id, X11_RESOURCE_GLYPHSET, set);
	if (error != 0) {
		set->references = 0U;
		render_glyphset_drop(server, set);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/* ReferenceGlyphSet: another id for a glyph set. */
static unsigned
render_reference_glyph_set(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_glyphset *set;
	uint32_t id;
	unsigned taken;
	int chosen;
	int error;
	int msb;

	/* An id of the client's range not in use, and a glyph set. */
	msb = server->clients[index].order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, msb);
	chosen = x11_id_chosen(&server->clients[index], id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;
	set = render_glyphset_find(server, x11_read32(request + 8, msb));
	if (set == NULL)
		return RENDER_BAD_GLYPH_SET;

	/* Found by the new id too. */
	error = x11_resource_add(server, id, X11_RESOURCE_GLYPHSET, set);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: one more name. */
	set->references++;
	return 0U;
}

/* FreeGlyphSet: one id of a glyph set goes (the set with the last). */
static unsigned
render_free_glyph_set(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_glyphset *set;
	uint32_t id;

	/* The glyph set. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, server->clients[index].order);
	set = render_glyphset_find(server, id);
	if (set == NULL)
		return RENDER_BAD_GLYPH_SET;

	/* Succeeded: the name goes. */
	x11_resource_remove(server, id);
	render_glyphset_drop(server, set);
	return 0U;
}

/*
 * AddGlyphs: glyphs of the set's format, each with its id, metrics and
 * image (rows padded to 32 bits; a bitmap's bits in the client's bit
 * order, wider pixels in its byte order).  A glyph of an id the set has
 * is replaced.
 */
static unsigned
render_add_glyphs(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_glyphset *set;
	struct render_glyph *glyph;
	struct render_glyph *old;
	const uint8_t *info;
	const uint8_t *data;
	const uint8_t *line;
	size_t count;
	size_t at;
	size_t used;
	size_t row_bytes;
	size_t bytes;
	uint32_t pixel;
	unsigned width;
	unsigned height;
	unsigned row;
	unsigned column;
	unsigned code;
	int error;
	int msb;

	/* The set, the count, and room for the ids and metrics. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	set = render_glyphset_find(server, x11_read32(request + 4, msb));
	if (set == NULL)
		return RENDER_BAD_GLYPH_SET;
	count = x11_read32(request + 8, msb);
	if (count > (length - 12U) / 16U)
		return X11_BAD_LENGTH;

	/* The images fill the request exactly. */
	used = 12U + count * 16U;
	for (at = 0U; at < count; at++) {
		info = request + 12U + count * 4U + at * 12U;
		width = x11_read16(info, msb);
		height = x11_read16(info + 2, msb);
		bytes = render_row_bytes(set->depth, width) * height;
		if (bytes > length - used)
			return X11_BAD_LENGTH;
		used += bytes;
	}
	if (used != length)
		return X11_BAD_LENGTH;

	/* Each glyph made and kept. */
	data = request + 12U + count * 16U;
	code = 0U;
	for (at = 0U; at < count && code == 0U; at++) {
		info = request + 12U + count * 4U + at * 12U;
		width = x11_read16(info, msb);
		height = x11_read16(info + 2, msb);
		row_bytes = render_row_bytes(set->depth, width);

		/* The glyph, charged to the set's owner. */
		glyph = calloc(1U, sizeof(*glyph));
		if (glyph == NULL) {
			code = X11_BAD_ALLOC;
			break;
		}
		glyph->id = x11_read32(request + 12U + at * 4U, msb);
		glyph->x = (int16_t)x11_read16(info + 4, msb);
		glyph->y = (int16_t)x11_read16(info + 6, msb);
		glyph->x_off = (int16_t)x11_read16(info + 8, msb);
		glyph->y_off = (int16_t)x11_read16(info + 10, msb);
		error = render_image_make(&glyph->image, (int)width, (int)height, set->depth);
		if (error == 0)
			error = x11_memory_charge(server, set->owner, render_glyph_bytes(glyph));
		if (error != 0) {
			free(glyph->image.pixels);
			free(glyph);
			code = X11_BAD_ALLOC;
			break;
		}

		/* Its pixels from the data. */
		for (row = 0U; row < height; row++) {
			line = data + (size_t)row * row_bytes;
			for (column = 0U; column < width; column++) {
				if (set->depth == 1U) {
					pixel = (line[column / 8U] >> (msb ? 7U - column % 8U : column % 8U)) & 1U;
				} else if (set->depth == X11_DEPTH_ALPHA) {
					pixel = line[column];
				} else {
					pixel = x11_read32(line + (size_t)column * 4U, msb);
				}
				if (set->depth == X11_DEPTH)
					pixel &= 0x00ffffffU;
				glyph->image.pixels[(size_t)row * glyph->image.stride + column] = pixel;
			}
		}
		data += row_bytes * height;

		/* Kept, replacing a glyph of the same id. */
		old = render_glyph_find(set, glyph->id);
		if (old != NULL) {
			render_glyph_remove(set, old->id);
			x11_memory_release(server, set->owner, render_glyph_bytes(old));
			set->charged -= render_glyph_bytes(old);
			free(old->image.pixels);
			free(old);
		}
		error = render_glyph_put(set, glyph);
		if (error != 0) {
			x11_memory_release(server, set->owner, render_glyph_bytes(glyph));
			free(glyph->image.pixels);
			free(glyph);
			code = X11_BAD_ALLOC;
			break;
		}
		set->charged += render_glyph_bytes(glyph);
	}

	/* Added, or the error (the glyphs before it stay). */
	if (code != 0U)
		return code;
	return 0U;
}

/* FreeGlyphs: glyphs of a set go; an id the set does not have is BadGlyph (those before it went). */
static unsigned
render_free_glyphs(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_glyphset *set;
	struct render_glyph *glyph;
	size_t count;
	size_t at;
	int msb;

	/* The set and whole ids. */
	msb = server->clients[index].order;
	if (length < 8U || length % 4U != 0U)
		return X11_BAD_LENGTH;
	set = render_glyphset_find(server, x11_read32(request + 4, msb));
	if (set == NULL)
		return RENDER_BAD_GLYPH_SET;

	/* Each glyph. */
	count = (length - 8U) / 4U;
	for (at = 0U; at < count; at++) {
		glyph = render_glyph_find(set, x11_read32(request + 8U + at * 4U, msb));
		if (glyph == NULL)
			return RENDER_BAD_GLYPH;
		render_glyph_remove(set, glyph->id);
		x11_memory_release(server, set->owner, render_glyph_bytes(glyph));
		set->charged -= render_glyph_bytes(glyph);
		free(glyph->image.pixels);
		free(glyph);
	}

	/* Succeeded: gone. */
	return 0U;
}

/*
 * CompositeGlyphs8, 16 and 32: runs of glyphs, each run moved from the
 * pen's place, each glyph drawn at the pen less its origin and the pen
 * moved on by its advance; a run of count 255 changes the glyph set.  With
 * a mask format the glyphs are added into a mask of that format over all
 * of them and the source composited through it; without one each glyph
 * is the mask of its own composite.  The source's (xSrc, ySrc) lies on
 * the first run's start.
 */
static unsigned
render_composite_glyphs(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct render_operand source;
	struct render_operand glyph_operand;
	struct render_operand mask_operand;
	struct render_glyphset *set;
	struct render_glyphset *first_set;
	struct render_glyph *glyph;
	struct x11_picture *source_picture;
	struct x11_picture *target;
	struct x11_image mask;
	struct x11_source white;
	struct x11_region all;
	struct x11_box drawable;
	struct x11_box extents;
	struct x11_box box;
	const uint8_t *item;
	uint32_t mask_format;
	uint32_t glyph_id;
	size_t at;
	size_t ids_bytes;
	size_t id_size;
	unsigned op;
	unsigned count;
	unsigned glyph_index;
	unsigned mask_depth;
	unsigned pass;
	unsigned code;
	int source_x;
	int source_y;
	int pen_x;
	int pen_y;
	int start_x;
	int start_y;
	int started;
	int inside;
	int have_extents;
	int error;
	int valid;
	int msb;

	/* The operator, the source and destination, the mask format and the first glyph set. */
	msb = server->clients[index].order;
	if (length < 28U)
		return X11_BAD_LENGTH;
	op = request[4];
	valid = render_op_valid(op);
	if (!valid)
		return X11_BAD_VALUE;
	source_picture = render_picture_find(server, x11_read32(request + 8, msb));
	if (source_picture == NULL)
		return RENDER_BAD_PICTURE;
	target = render_picture_find(server, x11_read32(request + 12, msb));
	if (target == NULL)
		return RENDER_BAD_PICTURE;
	if (target->kind != X11_SOURCE_IMAGE)
		return X11_BAD_DRAWABLE;
	mask_format = x11_read32(request + 16, msb);
	mask_depth = 0U;
	if (mask_format != 0U) {
		mask_depth = render_format_depth(mask_format);
		if (mask_depth == 0U)
			return RENDER_BAD_PICT_FORMAT;
	}
	first_set = render_glyphset_find(server, x11_read32(request + 20, msb));
	if (first_set == NULL)
		return RENDER_BAD_GLYPH_SET;
	source_x = (int16_t)x11_read16(request + 24, msb);
	source_y = (int16_t)x11_read16(request + 26, msb);
	id_size = 1U;
	if (request[1] == RENDER_COMPOSITE_GLYPHS16)
		id_size = 2U;
	if (request[1] == RENDER_COMPOSITE_GLYPHS32)
		id_size = 4U;

	/*
	 * Two passes over the runs: the first checks them (the glyph sets
	 * they change to) and finds the glyphs' extents, the second draws.
	 */
	code = render_operand_of(server, source_picture, &source);
	if (code != 0U) {
		render_operand_fini(&source);
		return code;
	}
	memset(&mask, 0, sizeof(mask));
	memset(&white, 0, sizeof(white));
	white.kind = X11_SOURCE_SOLID;
	white.color.red = 0xffffU;
	white.color.green = 0xffffU;
	white.color.blue = 0xffffU;
	white.color.alpha = 0xffffU;
	have_extents = 0;
	extents.x0 = 0;
	extents.y0 = 0;
	extents.x1 = 0;
	extents.y1 = 0;
	start_x = 0;
	start_y = 0;
	for (pass = 0U; pass < 2U; pass++) {
		set = first_set;
		pen_x = 0;
		pen_y = 0;
		started = 0;
		at = 28U;
		while (at + 8U <= length) {
			item = request + at;
			count = item[0];

			/* A change of glyph set. */
			if (count == 0xffU) {
				if (at + 12U > length) {
					code = X11_BAD_LENGTH;
					break;
				}
				set = render_glyphset_find(server, x11_read32(item + 8, msb));
				if (set == NULL) {
					code = RENDER_BAD_GLYPH_SET;
					break;
				}
				at += 12U;
				continue;
			}

			/* A run: the pen moves, then each glyph. */
			ids_bytes = ((size_t)count * id_size + 3U) & ~(size_t)3U;
			if (at + 8U + ids_bytes > length) {
				code = X11_BAD_LENGTH;
				break;
			}
			pen_x += (int16_t)x11_read16(item + 4, msb);
			pen_y += (int16_t)x11_read16(item + 6, msb);
			if (!started) {
				start_x = pen_x;
				start_y = pen_y;
				started = 1;
			}
			for (glyph_index = 0U; glyph_index < count; glyph_index++) {
				/* The glyph's id; a set without it draws nothing and moves nothing. */
				if (id_size == 1U) {
					glyph_id = item[8U + glyph_index];
				} else if (id_size == 2U) {
					glyph_id = x11_read16(item + 8U + glyph_index * 2U, msb);
				} else {
					glyph_id = x11_read32(item + 8U + glyph_index * 4U, msb);
				}
				glyph = render_glyph_find(set, glyph_id);
				if (glyph == NULL)
					continue;

				/* The glyph's box on the destination. */
				box.x0 = pen_x - glyph->x;
				box.y0 = pen_y - glyph->y;
				box.x1 = box.x0 + glyph->image.width;
				box.y1 = box.y0 + glyph->image.height;
				if (pass == 0U && box.x1 > box.x0 && box.y1 > box.y0) {
					if (!have_extents) {
						extents = box;
						have_extents = 1;
					}
					if (box.x0 < extents.x0)
						extents.x0 = box.x0;
					if (box.y0 < extents.y0)
						extents.y0 = box.y0;
					if (box.x1 > extents.x1)
						extents.x1 = box.x1;
					if (box.y1 > extents.y1)
						extents.y1 = box.y1;
				}

				/* Drawn: added into the mask, or composited through itself. */
				if (pass == 1U && box.x1 > box.x0 && box.y1 > box.y0) {
					render_operand_image(&glyph_operand, &glyph->image, set->depth == X11_DEPTH_ARGB);
					if (mask_format != 0U) {
						x11_region_init(&all);
						error = x11_region_set(&all, &box);
						if (error == 0)
							x11_region_translate(&all, -extents.x0, -extents.y0);
						if (error == 0 && mask_depth >= X11_DEPTH && set->depth <= X11_DEPTH_ALPHA) {
							/* An alpha glyph in a mask with colour channels: white through it, every channel its alpha. */
							x11_render_composite(&mask, &all, X11_OP_ADD, &white, 0, 0, &glyph_operand.source, 0, 0, box.x0 - extents.x0, box.y0 - extents.y0, box.x1 - box.x0, box.y1 - box.y0);
						} else if (error == 0) {
							x11_render_composite(&mask, &all, X11_OP_ADD, &glyph_operand.source, 0, 0, NULL, 0, 0, box.x0 - extents.x0, box.y0 - extents.y0, box.x1 - box.x0, box.y1 - box.y0);
						}
						x11_region_fini(&all);
					} else {
						render_draw(server, op, target, &source, source_x + box.x0 - start_x, source_y + box.y0 - start_y, &glyph_operand, 0, 0, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
					}
					render_operand_fini(&glyph_operand);
				}

				/* The pen moves on. */
				pen_x += glyph->x_off;
				pen_y += glyph->y_off;
			}
			at += 8U + ids_bytes;
		}

		/* A malformed run ends it. */
		if (code != 0U)
			break;

		/* After the first pass: the mask over the extents inside the drawable, empty. */
		if (pass == 0U && mask_format != 0U) {
			if (!have_extents)
				break;
			render_drawable_box(target, &drawable);
			inside = x11_box_intersect(&extents, &drawable, &extents);
			if (!inside)
				break;
			error = render_mask_make(server, &mask, extents.x1 - extents.x0, extents.y1 - extents.y0, mask_depth);
			if (error != 0) {
				code = X11_BAD_ALLOC;
				break;
			}
		}
	}

	/* With a mask: the source through it, over the extents. */
	if (code == 0U && mask_format != 0U && mask.pixels != NULL) {
		render_operand_image(&mask_operand, &mask, mask_depth == X11_DEPTH_ARGB);
		render_draw(server, op, target, &source, source_x + extents.x0 - start_x, source_y + extents.y0 - start_y, &mask_operand, 0, 0, extents.x0, extents.y0, extents.x1 - extents.x0, extents.y1 - extents.y0);
		render_operand_fini(&mask_operand);
	}

	/* What was made goes. */
	render_mask_free(server, &mask);
	render_operand_fini(&source);
	if (code != 0U)
		return code;

	/* Succeeded: drawn. */
	return 0U;
}

/*
 * CreateCursor: a cursor of a picture's pixels (its drawable read as
 * a8r8g8b8), with a hot spot inside them.
 */
static unsigned
render_create_cursor(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t *pixels;
	unsigned code;
	int width;
	int height;
	int hot_x;
	int hot_y;
	int msb;

	/* The id, the picture and the hot spot. */
	msb = server->clients[index].order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	hot_x = x11_read16(request + 12, msb);
	hot_y = x11_read16(request + 14, msb);

	/* The picture's pixels. */
	code = x11_render_picture_pixels(server, x11_read32(request + 8, msb), &pixels, &width, &height);
	if (code != 0U)
		return code;

	/* A hot spot inside them. */
	if (hot_x >= width || hot_y >= height) {
		free(pixels);
		return X11_BAD_MATCH;
	}

	/* The cursor, which takes the pixels. */
	code = x11_cursor_create_argb(server, index, x11_read32(request + 4, msb), pixels, width, height, hot_x, hot_y);
	if (code != 0U)
		return code;

	/* Succeeded: made. */
	return 0U;
}

/*
 * CreateAnimCursor: a cursor of frames (each a cursor and how long it is
 * shown).  The server shows the first frame and does not move on.
 */
static unsigned
render_create_anim_cursor(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_cursor *first;
	struct x11_cursor *frame;
	size_t count;
	size_t at;
	unsigned code;
	int msb;

	/* The id and at least one whole frame. */
	msb = server->clients[index].order;
	if (length < 8U || (length - 8U) % 8U != 0U)
		return X11_BAD_LENGTH;
	count = (length - 8U) / 8U;
	if (count == 0U)
		return X11_BAD_VALUE;

	/* Every frame's cursor exists. */
	first = NULL;
	for (at = 0U; at < count; at++) {
		frame = x11_cursor_find(server, x11_read32(request + 8U + at * 8U, msb));
		if (frame == NULL)
			return X11_BAD_CURSOR;
		if (first == NULL)
			first = frame;
	}

	/* The cursor, as its first frame. */
	code = x11_cursor_create_copy(server, index, x11_read32(request + 4, msb), first);
	if (code != 0U)
		return code;

	/* Succeeded: made. */
	return 0U;
}

/* SetPictureTransform: the matrix a picture is read through (the identity: none). */
static unsigned
render_set_transform(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_picture *picture;
	unsigned at;
	int identity;
	int msb;

	/* The picture and the nine values. */
	msb = server->clients[index].order;
	if (length != 44U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, msb));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;

	/* The matrix, and whether it changes nothing. */
	identity = 1;
	for (at = 0U; at < 9U; at++) {
		picture->transform[at] = (int32_t)x11_read32(request + 8U + at * 4U, msb);
		if (at % 4U == 0U && picture->transform[at] != 65536)
			identity = 0;
		if (at % 4U != 0U && picture->transform[at] != 0)
			identity = 0;
	}

	/* Succeeded: kept. */
	picture->transformed = !identity;
	return 0U;
}

/*
 * QueryFilters: the filters (nearest, bilinear, convolution) and their
 * aliases (fast for nearest, good and best for bilinear), each name with
 * the number of the filter it is an alias of.
 */
static unsigned
render_query_filters(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	static const char *const names[RENDER_FILTER_NAMES] = { "nearest", "bilinear", "convolution", "fast", "good", "best" };
	static const uint16_t aliases[RENDER_FILTER_NAMES] = { RENDER_FILTER_ALIAS_NONE, RENDER_FILTER_ALIAS_NONE, RENDER_FILTER_ALIAS_NONE, 0U, 1U, 1U };
	uint8_t reply[32U + RENDER_FILTER_NAMES * 2U + 64U];
	size_t used;
	size_t name_length;
	unsigned at;
	unsigned kind;
	int msb;

	/* A drawable. */
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;
	kind = x11_resource_type(server, x11_read32(request + 4, msb));
	if (kind != X11_RESOURCE_WINDOW && kind != X11_RESOURCE_PIXMAP)
		return X11_BAD_DRAWABLE;

	/* The aliases, then the names, padded to a word. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, RENDER_FILTER_NAMES, msb);
	x11_write32(reply + 12, RENDER_FILTER_NAMES, msb);
	used = 32U;
	for (at = 0U; at < RENDER_FILTER_NAMES; at++) {
		x11_write16(reply + used, aliases[at], msb);
		used += 2U;
	}
	for (at = 0U; at < RENDER_FILTER_NAMES; at++) {
		name_length = strlen(names[at]);
		reply[used] = (uint8_t)name_length;
		memcpy(reply + used + 1U, names[at], name_length);
		used += 1U + name_length;
	}

	/* Succeeded: answered. */
	used = (used + 3U) & ~(size_t)3U;
	x11_reply(server, client, reply, used);
	return 0U;
}

/*
 * SetPictureFilter: the filter a transformed picture is sampled by, by
 * name (an alias names its filter); a convolution takes its kernel's
 * width and height and then its values (16.16).
 */
static unsigned
render_set_filter(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	static const char *const names[RENDER_FILTER_NAMES] = { "nearest", "bilinear", "convolution", "fast", "good", "best" };
	static const unsigned filters[RENDER_FILTER_NAMES] = { X11_FILTER_NEAREST, X11_FILTER_BILINEAR, X11_FILTER_CONVOLUTION, X11_FILTER_NEAREST, X11_FILTER_BILINEAR, X11_FILTER_BILINEAR };
	struct x11_picture *picture;
	int32_t *kernel;
	size_t name_length;
	size_t values_at;
	size_t value_count;
	size_t at;
	size_t letter;
	unsigned filter;
	int width;
	int height;
	int matches;
	size_t known_length;
	int msb;

	/* The picture, the name and the values after it. */
	msb = server->clients[index].order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	picture = render_picture_find(server, x11_read32(request + 4, msb));
	if (picture == NULL)
		return RENDER_BAD_PICTURE;
	name_length = x11_read16(request + 8, msb);
	values_at = 12U + ((name_length + 3U) & ~(size_t)3U);
	if (values_at > length || (length - values_at) % 4U != 0U)
		return X11_BAD_LENGTH;
	value_count = (length - values_at) / 4U;

	/* The filter of the name (letters in either case). */
	filter = RENDER_FILTER_NAMES;
	for (at = 0U; at < RENDER_FILTER_NAMES && filter == RENDER_FILTER_NAMES; at++) {
		known_length = strlen(names[at]);
		if (known_length != name_length)
			continue;
		matches = 1;
		for (letter = 0U; letter < name_length; letter++) {
			if ((request[12U + letter] | 0x20U) != (uint8_t)names[at][letter])
				matches = 0;
		}
		if (matches)
			filter = filters[at];
	}
	if (filter == RENDER_FILTER_NAMES)
		return X11_BAD_NAME;

	/* Nearest and bilinear take no values. */
	if (filter != X11_FILTER_CONVOLUTION) {
		if (value_count != 0U)
			return X11_BAD_MATCH;
		free(picture->kernel);
		picture->kernel = NULL;
		picture->filter = filter;
		return 0U;
	}

	/* A convolution: its size (whole numbers) and a value for each cell. */
	if (value_count < 2U)
		return X11_BAD_MATCH;
	width = (int32_t)x11_read32(request + values_at, msb) >> 16;
	height = (int32_t)x11_read32(request + values_at + 4U, msb) >> 16;
	if (width <= 0 || height <= 0 || width > RENDER_KERNEL_MAX || height > RENDER_KERNEL_MAX)
		return X11_BAD_MATCH;
	if (value_count != 2U + (size_t)width * (size_t)height)
		return X11_BAD_MATCH;
	kernel = malloc((size_t)width * (size_t)height * sizeof(*kernel));
	if (kernel == NULL)
		return X11_BAD_ALLOC;
	for (at = 0U; at < (size_t)width * (size_t)height; at++)
		kernel[at] = (int32_t)x11_read32(request + values_at + 8U + at * 4U, msb);

	/* Succeeded: the kernel replaces the old. */
	free(picture->kernel);
	picture->kernel = kernel;
	picture->kernel_width = width;
	picture->kernel_height = height;
	picture->filter = filter;
	return 0U;
}

/*
 * CreateSolidFill, CreateLinearGradient, CreateRadialGradient and
 * CreateConicalGradient: a picture of its own, one colour or a gradient
 * with its stops (in order, from 0 to 1; at least one).
 */
static unsigned
render_create_source(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_picture *picture;
	struct x11_gradient_stop *stops;
	const uint8_t *colors;
	size_t fixed;
	size_t count;
	size_t at;
	unsigned code;
	int32_t previous;
	int error;
	int msb;

	/* The fixed part by the kind, and the stops after it. */
	msb = server->clients[index].order;
	fixed = 16U;
	if (request[1] == RENDER_CREATE_LINEAR_GRADIENT)
		fixed = 28U;
	if (request[1] == RENDER_CREATE_RADIAL_GRADIENT)
		fixed = 36U;
	if (request[1] == RENDER_CREATE_CONICAL_GRADIENT)
		fixed = 24U;
	if (length < fixed)
		return X11_BAD_LENGTH;
	count = 0U;
	if (request[1] != RENDER_CREATE_SOLID_FILL) {
		count = x11_read32(request + fixed - 4U, msb);
		if (count > (length - fixed) / 12U || length != fixed + count * 12U)
			return X11_BAD_LENGTH;
		if (count == 0U)
			return X11_BAD_VALUE;
	} else if (length != fixed) {
		return X11_BAD_LENGTH;
	}

	/* The stops, in order inside 0 to 1. */
	stops = NULL;
	if (count != 0U) {
		stops = malloc(count * sizeof(*stops));
		if (stops == NULL)
			return X11_BAD_ALLOC;
		colors = request + fixed + count * 4U;
		previous = 0;
		for (at = 0U; at < count; at++) {
			stops[at].offset = (int32_t)x11_read32(request + fixed + at * 4U, msb);
			stops[at].color.red = x11_read16(colors + at * 8U, msb);
			stops[at].color.green = x11_read16(colors + at * 8U + 2U, msb);
			stops[at].color.blue = x11_read16(colors + at * 8U + 4U, msb);
			stops[at].color.alpha = x11_read16(colors + at * 8U + 6U, msb);
			if (stops[at].offset < previous || stops[at].offset > 65536) {
				free(stops);
				return X11_BAD_VALUE;
			}
			previous = stops[at].offset;
		}
	}

	/* The picture. */
	code = render_new_picture(server, index, x11_read32(request + 4, msb), &picture);
	if (code != 0U) {
		free(stops);
		return code;
	}
	picture->stops = stops;
	picture->stop_count = count;

	/* Its kind and geometry. */
	switch (request[1]) {
	case RENDER_CREATE_SOLID_FILL:
		picture->kind = X11_SOURCE_SOLID;
		picture->color.red = x11_read16(request + 8, msb);
		picture->color.green = x11_read16(request + 10, msb);
		picture->color.blue = x11_read16(request + 12, msb);
		picture->color.alpha = x11_read16(request + 14, msb);
		break;
	case RENDER_CREATE_LINEAR_GRADIENT:
		picture->kind = X11_SOURCE_LINEAR;
		for (at = 0U; at < 4U; at++)
			picture->points[at] = (int32_t)x11_read32(request + 8U + at * 4U, msb);
		break;
	case RENDER_CREATE_RADIAL_GRADIENT:
		picture->kind = X11_SOURCE_RADIAL;
		for (at = 0U; at < 4U; at++)
			picture->points[at] = (int32_t)x11_read32(request + 8U + at * 4U, msb);
		picture->radii[0] = (int32_t)x11_read32(request + 24, msb);
		picture->radii[1] = (int32_t)x11_read32(request + 28, msb);
		break;
	default:
		picture->kind = X11_SOURCE_CONICAL;
		picture->points[0] = (int32_t)x11_read32(request + 8, msb);
		picture->points[1] = (int32_t)x11_read32(request + 12, msb);
		picture->angle = (int32_t)x11_read32(request + 16, msb);
		break;
	}

	/* The stops' charge, and found by its id. */
	error = x11_memory_charge(server, index, count * sizeof(*stops));
	if (error != 0) {
		render_picture_free(server, picture);
		return X11_BAD_ALLOC;
	}
	picture->charged += count * sizeof(*stops);
	error = x11_resource_add(server, picture->id, X11_RESOURCE_PICTURE, picture);
	if (error != 0) {
		render_picture_free(server, picture);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: made. */
	return 0U;
}

/*
 * Makes an empty picture of an id the client chose, with the default
 * attributes, charged to the client.  Returns 0 with it, or the error.
 */
static unsigned
render_new_picture(
	struct x11server *server,
	unsigned index,
	uint32_t id,
	struct x11_picture **made)
{
	struct x11_picture *picture;
	unsigned taken;
	int chosen;
	int error;

	/* An id of the client's range not in use. */
	chosen = x11_id_chosen(&server->clients[index], id);
	taken = x11_resource_type(server, id);
	if (!chosen || taken != 0U)
		return X11_BAD_ID_CHOICE;

	/* The picture, charged to the client. */
	error = x11_memory_charge(server, index, sizeof(*picture));
	if (error != 0)
		return X11_BAD_ALLOC;
	picture = calloc(1U, sizeof(*picture));
	if (picture == NULL) {
		x11_memory_release(server, index, sizeof(*picture));
		return X11_BAD_ALLOC;
	}

	/* The defaults: no repeat, no clip, ClipByChildren, sharp edges, nearest. */
	picture->id = id;
	picture->owner = index;
	picture->charged = sizeof(*picture);
	picture->repeat = X11_REPEAT_NONE;
	x11_region_init(&picture->clip);
	picture->graphics_exposures = 1U;
	picture->subwindow_mode = X11_CLIP_BY_CHILDREN;
	picture->poly_edge = POLY_EDGE_SHARP;
	picture->filter = X11_FILTER_NEAREST;

	/* Succeeded: made. */
	*made = picture;
	return 0U;
}

/*
 * Reads the values of a picture's attributes a mask names and checks
 * each: BadValue for one out of range, BadPicture for an alpha map that
 * is not a picture (BadMatch for one that is not a drawable's), BadPixmap
 * for a clip mask that is not a pixmap (BadMatch for one not of depth 1).
 * Returns 0, or the error; the read values' clip is to be ended either
 * way (render_values_apply takes it).
 */
static unsigned
render_values_read(
	struct x11server *server,
	unsigned index,
	uint32_t mask,
	const uint8_t *values,
	struct render_values *read)
{
	struct x11_picture *alpha_map;
	struct x11_pixmap *pixmap;
	uint32_t value;
	unsigned bit;
	int error;
	int msb;

	/* Each value in bit order. */
	msb = server->clients[index].order;
	memset(read, 0, sizeof(*read));
	x11_region_init(&read->clip);
	read->mask = mask;
	for (bit = 0U; bit < PICTURE_ATTRIBUTES; bit++) {
		if ((mask & (1U << bit)) == 0U)
			continue;
		value = x11_read32(values, msb);
		values += 4;
		read->value[bit] = value;

		/* Its check. */
		switch (bit) {
		case PICTURE_REPEAT:
			/* None, Normal, Pad or Reflect. */
			if (value > X11_REPEAT_REFLECT)
				return X11_BAD_VALUE;
			break;
		case PICTURE_ALPHA_MAP:
			/* None, or a picture of a drawable. */
			if (value == 0U)
				break;
			alpha_map = render_picture_find(server, value);
			if (alpha_map == NULL)
				return RENDER_BAD_PICTURE;
			if (alpha_map->kind != X11_SOURCE_IMAGE)
				return X11_BAD_MATCH;
			break;
		case PICTURE_CLIP_MASK:
			/* None, or a pixmap of depth 1 made a region. */
			read->clip_given = 1;
			if (value == 0U) {
				read->clip_none = 1;
				break;
			}
			pixmap = x11_pixmap_find(server, value);
			if (pixmap == NULL)
				return X11_BAD_PIXMAP;
			if (pixmap->depth != 1U)
				return X11_BAD_MATCH;
			error = x11_gc_clip_region(&pixmap->image, &read->clip);
			if (error != 0)
				return X11_BAD_ALLOC;
			break;
		case PICTURE_GRAPHICS_EXPOSURES:
		case PICTURE_SUBWINDOW_MODE:
		case PICTURE_POLY_EDGE:
		case PICTURE_POLY_MODE:
		case PICTURE_COMPONENT_ALPHA:
			/* A boolean, or one of two modes. */
			if (value > 1U)
				return X11_BAD_VALUE;
			break;
		default:
			/* An origin or the dither: any value. */
			break;
		}
	}

	/* Succeeded: every value may be kept. */
	return 0U;
}

/* Keeps the attributes read and checked (the clip mask's region taken over). */
static void
render_values_apply(
	struct x11_picture *picture,
	struct render_values *values)
{
	uint32_t mask;

	/* Each attribute given. */
	mask = values->mask;
	if ((mask & (1U << PICTURE_REPEAT)) != 0U)
		picture->repeat = values->value[PICTURE_REPEAT];
	if ((mask & (1U << PICTURE_ALPHA_MAP)) != 0U)
		picture->alpha_map = values->value[PICTURE_ALPHA_MAP];
	if ((mask & (1U << PICTURE_ALPHA_X)) != 0U)
		picture->alpha_x = (int16_t)values->value[PICTURE_ALPHA_X];
	if ((mask & (1U << PICTURE_ALPHA_Y)) != 0U)
		picture->alpha_y = (int16_t)values->value[PICTURE_ALPHA_Y];
	if ((mask & (1U << PICTURE_CLIP_X)) != 0U)
		picture->clip_x = (int16_t)values->value[PICTURE_CLIP_X];
	if ((mask & (1U << PICTURE_CLIP_Y)) != 0U)
		picture->clip_y = (int16_t)values->value[PICTURE_CLIP_Y];
	if ((mask & (1U << PICTURE_GRAPHICS_EXPOSURES)) != 0U)
		picture->graphics_exposures = (uint8_t)values->value[PICTURE_GRAPHICS_EXPOSURES];
	if ((mask & (1U << PICTURE_SUBWINDOW_MODE)) != 0U)
		picture->subwindow_mode = (uint8_t)values->value[PICTURE_SUBWINDOW_MODE];
	if ((mask & (1U << PICTURE_POLY_EDGE)) != 0U)
		picture->poly_edge = (uint8_t)values->value[PICTURE_POLY_EDGE];
	if ((mask & (1U << PICTURE_POLY_MODE)) != 0U)
		picture->poly_mode = (uint8_t)values->value[PICTURE_POLY_MODE];
	if ((mask & (1U << PICTURE_DITHER)) != 0U)
		picture->dither = values->value[PICTURE_DITHER];
	if ((mask & (1U << PICTURE_COMPONENT_ALPHA)) != 0U)
		picture->component_alpha = (uint8_t)values->value[PICTURE_COMPONENT_ALPHA];

	/* The clip mask: its region replaces the client clip, or None takes it away. */
	if (values->clip_given) {
		x11_region_fini(&picture->clip);
		picture->clip = values->clip;
		x11_region_init(&values->clip);
		picture->clip_set = !values->clip_none;
	}
	x11_region_fini(&values->clip);
}

/* Finds a picture by its id, or NULL. */
static struct x11_picture *
render_picture_find(
	struct x11server *server,
	uint32_t id)
{
	/* The resource of the kind. */
	return x11_resource_find(server, id, X11_RESOURCE_PICTURE);
}

/*
 * Frees a picture that no id names: off the list of drawables' pictures,
 * what it keeps, its charge, and its hold on a pixmap.
 */
static void
render_picture_free(
	struct x11server *server,
	struct x11_picture *picture)
{
	struct x11_picture **link;

	/* Off the list, when it is on it. */
	for (link = &server->pictures;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == picture) {
			*link = picture->next;
			break;
		}
	}
	picture->next = NULL;

	/* What it keeps and its charge. */
	x11_region_fini(&picture->clip);
	free(picture->kernel);
	free(picture->stops);
	x11_memory_release(server, picture->owner, picture->charged);

	/* Its hold on a pixmap (which may go with it). */
	if (picture->pixmap != NULL)
		x11_pixmap_let_go(server, picture->pixmap);

	/* Succeeded: gone. */
	free(picture);
}

/* Gives the depth of one of the screen's formats, or 0 for an id that is none. */
static unsigned
render_format_depth(
	uint32_t format)
{
	/* By the id. */
	switch (format) {
	case RENDER_FORMAT_A1:
		return 1U;
	case RENDER_FORMAT_A8:
		return X11_DEPTH_ALPHA;
	case RENDER_FORMAT_X8R8G8B8:
		return X11_DEPTH;
	case RENDER_FORMAT_A8R8G8B8:
		return X11_DEPTH_ARGB;
	default:
		return 0U;
	}
}

/* Tells whether a number is an operator: Porter-Duff's, the disjoint and conjoint ones, the blend modes. */
static int
render_op_valid(
	unsigned op)
{
	/* Each family's range. */
	if (op <= 13U)
		return 1;
	if (op >= 0x10U && op <= 0x1bU)
		return 1;
	if (op >= 0x20U && op <= 0x2bU)
		return 1;
	if (op >= 0x30U && op <= 0x3eU)
		return 1;

	/* None. */
	return 0;
}

/*
 * Makes a picture an operand: what the layer reads (a drawable's pixels
 * on its image, or the picture's own colour or gradient) and its client
 * clip.  A window that cannot be seen reads as nothing.  Returns 0, or
 * BadAlloc; the operand is to be ended either way.
 */
static unsigned
render_operand_of(
	struct x11server *server,
	struct x11_picture *picture,
	struct render_operand *operand)
{
	unsigned code;
	int error;

	/* The picture's own reading. */
	memset(operand, 0, sizeof(*operand));
	x11_region_init(&operand->view.clip);
	operand->source.kind = picture->kind;
	operand->source.repeat = picture->repeat;
	operand->source.filter = picture->filter;
	operand->source.kernel = picture->kernel;
	operand->source.kernel_width = picture->kernel_width;
	operand->source.kernel_height = picture->kernel_height;
	operand->source.transformed = picture->transformed;
	memcpy(operand->source.transform, picture->transform, sizeof(picture->transform));
	operand->source.component_alpha = picture->component_alpha;
	operand->source.color = picture->color;
	memcpy(operand->source.points, picture->points, sizeof(picture->points));
	memcpy(operand->source.radii, picture->radii, sizeof(picture->radii));
	operand->source.angle = picture->angle;
	operand->source.stops = picture->stops;
	operand->source.stop_count = picture->stop_count;
	if (picture->clip_set) {
		operand->clip = &picture->clip;
		operand->clip_x = picture->clip_x;
		operand->clip_y = picture->clip_y;
	}

	/* A colour or a gradient is all there is. */
	if (picture->kind != X11_SOURCE_IMAGE)
		return 0U;

	/* A drawable: its view. */
	operand->viewed = 1;
	if (picture->window != NULL) {
		x11_region_fini(&operand->view.clip);
		code = x11_draw_view_window(server, picture->window, picture->subwindow_mode, &operand->view);
		if (code != 0U)
			return X11_BAD_ALLOC;
	} else {
		x11_region_fini(&operand->view.clip);
		error = x11_draw_view_pixmap(picture->pixmap, &operand->view);
		if (error != 0)
			return X11_BAD_ALLOC;
	}

	/* Nothing to read: a transparent colour. */
	if (operand->view.image == NULL) {
		operand->source.kind = X11_SOURCE_SOLID;
		memset(&operand->source.color, 0, sizeof(operand->source.color));
		return 0U;
	}

	/* Succeeded: the drawable's box on its image. */
	operand->source.image = operand->view.image;
	operand->source.box.x0 = operand->view.x;
	operand->source.box.y0 = operand->view.y;
	operand->source.box.x1 = operand->view.x + operand->view.width;
	operand->source.box.y1 = operand->view.y + operand->view.height;
	return 0U;
}

/* Makes an image of the server's own (a glyph, a mask) an operand: all of it, not repeated. */
static void
render_operand_image(
	struct render_operand *operand,
	const struct x11_image *image,
	int component_alpha)
{
	/* The image, from its top left. */
	memset(operand, 0, sizeof(*operand));
	x11_region_init(&operand->view.clip);
	operand->source.kind = X11_SOURCE_IMAGE;
	operand->source.image = image;
	operand->source.box.x1 = image->width;
	operand->source.box.y1 = image->height;
	operand->source.repeat = X11_REPEAT_NONE;
	operand->source.component_alpha = component_alpha;
}

/* Ends what making an operand made. */
static void
render_operand_fini(
	struct render_operand *operand)
{
	/* The view's clip. */
	x11_draw_view_fini(&operand->view);
}

/*
 * Draws a composite onto a destination picture: a rectangle (on the
 * destination) inside the destination's clip (its drawable's and its
 * client clip), the source read at (source_x, source_y) and the mask at
 * (mask_x, mask_y) where the rectangle starts.  The source's and mask's
 * client clips do not narrow it (as X.Org's server does not).  A source or mask that reads the destination's own
 * image reads a copy taken first.
 */
static void
render_draw(
	struct x11server *server,
	unsigned op,
	struct x11_picture *target,
	struct render_operand *source,
	int source_x,
	int source_y,
	struct render_operand *mask,
	int mask_x,
	int mask_y,
	int x,
	int y,
	int width,
	int height)
{
	struct render_operand destination;
	struct x11_region region;
	struct x11_region clip;
	struct x11_image *copy;
	struct x11_box box;
	const struct x11_image *source_image;
	const struct x11_image *mask_image;
	struct x11_box image_box;
	struct x11_image *target_image;
	int drawn;
	unsigned code;
	int error;

	/* Nothing to draw. */
	if (width <= 0 || height <= 0)
		return;

	/* The destination's drawable. */
	code = render_operand_of(server, target, &destination);
	if (code != 0U || destination.view.image == NULL) {
		render_operand_fini(&destination);
		return;
	}

	/* The rectangle on the image, inside the drawable's clip. */
	x11_region_init(&region);
	box.x0 = destination.view.x + x;
	box.y0 = destination.view.y + y;
	box.x1 = box.x0 + width;
	box.y1 = box.y0 + height;
	error = x11_region_copy(&region, &destination.view.clip);
	if (error == 0)
		x11_region_intersect_box(&region, &box);

	/* Inside the destination's client clip (on its clip origin). */
	if (error == 0 && destination.clip != NULL) {
		x11_region_init(&clip);
		error = x11_region_copy(&clip, destination.clip);
		if (error == 0) {
			x11_region_translate(&clip, destination.view.x + destination.clip_x, destination.view.y + destination.clip_y);
			error = x11_region_intersect(&region, &clip);
		}
		x11_region_fini(&clip);
	}

	/*
	 * Images with a copy on the GPU: what is read and the part drawn are
	 * brought up to date in memory first (the composite is drawn in
	 * memory).
	 */
	source_image = source->source.image;
	mask_image = NULL;
	if (mask != NULL)
		mask_image = mask->source.image;
	if (source_image != NULL)
		x11_render_image_memory(source_image);
	if (mask_image != NULL)
		x11_render_image_memory(mask_image);
	image_box.x0 = 0;
	image_box.y0 = 0;
	image_box.x1 = destination.view.image->width;
	image_box.y1 = destination.view.image->height;
	drawn = x11_box_intersect(&box, &image_box, &image_box);
	if (drawn)
		x11_render_image_memory_box(destination.view.image, &image_box);

	/* A source or mask on the destination's image reads a copy of it. */
	copy = NULL;
	if (error == 0 && (source_image == destination.view.image || mask_image == destination.view.image))
		error = x11_image_copy(destination.view.image, &copy);
	if (error == 0 && copy != NULL) {
		if (source_image == destination.view.image)
			source->source.image = copy;
		if (mask != NULL && mask_image == destination.view.image)
			mask->source.image = copy;
	}

	/* Composited (and the part drawn put onto the copy on the GPU, when there is one), and shown again. */
	if (error == 0 && drawn) {
		x11_render_composite(destination.view.image, &region, op, &source->source, source_x, source_y, mask != NULL ? &mask->source : NULL, mask_x, mask_y, box.x0, box.y0, width, height);
		target_image = destination.view.image;
		if (target_image->gpu != NULL)
			x11_render_vulkan_put(target_image, &region, image_box.x0, image_box.y0, image_box.x1 - image_box.x0, image_box.y1 - image_box.y0, target_image->pixels + (size_t)image_box.y0 * target_image->stride + (size_t)image_box.x0, target_image->stride, X11_FUNCTION_COPY, 0xffffffffU);
		x11_draw_view_dirty(server, &destination.view, &box);
	}

	/* What was made goes, the operands reading their own images again. */
	source->source.image = source_image;
	if (mask != NULL)
		mask->source.image = mask_image;
	x11_image_free(copy);
	x11_region_fini(&region);
	render_operand_fini(&destination);
}

/* Makes an image of the server's own, all 0, of a size and depth.  Returns 0, or -1 without memory. */
static int
render_image_make(
	struct x11_image *image,
	int width,
	int height,
	unsigned depth)
{
	/* The pixels (at least one, so that an empty image has some). */
	memset(image, 0, sizeof(*image));
	image->pixels = calloc((size_t)(width > 0 ? width : 1) * (size_t)(height > 0 ? height : 1), sizeof(uint32_t));
	if (image->pixels == NULL)
		return -1;

	/* Succeeded: its size and depth. */
	image->width = width;
	image->height = height;
	image->stride = (size_t)(width > 0 ? width : 1);
	image->depth = depth;
	return 0;
}

/*
 * Draws shapes (trapezoids, in groups of per_shape for one shape) as
 * Trapezoids and Triangles do: with a mask format, all of them added into
 * one mask over their bounds; without, each shape into a mask of its own
 * (a1 for sharp edges, a8 for smooth).  The source is read at the
 * destination's place moved by (source_dx, source_dy).  Masks are kept
 * inside the destination's drawable.
 */
static void
render_shapes(
	struct x11server *server,
	unsigned op,
	struct x11_picture *source,
	struct x11_picture *target,
	uint32_t mask_format,
	int source_dx,
	int source_dy,
	const struct x11_trapezoid *trapezoids,
	size_t count,
	size_t per_shape)
{
	struct render_operand source_operand;
	struct render_operand mask_operand;
	struct x11_image mask;
	struct x11_box drawable;
	struct x11_box bounds;
	struct x11_box box;
	size_t group;
	size_t first;
	size_t at;
	unsigned depth;
	unsigned format_depth;
	unsigned code;
	int have_bounds;
	int inside;
	int error;

	/* The source as the layer reads it. */
	code = render_operand_of(server, source, &source_operand);
	if (code != 0U) {
		render_operand_fini(&source_operand);
		return;
	}

	/* The mask's depth, and the groups: one for a mask format, each shape's own without. */
	depth = X11_DEPTH_ALPHA;
	if (mask_format == 0U && target->poly_edge == POLY_EDGE_SHARP)
		depth = 1U;
	format_depth = 0U;
	if (mask_format != 0U)
		format_depth = render_format_depth(mask_format);
	if (format_depth == 1U)
		depth = 1U;
	group = per_shape;
	if (mask_format != 0U)
		group = count;

	/* The destination's drawable on itself. */
	render_drawable_box(target, &drawable);

	/* Each group: its bounds inside the drawable, its mask, the source through it. */
	for (first = 0U; first < count; first += group) {
		have_bounds = 0;
		bounds.x0 = 0;
		bounds.y0 = 0;
		bounds.x1 = 0;
		bounds.y1 = 0;
		for (at = first; at < first + group && at < count; at++) {
			if (trapezoids[at].bottom <= trapezoids[at].top)
				continue;
			render_trapezoid_bounds(&trapezoids[at], &box);
			if (!have_bounds) {
				bounds = box;
				have_bounds = 1;
			}
			if (box.x0 < bounds.x0)
				bounds.x0 = box.x0;
			if (box.y0 < bounds.y0)
				bounds.y0 = box.y0;
			if (box.x1 > bounds.x1)
				bounds.x1 = box.x1;
			if (box.y1 > bounds.y1)
				bounds.y1 = box.y1;
		}
		if (!have_bounds)
			continue;
		inside = x11_box_intersect(&bounds, &drawable, &bounds);
		if (!inside)
			continue;

		/* The mask, each trapezoid's coverage added. */
		error = render_mask_make(server, &mask, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, depth);
		if (error != 0)
			break;
		for (at = first; at < first + group && at < count; at++)
			x11_coverage_trapezoid(&mask, -bounds.x0, -bounds.y0, &trapezoids[at]);

		/* The source through it. */
		render_operand_image(&mask_operand, &mask, 0);
		render_draw(server, op, target, &source_operand, bounds.x0 + source_dx, bounds.y0 + source_dy, &mask_operand, 0, 0, bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
		render_operand_fini(&mask_operand);
		render_mask_free(server, &mask);
	}

	/* The source's view goes. */
	render_operand_fini(&source_operand);
}

/* Reads a trapezoid of a request: top, bottom, the left line's two points, the right line's. */
static void
render_trapezoid_read(
	const uint8_t *bytes,
	int msb,
	struct x11_trapezoid *trapezoid)
{
	unsigned at;

	/* The heights, then the lines. */
	trapezoid->top = (int32_t)x11_read32(bytes, msb);
	trapezoid->bottom = (int32_t)x11_read32(bytes + 4, msb);
	for (at = 0U; at < 4U; at++) {
		trapezoid->left[at] = (int32_t)x11_read32(bytes + 8U + at * 4U, msb);
		trapezoid->right[at] = (int32_t)x11_read32(bytes + 24U + at * 4U, msb);
	}
}

/*
 * Makes a triangle (three points, 16.16) two trapezoids: from its top
 * point to its middle one, and from the middle to the bottom, each
 * between the side through the middle point and the long side from the
 * top to the bottom.  A flat part has no height and draws nothing.
 */
static void
render_triangle_trapezoids(
	const int32_t *points,
	struct x11_trapezoid *trapezoids)
{
	const int32_t *sorted[3];
	const int32_t *swap;
	const int32_t *top;
	const int32_t *middle;
	const int32_t *bottom;
	int64_t cross;
	unsigned at;
	unsigned next;

	/* The points by height, the top first. */
	sorted[0] = points;
	sorted[1] = points + 2;
	sorted[2] = points + 4;
	for (at = 0U; at < 2U; at++) {
		for (next = at + 1U; next < 3U; next++) {
			if (sorted[next][1] < sorted[at][1]) {
				swap = sorted[at];
				sorted[at] = sorted[next];
				sorted[next] = swap;
			}
		}
	}
	top = sorted[0];
	middle = sorted[1];
	bottom = sorted[2];

	/* Whether the middle point is left of the long side (the cross product's sign). */
	cross = ((int64_t)bottom[0] - top[0]) * ((int64_t)middle[1] - top[1]) - ((int64_t)middle[0] - top[0]) * ((int64_t)bottom[1] - top[1]);

	/* The upper trapezoid: between the top-middle side and the long side. */
	trapezoids[0].top = top[1];
	trapezoids[0].bottom = middle[1];
	trapezoids[1].top = middle[1];
	trapezoids[1].bottom = bottom[1];
	if (cross > 0) {
		trapezoids[0].left[0] = top[0];
		trapezoids[0].left[1] = top[1];
		trapezoids[0].left[2] = middle[0];
		trapezoids[0].left[3] = middle[1];
		trapezoids[0].right[0] = top[0];
		trapezoids[0].right[1] = top[1];
		trapezoids[0].right[2] = bottom[0];
		trapezoids[0].right[3] = bottom[1];
		trapezoids[1].left[0] = middle[0];
		trapezoids[1].left[1] = middle[1];
		trapezoids[1].left[2] = bottom[0];
		trapezoids[1].left[3] = bottom[1];
		trapezoids[1].right[0] = top[0];
		trapezoids[1].right[1] = top[1];
		trapezoids[1].right[2] = bottom[0];
		trapezoids[1].right[3] = bottom[1];
	} else {
		trapezoids[0].left[0] = top[0];
		trapezoids[0].left[1] = top[1];
		trapezoids[0].left[2] = bottom[0];
		trapezoids[0].left[3] = bottom[1];
		trapezoids[0].right[0] = top[0];
		trapezoids[0].right[1] = top[1];
		trapezoids[0].right[2] = middle[0];
		trapezoids[0].right[3] = middle[1];
		trapezoids[1].left[0] = top[0];
		trapezoids[1].left[1] = top[1];
		trapezoids[1].left[2] = bottom[0];
		trapezoids[1].left[3] = bottom[1];
		trapezoids[1].right[0] = middle[0];
		trapezoids[1].right[1] = middle[1];
		trapezoids[1].right[2] = bottom[0];
		trapezoids[1].right[3] = bottom[1];
	}
}

/*
 * Gives the whole pixels a trapezoid can touch: from its top to its
 * bottom, and from its left side's leftmost place to its right side's
 * rightmost between them.
 */
static void
render_trapezoid_bounds(
	const struct x11_trapezoid *trapezoid,
	struct x11_box *bounds)
{
	const int32_t *lines[2];
	int64_t places[4];
	int64_t dy;
	int64_t lowest;
	int64_t highest;
	unsigned side;
	unsigned at;

	/* Each side's place at the top and at the bottom (along its line). */
	lines[0] = trapezoid->left;
	lines[1] = trapezoid->right;
	for (side = 0U; side < 2U; side++) {
		dy = (int64_t)lines[side][3] - lines[side][1];
		places[side * 2U] = lines[side][0];
		places[side * 2U + 1U] = lines[side][2];
		if (dy != 0) {
			places[side * 2U] = lines[side][0] + ((int64_t)trapezoid->top - lines[side][1]) * ((int64_t)lines[side][2] - lines[side][0]) / dy;
			places[side * 2U + 1U] = lines[side][0] + ((int64_t)trapezoid->bottom - lines[side][1]) * ((int64_t)lines[side][2] - lines[side][0]) / dy;
		}
	}

	/* The leftmost and rightmost of them. */
	lowest = places[0];
	highest = places[0];
	for (at = 1U; at < 4U; at++) {
		if (places[at] < lowest)
			lowest = places[at];
		if (places[at] > highest)
			highest = places[at];
	}

	/* Whole pixels around them, kept to X's coordinates. */
	bounds->x0 = render_fixed_int(lowest);
	bounds->x1 = render_fixed_int_ceil(highest) + 1;
	bounds->y0 = render_fixed_int(trapezoid->top);
	bounds->y1 = render_fixed_int_ceil(trapezoid->bottom) + 1;
	if (bounds->x0 < -32768)
		bounds->x0 = -32768;
	if (bounds->x1 > 32768)
		bounds->x1 = 32768;
	if (bounds->y0 < -32768)
		bounds->y0 = -32768;
	if (bounds->y1 > 32768)
		bounds->y1 = 32768;
}

/* Gives the whole number at or below a 16.16 value. */
static int
render_fixed_int(
	int64_t value)
{
	int64_t whole;

	/* Towards zero, then down for a negative fraction. */
	whole = value / 65536;
	if (value % 65536 != 0 && value < 0)
		whole--;

	/* Kept to an int. */
	if (whole < -2147483647)
		whole = -2147483647;
	if (whole > 2147483647)
		whole = 2147483647;
	return (int)whole;
}

/* Gives the whole number at or above a 16.16 value. */
static int
render_fixed_int_ceil(
	int64_t value)
{
	/* The floor of the value less the smallest step, plus one. */
	return render_fixed_int(value - 1) + 1;
}

/* Finds a glyph set by an id that names it, or NULL. */
static struct render_glyphset *
render_glyphset_find(
	struct x11server *server,
	uint32_t id)
{
	/* The resource of the kind. */
	return x11_resource_find(server, id, X11_RESOURCE_GLYPHSET);
}

/* Lets one name of a glyph set go: the last frees the set, its glyphs and its charge. */
static void
render_glyphset_drop(
	struct x11server *server,
	struct render_glyphset *set)
{
	size_t slot;

	/* One name fewer; others keep it. */
	if (set->references > 0U)
		set->references--;
	if (set->references > 0U)
		return;

	/* Each glyph. */
	for (slot = 0U; slot < set->capacity; slot++) {
		if (set->slots[slot] == NULL)
			continue;
		free(set->slots[slot]->image.pixels);
		free(set->slots[slot]);
	}

	/* Succeeded: the table, the charge and the set go. */
	free(set->slots);
	x11_memory_release(server, set->owner, set->charged);
	free(set);
}

/* Gives the slot a glyph id is looked for first (the table's capacity is a power of two). */
static size_t
render_glyph_slot(
	const struct render_glyphset *set,
	uint32_t id)
{
	/* A multiplicative hash. */
	return (size_t)((id * 2654435761U) >> 7U) & (set->capacity - 1U);
}

/* Finds a glyph of a set by its id, or NULL. */
static struct render_glyph *
render_glyph_find(
	const struct render_glyphset *set,
	uint32_t id)
{
	size_t slot;

	/* An empty table has none. */
	if (set->capacity == 0U)
		return NULL;

	/* From its first slot to the first empty one. */
	slot = render_glyph_slot(set, id);
	while (set->slots[slot] != NULL) {
		if (set->slots[slot]->id == id)
			return set->slots[slot];
		slot = (slot + 1U) & (set->capacity - 1U);
	}

	/* Not there. */
	return NULL;
}

/*
 * Puts a glyph (whose id the set does not have) into a set's table,
 * which grows to keep under half full.  Returns 0, or -1 without memory.
 */
static int
render_glyph_put(
	struct render_glyphset *set,
	struct render_glyph *glyph)
{
	struct render_glyph **old;
	size_t old_capacity;
	size_t at;
	size_t slot;

	/* Room: a table twice the size, every glyph put again. */
	if ((set->count + 1U) * 2U > set->capacity) {
		old = set->slots;
		old_capacity = set->capacity;
		set->capacity = GLYPHS_FIRST_CAPACITY;
		if (old_capacity != 0U)
			set->capacity = old_capacity * 2U;
		set->slots = calloc(set->capacity, sizeof(*set->slots));
		if (set->slots == NULL) {
			set->slots = old;
			set->capacity = old_capacity;
			return -1;
		}
		for (at = 0U; at < old_capacity; at++) {
			if (old[at] == NULL)
				continue;
			slot = render_glyph_slot(set, old[at]->id);
			while (set->slots[slot] != NULL)
				slot = (slot + 1U) & (set->capacity - 1U);
			set->slots[slot] = old[at];
		}
		free(old);
	}

	/* The first empty slot from its own. */
	slot = render_glyph_slot(set, glyph->id);
	while (set->slots[slot] != NULL)
		slot = (slot + 1U) & (set->capacity - 1U);

	/* Succeeded: kept. */
	set->slots[slot] = glyph;
	set->count++;
	return 0;
}

/*
 * Takes a glyph out of a set's table (it is not freed): the glyphs after
 * it in its run are put again so that each is found from its first slot.
 */
static void
render_glyph_remove(
	struct render_glyphset *set,
	uint32_t id)
{
	struct render_glyph *moved;
	size_t slot;
	size_t next;
	size_t home;

	/* Its slot. */
	if (set->capacity == 0U)
		return;
	slot = render_glyph_slot(set, id);
	while (set->slots[slot] != NULL && set->slots[slot]->id != id)
		slot = (slot + 1U) & (set->capacity - 1U);
	if (set->slots[slot] == NULL)
		return;

	/* Emptied, and each glyph of the run after it put again. */
	set->slots[slot] = NULL;
	set->count--;
	next = (slot + 1U) & (set->capacity - 1U);
	while (set->slots[next] != NULL) {
		moved = set->slots[next];
		set->slots[next] = NULL;
		home = render_glyph_slot(set, moved->id);
		while (set->slots[home] != NULL)
			home = (home + 1U) & (set->capacity - 1U);
		set->slots[home] = moved;
		next = (next + 1U) & (set->capacity - 1U);
	}
}

/* Gives the bytes a glyph is charged as: itself and its pixels. */
static size_t
render_glyph_bytes(
	const struct render_glyph *glyph)
{
	/* The glyph and four bytes a pixel. */
	return sizeof(*glyph) + x11_image_bytes(glyph->image.width, glyph->image.height);
}

/* Gives the bytes of a row of a glyph's image in a request: a bit, a byte or a word a pixel, padded to 32 bits. */
static size_t
render_row_bytes(
	unsigned depth,
	unsigned width)
{
	/* By the depth. */
	if (depth == 1U)
		return (((size_t)width + 31U) / 32U) * 4U;
	if (depth == X11_DEPTH_ALPHA)
		return ((size_t)width + 3U) & ~(size_t)3U;

	/* A word a pixel. */
	return (size_t)width * 4U;
}

/*
 * Makes a mask of the server's own for one request (all 0), charged to
 * the server while it lasts.  Returns 0, or -1 when the server may not
 * hold it or there is no memory.
 */
static int
render_mask_make(
	struct x11server *server,
	struct x11_image *image,
	int width,
	int height,
	unsigned depth)
{
	size_t bytes;
	int error;

	/* The server holds it. */
	memset(image, 0, sizeof(*image));
	bytes = x11_image_bytes(width, height);
	error = x11_memory_charge(server, X11_NO_CLIENT, bytes);
	if (error != 0)
		return -1;

	/* The pixels. */
	error = render_image_make(image, width, height, depth);
	if (error != 0) {
		x11_memory_release(server, X11_NO_CLIENT, bytes);
		return -1;
	}

	/* Succeeded: made. */
	return 0;
}

/* Frees a mask render_mask_make made (one with no pixels: nothing), with its charge. */
static void
render_mask_free(
	struct x11server *server,
	struct x11_image *image)
{
	/* None was made. */
	if (image->pixels == NULL)
		return;

	/* The charge and the pixels. */
	x11_memory_release(server, X11_NO_CLIENT, x11_image_bytes(image->width, image->height));
	free(image->pixels);
	image->pixels = NULL;
}

/* Gives a drawable's picture's drawable on itself: from (0, 0), its size. */
static void
render_drawable_box(
	const struct x11_picture *picture,
	struct x11_box *box)
{
	/* Nothing, or the window's or pixmap's size. */
	box->x0 = 0;
	box->y0 = 0;
	box->x1 = 0;
	box->y1 = 0;
	if (picture->window != NULL) {
		box->x1 = picture->window->width;
		box->y1 = picture->window->height;
	}
	if (picture->pixmap != NULL) {
		box->x1 = picture->pixmap->width;
		box->y1 = picture->pixmap->height;
	}
}
