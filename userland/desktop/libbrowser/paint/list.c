/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display list: a laid out page walked in painting order into
 * rectangles and runs of glyphs, and its text dump for the tests.
 *
 * The walk is CSS 2's painting order simplified: the canvas, then each
 * block's background and borders before its content, a block's children
 * in document order, and a block's lines after its own background.  The
 * positioned boxes are painted apart, each with its descendants that are
 * not positioned, in the order layout_stacking_order gives: those with a
 * negative z-index before the normal flow, the others after it.  A box
 * that clips its overflow puts its content between a clip to its padding
 * box and the clip's end; a positioned box is clipped by the clipping
 * boxes around it that it does not escape (an absolute box escapes those
 * outside its containing block, a fixed one all of them).  A replaced box
 * paints its background and borders, then its image over its content box:
 * a block one as a block does, an inline one where its fragment is.  An
 * inline block is painted as a block in its place among its line's text.
 *
 * The decoration (ws074-p062) is made of the same rectangles, so that both
 * renderers draw it alike: a box with rounded corners fills and borders
 * each pixel row of its corners with a rectangle as wide as the curve at
 * the row's middle (the straight part between them is one rectangle); a
 * shadow is its box's shape spread and offset, blurred by stacking
 * translucent copies grown and shrunk by up to its blur radius; an outline
 * is four rectangles around the border box; and a box's opacity scales the
 * alpha of the rectangles and text painted for it and its content (an
 * image inside keeps its own), a box of no opacity painting nothing.  A
 * clip-path inset() clips the box's painting, its shadows included, to its
 * border box moved in (or out) by the inset.
 */

#include "paint/paint.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* The canvas color when neither the root nor the body has a background: white. */
#define LIST_CANVAS_DEFAULT	0xffffffffU

/* How many spaces a tab stands for in preserved whitespace (as in the layout). */
#define LIST_TAB_SPACES		8

/* The visibility value that hides a box's own painting. */
#define LIST_VISIBILITY_HIDDEN	1

/* An opacity at or under which a box and its content are not painted at all. */
#define LIST_OPACITY_NONE	0.002f

/* How many translucent copies a blurred shadow is stacked from. */
#define LIST_SHADOW_LAYERS	10

/* The most tiles one background image is painted in (a tiny tile over a huge page stops there). */
#define LIST_TILES_MAX		16384U

/* Which part of a block is being painted while inline content is deferred past sibling floats. */
#define LIST_BOX_FULL		0
#define LIST_BOX_DECORATION	1
#define LIST_BOX_INLINE		2

/*
 * The colors of the form controls' own look, as Chromium draws them
 * (0xAARRGGBB): a checkbox's frame and inside, a checked one's fill, and a
 * field's placeholder.
 */
#define LIST_FRAME_COLOR	0xff767676U
#define LIST_FIELD_COLOR	0xffffffffU
#define LIST_CHECKED_COLOR	0xff0075ffU
#define LIST_PLACEHOLDER_COLOR	0xff757575U

/* The character a password field shows for each of its characters. */
#define LIST_PASSWORD_BULLET	0x2022U

/* A select's arrow: how far its left is from the content box's right, its rows and its widest row, in pixels. */
#define LIST_ARROW_RIGHT	14
#define LIST_ARROW_ROWS		5
#define LIST_ARROW_WIDTH	9

/*
 * How far each copy of a blurred shadow is grown, in standard deviations
 * of the blur (a blur radius is two of them): the normal distribution's
 * quantiles at the middles of ten equal shares, so that the share of
 * copies covering a point at a distance outside the shape is the Gaussian
 * blur's there.
 */
static const float list_shadow_quantiles[LIST_SHADOW_LAYERS] = {
	1.645f, 1.036f, 0.674f, 0.385f, 0.126f, -0.126f, -0.385f, -0.674f, -1.036f, -1.645f
};

/*
 * What a walk of the box tree carries: the list being filled, the text
 * system that measures the glyphs, the box whose background went to the
 * canvas (and is not painted again), and the first error.
 */
struct list_walk {
	struct paint_list *list;
	struct text_system *text;
	const struct layout_box *canvas_box;
	layout_unit viewport_width;
	layout_unit viewport_height;
	int error;
};

/*
 * A box's rounded shape in layout units: its rectangle and each corner's
 * radius, horizontal then vertical (CSS_TOP_LEFT and on, clockwise).
 */
struct list_shape {
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
	layout_unit radius[4][2];
};

/*
 * A rectangle of the page in layout units: a background's positioning
 * area or its painting area.
 */
struct list_area {
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
};

static const struct layout_box *list_canvas(const struct layout_tree *tree, uint32_t *color);
static void list_box(struct list_walk *walk, const struct layout_box *box, const struct layout_box *layer, int depth);
static void list_box_part(struct list_walk *walk, const struct layout_box *box, const struct layout_box *layer, int depth, int part);
static void list_box_own(struct list_walk *walk, const struct layout_box *box, const struct layout_box *layer, int depth, int part);
static void list_fade(struct list_walk *walk, size_t first, float opacity);
static int list_border_shape(const struct layout_box *box, struct list_shape *shape);
static void list_round_fill(struct list_walk *walk, const struct list_shape *shape, uint32_t color);
static void list_round_row(struct list_walk *walk, const struct list_shape *shape, layout_unit top, layout_unit bottom, uint32_t color);
static void list_round_span(const struct list_shape *shape, layout_unit y, layout_unit *left, layout_unit *right);
static void list_round_borders(struct list_walk *walk, const struct layout_box *box, const struct list_shape *outer);
static void list_shadows(struct list_walk *walk, const struct layout_box *box);
static void list_shadow(struct list_walk *walk, const struct list_shape *border, const struct css_shadow *shadow);
static void list_grow_shape(const struct list_shape *shape, layout_unit amount, struct list_shape *grown);
static void list_outline(struct list_walk *walk, const struct layout_box *box);
static float list_inline_opacity(const struct layout_box *box);
static int list_clip_path(struct list_walk *walk, const struct layout_box *box);
static void list_borders(struct list_walk *walk, const struct layout_box *box);
static int list_point_borders(struct list_walk *walk, const struct layout_box *box);
static void list_inline_floats(struct list_walk *walk, const struct layout_box *box, const struct layout_box *layer, int depth);
static void list_lines(struct list_walk *walk, const struct layout_box *box, const struct layout_box *layer, int depth);
static void list_fragment(struct list_walk *walk, const struct layout_fragment *fragment, layout_unit x, layout_unit baseline);
static void list_rect(struct list_walk *walk, layout_unit x, layout_unit y, layout_unit width, layout_unit height, uint32_t color);
static void list_replaced(struct list_walk *walk, const struct layout_box *box, layout_unit x, layout_unit y);
static void list_image(struct list_walk *walk, const struct layout_box *box, layout_unit x, layout_unit y);
static void list_control(struct list_walk *walk, const struct layout_box *box);
static int list_control_native(const struct layout_box *box);
static void list_check(struct list_walk *walk, const struct layout_box *box, layout_unit width, layout_unit height);
static void list_control_text(struct list_walk *walk, const struct layout_box *box);
static int list_control_shown(const struct layout_box *box, struct dom_element *element, const struct dom_control *control, struct wb_units *shown, uint32_t *color);
static int list_caret_offset(struct list_walk *walk, const struct text_font *font, const struct layout_box *box, struct dom_element *element, struct dom_control *control, layout_unit *offset);
static void list_textarea(struct list_walk *walk, const struct layout_box *box, struct dom_element *element, const struct list_area *content, const struct text_font *font, layout_unit line, layout_unit ascent);
static void list_select(struct list_walk *walk, const struct layout_box *box, struct dom_element *element, const struct list_area *content, const struct text_font *font, layout_unit line, layout_unit ascent);
static void list_control_record(struct list_walk *walk, const struct layout_box *box, struct dom_control *control, const struct list_area *content, const struct text_font *font, layout_unit text_x, layout_unit caret_x, layout_unit line_top, layout_unit ascent);
static int list_composing(const struct layout_box *box, const struct dom_control *control);
static int list_insert_preedit(const struct dom_control *control, struct wb_units *value);
static void list_preedit_underline(struct list_walk *walk, const struct text_font *font, const struct dom_control *control, layout_unit caret_x, layout_unit baseline, uint32_t color);
static void list_style_font(struct list_walk *walk, const struct css_style *style, struct text_font *font);
static void list_text_run(struct list_walk *walk, const struct text_font *font, uint32_t color, const uint16_t *units, size_t length, layout_unit x, layout_unit baseline);
static void list_image_item(struct list_walk *walk, const struct img_bitmap *image, layout_unit x, layout_unit y, layout_unit width, layout_unit height);
static void list_box_background(struct list_walk *walk, const struct layout_box *box);
static void list_canvas_background(struct list_walk *walk, const struct layout_tree *tree);
static void list_background_image(struct list_walk *walk, const struct layout_box *box, const struct list_area *area, const struct list_area *painting);
static void list_background_size(const struct layout_box *box, const struct img_bitmap *image, const struct list_area *area, layout_unit *width, layout_unit *height);
static layout_unit list_background_length(const struct css_length *length, layout_unit whole);
static layout_unit list_background_offset(const struct css_length *position, layout_unit room);
static void list_clip_rect(struct list_walk *walk, const struct list_area *area);
static void list_clip(struct list_walk *walk, const struct layout_box *box);
static void list_unclip(struct list_walk *walk);
static int list_layer_clips(struct list_walk *walk, const struct layout_box *layer, int push);
static void list_color(struct wb_buffer *out, uint32_t color);

/*
 * Builds the display list of a laid out page.
 */
int
paint_build(
	struct paint_list *list,
	const struct layout_tree *tree)
{
	struct list_walk walk;
	struct wb_vector order;
	const struct layout_box *layer;
	size_t flow_index;
	size_t index;
	int clips;
	int error;

	/* Starts an empty list the size of the document, at least the viewport. */
	memset(list, 0, sizeof(*list));
	wb_arena_init(&list->arena, 0);
	wb_vector_init(&list->items, sizeof(struct paint_item));
	list->width = tree->viewport_width;
	list->height = tree->document_height;
	if (list->height < tree->viewport_height)
		list->height = tree->viewport_height;

	/* The canvas takes the root's background, or the body's. */
	memset(&walk, 0, sizeof(walk));
	walk.list = list;
	walk.text = tree->text;
	walk.canvas_box = list_canvas(tree, &list->canvas_color);
	walk.viewport_width = tree->viewport_width;
	walk.viewport_height = tree->viewport_height;

	/* An empty document paints only the canvas. */
	if (tree->root == NULL)
		return 0;

	/* The canvas's background image, under everything. */
	list_canvas_background(&walk, tree);

	/* The positioned boxes in painting order, and where the normal flow goes among them. */
	wb_vector_init(&order, sizeof(const struct layout_box *));
	error = layout_stacking_order(tree, &order, &flow_index);
	if (error != 0) {
		wb_vector_release(&order);
		paint_release(list);
		return error;
	}

	/* The positioned boxes below the flow, the flow from the root, then the ones above it. */
	for (index = 0; index < order.count; index++) {
		if (index == flow_index)
			list_box(&walk, tree->root, NULL, 0);
		layer = *(const struct layout_box **)wb_vector_at(&order, index);
		clips = list_layer_clips(&walk, layer, 1);
		list_box(&walk, layer, layer, 0);
		while (clips > 0) {
			list_unclip(&walk);
			clips--;
		}
	}

	/* The flow is last when no box is above it. */
	if (flow_index == order.count)
		list_box(&walk, tree->root, NULL, 0);
	wb_vector_release(&order);
	if (walk.error != 0) {
		paint_release(list);
		return walk.error;
	}

	/* Succeeded: the list holds the page's painting. */
	return 0;
}

/*
 * Frees a display list.
 */
void
paint_release(
	struct paint_list *list)
{
	/* The items and the glyphs they point at. */
	wb_vector_release(&list->items);
	wb_arena_release(&list->arena);
	memset(list, 0, sizeof(*list));
}

/*
 * Adds a ring around a rectangle on top of everything painted so far: four
 * filled rectangles of a thickness just outside it (the focus ring of the
 * focused element, ws074-p056).
 */
int
paint_add_ring(
	struct paint_list *list,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height,
	layout_unit thickness,
	uint32_t color)
{
	struct paint_item edges[4];
	size_t index;
	int error;

	/* The four edges, all of one color. */
	memset(edges, 0, sizeof(edges));
	for (index = 0; index < 4U; index++) {
		edges[index].kind = PAINT_RECT;
		edges[index].color = color;
	}

	/* The top edge, across the corners. */
	edges[0].x = x - thickness;
	edges[0].y = y - thickness;
	edges[0].width = width + 2 * thickness;
	edges[0].height = thickness;

	/* The bottom edge, across the corners. */
	edges[1].x = x - thickness;
	edges[1].y = y + height;
	edges[1].width = width + 2 * thickness;
	edges[1].height = thickness;

	/* The left edge, between the top and the bottom. */
	edges[2].x = x - thickness;
	edges[2].y = y;
	edges[2].width = thickness;
	edges[2].height = height;

	/* The right edge, between the top and the bottom. */
	edges[3].x = x + width;
	edges[3].y = y;
	edges[3].width = thickness;
	edges[3].height = height;

	/* The items, last in the list so they are drawn over the page. */
	for (index = 0; index < 4U; index++) {
		error = wb_vector_push(&list->items, &edges[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the ring is in the list. */
	return 0;
}

/*
 * Adds a filled rectangle on top of everything painted so far (the caret
 * of a focused text control, ws074-p032).
 */
int
paint_add_rect(
	struct paint_list *list,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height,
	uint32_t color)
{
	struct paint_item item;
	int error;

	/* The rectangle. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_RECT;
	item.x = x;
	item.y = y;
	item.width = width;
	item.height = height;
	item.color = color;

	/* The item, last in the list so it is drawn over the page. */
	error = wb_vector_push(&list->items, &item);
	if (error != 0)
		return error;

	/* Succeeded: the rectangle is in the list. */
	return 0;
}

/*
 * Writes a display list as text, one item a line, in pixels.
 */
int
paint_dump(
	const struct paint_list *list,
	struct wb_buffer *out)
{
	const struct paint_item *item;
	size_t index;
	size_t glyph;
	int error;

	/* The canvas: its size and color. */
	wb_buffer_printf(out, "canvas %.2f x %.2f ", (double)layout_to_px(list->width), (double)layout_to_px(list->height));
	list_color(out, list->canvas_color);
	wb_buffer_append_string(out, "\n");

	/* Each item in painting order. */
	for (index = 0; index < list->items.count; index++) {
		item = wb_vector_at(&list->items, index);

		/* A clip: the rectangle the items until its end are drawn inside. */
		if (item->kind == PAINT_CLIP) {
			wb_buffer_printf(out, "clip %.2f %.2f %.2f %.2f\n", (double)layout_to_px(item->x), (double)layout_to_px(item->y),
			    (double)layout_to_px(item->width), (double)layout_to_px(item->height));
			continue;
		}

		/* The end of the last clip. */
		if (item->kind == PAINT_UNCLIP) {
			wb_buffer_append_string(out, "unclip\n");
			continue;
		}

		/* A rectangle: its position, size and color. */
		if (item->kind == PAINT_RECT) {
			wb_buffer_printf(out, "rect %.2f %.2f %.2f %.2f ", (double)layout_to_px(item->x), (double)layout_to_px(item->y),
			    (double)layout_to_px(item->width), (double)layout_to_px(item->height));
			list_color(out, item->color);
			wb_buffer_append_string(out, "\n");
			continue;
		}

		/* An image: its rectangle and its image's size. */
		if (item->kind == PAINT_IMAGE) {
			wb_buffer_printf(out, "image %.2f %.2f %.2f %.2f %dx%d\n", (double)layout_to_px(item->x), (double)layout_to_px(item->y),
			    (double)layout_to_px(item->width), (double)layout_to_px(item->height), item->image->width, item->image->height);
			continue;
		}

		/* A text run: its origin, font, color and characters. */
		wb_buffer_printf(out, "text %.2f %.2f %upx face %d bold %d ", (double)layout_to_px(item->x),
		    (double)layout_to_px(item->y), item->font.pixels, item->font.face, item->font.bold);
		list_color(out, item->color);
		wb_buffer_append_string(out, " \"");
		for (glyph = 0; glyph < item->glyph_count; glyph++)
			wb_buffer_append_utf8(out, item->glyphs[glyph].code_point);
		wb_buffer_append_string(out, "\"\n");
	}

	/* Reports a buffer that could not grow. */
	error = wb_buffer_reserve(out, 1);
	if (error != 0)
		return error;

	/* Succeeded: the dump is written. */
	return 0;
}

/*
 * Starts a clip stack for a target of a size: only the whole target.
 */
void
paint_clips_init(
	struct paint_clips *clips,
	int width,
	int height)
{
	struct paint_clip *whole;

	/* The target's rectangle is the first clip. */
	memset(clips, 0, sizeof(*clips));
	whole = &clips->stack[0];
	whole->right = (float)width;
	whole->bottom = (float)height;
	whole->pixel_right = width;
	whole->pixel_bottom = height;
}

/*
 * Starts a clip (a PAINT_CLIP item, the document scrolled up by
 * scroll_y): the top of the stack becomes its intersection with the clip
 * around it.
 */
void
paint_clips_push(
	struct paint_clips *clips,
	const struct paint_item *item,
	layout_unit scroll_y)
{
	const struct paint_clip *outer;
	struct paint_clip *inner;
	float value;

	/* A clip past the stack's depth is only counted. */
	if (clips->depth >= PAINT_CLIP_DEPTH) {
		clips->ignored++;
		return;
	}

	/* The new clip starts as the one around it. */
	outer = &clips->stack[clips->depth];
	clips->depth++;
	inner = &clips->stack[clips->depth];
	*inner = *outer;

	/* Each edge moves in to the item's edge when that is further in. */
	value = layout_to_px(item->x);
	if (value > inner->left)
		inner->left = value;
	value = layout_to_px(item->y - scroll_y);
	if (value > inner->top)
		inner->top = value;
	value = layout_to_px(item->x + item->width);
	if (value < inner->right)
		inner->right = value;
	value = layout_to_px(item->y - scroll_y + item->height);
	if (value < inner->bottom)
		inner->bottom = value;

	/* The same edges on whole pixels (rounded to the nearest), for glyphs. */
	inner->pixel_left = (int)floorf(inner->left + 0.5f);
	inner->pixel_top = (int)floorf(inner->top + 0.5f);
	inner->pixel_right = (int)floorf(inner->right + 0.5f);
	inner->pixel_bottom = (int)floorf(inner->bottom + 0.5f);
}

/*
 * Ends the last clip started (a PAINT_UNCLIP item).
 */
void
paint_clips_pop(
	struct paint_clips *clips)
{
	/* A clip past the depth was only counted. */
	if (clips->ignored > 0) {
		clips->ignored--;
		return;
	}

	/* The clip around it is the top again. */
	if (clips->depth > 0)
		clips->depth--;
}

/*
 * Reports the clip the items are drawn inside now.
 */
const struct paint_clip *
paint_clips_top(
	const struct paint_clips *clips)
{
	/* The top of the stack. */
	return &clips->stack[clips->depth];
}

/*
 * Finds the canvas color: the root's background, or when the root has
 * none, the body's.  Reports the box whose background it took, so the walk
 * does not paint that background again, or NULL.
 */
static const struct layout_box *
list_canvas(
	const struct layout_tree *tree,
	uint32_t *color)
{
	const struct layout_box *body;
	const struct dom_element *element;
	int is_body;

	/* White unless a background is found. */
	*color = LIST_CANVAS_DEFAULT;
	if (tree->root == NULL)
		return NULL;

	/* The root's own background (a color or an image) wins. */
	if ((tree->root->style.background_color >> 24) != 0 || tree->root->background != NULL) {
		*color = tree->root->style.background_color;
		if ((*color >> 24) == 0)
			*color = LIST_CANVAS_DEFAULT;
		return tree->root;
	}

	/* Otherwise the body's, when the root's first block is the body. */
	body = tree->root->first_child;
	while (body != NULL && body->node == NULL)
		body = body->next;
	if (body == NULL || body->node->type != DOM_ELEMENT)
		return NULL;

	/* Only the body element gives the canvas its background. */
	element = (const struct dom_element *)body->node;
	is_body = vm_string_equal_ascii(element->local_name, "body");
	if (!is_body)
		return NULL;

	/* A body without a background leaves the canvas white. */
	if ((body->style.background_color >> 24) == 0 && body->background == NULL)
		return NULL;

	/* The body's background is the canvas's. */
	*color = body->style.background_color;
	if ((*color >> 24) == 0)
		*color = LIST_CANVAS_DEFAULT;
	return body;
}

/*
 * Adds a box's painting and its descendants' to the list, its outline over
 * them and its opacity applied to them all; positioned boxes other than
 * the layer being painted wait for their turn.
 */
static void
list_box(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct layout_box *layer,
	int depth)
{
	/* The complete box in its normal turn. */
	list_box_part(walk, box, layer, depth, LIST_BOX_FULL);
}

/* Adds all or one painting-order part of a block and applies its common clipping, outline and opacity. */
static void
list_box_part(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct layout_box *layer,
	int depth,
	int part)
{
	size_t first;
	int positioned;
	int clipped;

	/* Stops at the depth the layout stops at, or after an error. */
	if (depth > LAYOUT_DEPTH_MAX || walk->error != 0)
		return;

	/* Only blocks paint a box of their own; inline content is painted through their lines. */
	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return;

	/* A positioned box (not the root) is painted in its own turn. */
	positioned = layout_is_positioned(box);
	if (positioned && box != layer && box->parent != NULL)
		return;

	/* A box of no opacity paints nothing, nor does its content. */
	if (box->style.opacity <= LIST_OPACITY_NONE)
		return;

	/* The box and its content inside its clip-path, then its outline over them. */
	first = walk->list->items.count;
	clipped = list_clip_path(walk, box);
	list_box_own(walk, box, layer, depth, part);
	if (part != LIST_BOX_DECORATION && box->style.visibility != LIST_VISIBILITY_HIDDEN)
		list_outline(walk, box);
	if (clipped)
		list_unclip(walk);

	/* A translucent box fades what was painted for it. */
	if (box->style.opacity < 1.0f)
		list_fade(walk, first, box->style.opacity);
}

/* Starts the clip of a box's clip-path inset(), and reports whether there is one to end. */
static int
list_clip_path(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct list_area area;
	layout_unit inset[4];
	layout_unit width;
	layout_unit height;
	layout_unit whole;
	int side;

	/* A box without an inset is not clipped. */
	if (!box->style.clip_inset)
		return 0;

	/* The border box, which the inset moves in from. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* Each side's inset: pixels, or a percentage of the height (top, bottom) or the width (left, right). */
	for (side = 0; side < 4; side++) {
		whole = width;
		if (side == CSS_TOP || side == CSS_BOTTOM)
			whole = height;
		inset[side] = 0;
		if (box->style.clip[side].unit == CSS_UNIT_PX)
			inset[side] = layout_from_px(box->style.clip[side].value);
		if (box->style.clip[side].unit == CSS_UNIT_PERCENT)
			inset[side] = (layout_unit)((float)whole * box->style.clip[side].value / 100.0f) + layout_from_px(box->style.clip[side].offset);
	}

	/* The clip's rectangle. */
	area.x = box->x + inset[CSS_LEFT];
	area.y = box->y + inset[CSS_TOP];
	area.width = width - inset[CSS_LEFT] - inset[CSS_RIGHT];
	area.height = height - inset[CSS_TOP] - inset[CSS_BOTTOM];
	if (area.width < 0)
		area.width = 0;
	if (area.height < 0)
		area.height = 0;
	list_clip_rect(walk, &area);

	/* The clip is started. */
	return 1;
}

/* Adds a box's own painting (its shadows, background, borders and image or content) to the list. */
static void
list_box_own(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct layout_box *layer,
	int depth,
	int part)
{
	const struct layout_box *child;
	struct list_shape shape;
	layout_unit width;
	layout_unit height;
	int rounded;
	int visible;
	int clips;
	int positioned;

	/* A hidden box paints nothing of its own, but its children may be visible. */
	visible = 1;
	if (box->style.visibility == LIST_VISIBILITY_HIDDEN)
		visible = 0;

	/* A form control standing as a block draws itself from its state. */
	if (part != LIST_BOX_INLINE && visible && box->control != DOM_CONTROL_NONE) {
		list_control(walk, box);
		return;
	}

	/* The shadows go under the box. */
	if (part != LIST_BOX_INLINE && visible)
		list_shadows(walk, box);

	/* The background fills the border box, rounded as its corners are, unless it went to the canvas. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	rounded = list_border_shape(box, &shape);
	if (part != LIST_BOX_INLINE && visible && box != walk->canvas_box && (box->style.background_color >> 24) != 0) {
		if (rounded) {
			list_round_fill(walk, &shape, box->style.background_color);
		} else {
			list_rect(walk, box->x, box->y, width, height, box->style.background_color);
		}
	}

	/* The background image over the color, unless it went to the canvas. */
	if (part != LIST_BOX_INLINE && visible && box != walk->canvas_box)
		list_box_background(walk, box);

	/* The borders go over the background. */
	if (part != LIST_BOX_INLINE && visible && rounded) {
		list_round_borders(walk, box, &shape);
	} else if (part != LIST_BOX_INLINE && visible) {
		list_borders(walk, box);
	}

	/* A replaced block's image fills its content box. */
	if (part != LIST_BOX_INLINE && visible && box->replaced) {
		list_image(walk, box, box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT], box->y + box->border[CSS_TOP] + box->padding[CSS_TOP]);
		return;
	}

	/* The decoration pass stops before the content, which is painted after sibling floats. */
	if (part == LIST_BOX_DECORATION)
		return;

	/* A box that clips its overflow clips its content to its padding box. */
	clips = layout_clips(box);
	if (clips)
		list_clip(walk, box);

	/* A block of lines paints the floats among its content, then its text and inline blocks. */
	if (box->children_inline) {
		list_inline_floats(walk, box, layer, depth + 1);
		list_lines(walk, box, layer, depth + 1);
	} else {
		/* Block decorations precede floats; their inline contents follow the floats. */
		for (child = box->first_child; child != NULL; child = child->next) {
			if (child->floating != CSS_FLOAT_NONE)
				continue;
			positioned = layout_is_positioned(child);
			if (child->children_inline && !positioned && child->style.opacity >= 1.0f) {
				list_box_part(walk, child, layer, depth + 1, LIST_BOX_DECORATION);
			} else {
				list_box(walk, child, layer, depth + 1);
			}
		}

		/* Then the floats, over the backgrounds of the blocks beside them. */
		for (child = box->first_child; child != NULL; child = child->next) {
			if (child->floating != CSS_FLOAT_NONE)
				list_box(walk, child, layer, depth + 1);
		}

		/* Inline contents of the in-flow blocks paint over those floats. */
		for (child = box->first_child; child != NULL; child = child->next) {
			positioned = layout_is_positioned(child);
			if (child->floating == CSS_FLOAT_NONE && child->children_inline && !positioned && child->style.opacity >= 1.0f)
				list_box_part(walk, child, layer, depth + 1, LIST_BOX_INLINE);
		}
	}

	/* The clip ends with the content. */
	if (clips)
		list_unclip(walk);
}

/* Paints the floats among a block's inline content (inline boxes are searched through). */
static void
list_inline_floats(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct layout_box *layer,
	int depth)
{
	const struct layout_box *child;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return;

	/*
	 * A float paints itself; an inline box is searched; a box out of the
	 * flow is painted in its own turn, and an inline block with its line.
	 */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->atomic)
			continue;
		if (child->floating != CSS_FLOAT_NONE) {
			list_box(walk, child, layer, depth + 1);
			continue;
		}

		/* An inline box's content. */
		list_inline_floats(walk, child, layer, depth + 1);
	}
}

/*
 * Adds a box's borders as four rectangles: the top and bottom across the
 * whole border box, the left and right between them.
 */
static void
list_borders(
	struct list_walk *walk,
	const struct layout_box *box)
{
	layout_unit width;
	layout_unit height;
	layout_unit inner;
	int pointed;

	/* Four borders around an empty point meet diagonally instead of covering one another as bands. */
	pointed = list_point_borders(walk, box);
	if (pointed)
		return;

	/* The border box, and the height between the top and bottom borders. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	inner = height - box->border[CSS_TOP] - box->border[CSS_BOTTOM];

	/* The top border. */
	if (box->border[CSS_TOP] > 0)
		list_rect(walk, box->x, box->y, width, box->border[CSS_TOP], box->style.border_color[CSS_TOP]);

	/* The bottom border. */
	if (box->border[CSS_BOTTOM] > 0) {
		list_rect(
			walk,
			box->x,
			box->y + height - box->border[CSS_BOTTOM],
			width,
			box->border[CSS_BOTTOM],
			box->style.border_color[CSS_BOTTOM]);
	}

	/* The left border. */
	if (box->border[CSS_LEFT] > 0 && inner > 0)
		list_rect(walk, box->x, box->y + box->border[CSS_TOP], box->border[CSS_LEFT], inner, box->style.border_color[CSS_LEFT]);

	/* The right border. */
	if (box->border[CSS_RIGHT] > 0 && inner > 0) {
		list_rect(
			walk,
			box->x + width - box->border[CSS_RIGHT],
			box->y + box->border[CSS_TOP],
			box->border[CSS_RIGHT],
			inner,
			box->style.border_color[CSS_RIGHT]);
	}
}

/*
 * Paints borders whose padding box is a point as four triangles, one
 * pixel row at a time.  This is the CSS border construction used for
 * arrows and other generated shapes; ordinary boxes use the rectangular
 * fast path above.
 */
static int
list_point_borders(
	struct list_walk *walk,
	const struct layout_box *box)
{
	layout_unit width;
	layout_unit height;
	layout_unit row;
	layout_unit row_height;
	layout_unit left;
	layout_unit right;
	layout_unit distance;
	layout_unit span;
	uint32_t middle;

	/* Only a zero-sized padding box has all four border edges meet at one point. */
	if (box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] != 0)
		return 0;
	if (box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] != 0)
		return 0;

	/* Its outer dimensions; an entirely empty border needs no special painting. */
	width = box->border[CSS_LEFT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->border[CSS_BOTTOM];
	if (width <= 0 || height <= 0)
		return 0;

	/* Each row is split at the diagonals from the outer corners to the padding point. */
	for (row = 0; row < height; row += LAYOUT_UNIT) {
		row_height = LAYOUT_UNIT;
		if (row + row_height > height)
			row_height = height - row;

		/* Above the point the top border narrows; below it the bottom border widens. */
		if (row < box->border[CSS_TOP]) {
			distance = row;
			span = box->border[CSS_TOP];
			left = (layout_unit)((int64_t)box->border[CSS_LEFT] * distance / span);
			right = width - (layout_unit)((int64_t)box->border[CSS_RIGHT] * distance / span);
			middle = box->style.border_color[CSS_TOP];
		} else {
			distance = row - box->border[CSS_TOP];
			span = box->border[CSS_BOTTOM];
			left = box->border[CSS_LEFT] - (layout_unit)((int64_t)box->border[CSS_LEFT] * distance / span);
			right = box->border[CSS_LEFT] + (layout_unit)((int64_t)box->border[CSS_RIGHT] * distance / span);
			middle = box->style.border_color[CSS_BOTTOM];
		}

		/* The left, vertical, and right triangles partition the row without overlap. */
		list_rect(walk, box->x, box->y + row, left, row_height, box->style.border_color[CSS_LEFT]);
		list_rect(walk, box->x + left, box->y + row, right - left, row_height, middle);
		list_rect(walk, box->x + right, box->y + row, width - right, row_height, box->style.border_color[CSS_RIGHT]);
	}

	/* The point-border path painted the box. */
	return 1;
}

/* Adds the text of a block's lines, and the inline blocks on them. */
static void
list_lines(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct layout_box *layer,
	int depth)
{
	const struct layout_line *line;
	const struct layout_fragment *fragment;
	layout_unit left;
	layout_unit top;
	size_t index;
	size_t item;

	/* The content box's origin, which the lines are placed from. */
	left = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	top = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];

	/* Each fragment of each line, on the line's baseline moved by its vertical alignment. */
	for (index = 0; index < box->line_count; index++) {
		line = &box->lines[index];
		for (item = 0; item < line->fragment_count; item++) {
			fragment = &line->fragments[item];

			/* An inline block paints itself where the line put it. */
			if (fragment->box->atomic) {
				list_box(walk, fragment->box, layer, depth);
				continue;
			}

			/* Text and replaced boxes. */
			list_fragment(walk, fragment, left + line->left + fragment->x, top + line->y + line->baseline + fragment->shift);
		}
	}
}

/* Adds a fragment's glyphs as a text item, and its underline. */
static void
list_fragment(
	struct list_walk *walk,
	const struct layout_fragment *fragment,
	layout_unit x,
	layout_unit baseline)
{
	struct paint_item item;
	struct paint_glyph *glyphs;
	struct text_glyph glyph;
	uint32_t code_point;
	layout_unit pen;
	layout_unit offset;
	layout_unit thickness;
	size_t count;
	size_t used;
	size_t position;
	float opacity;
	int error;

	/* A fragment of a hidden box paints nothing. */
	if (fragment->box->style.visibility == LIST_VISIBILITY_HIDDEN)
		return;

	/* An inline replaced box's margin box stands on the baseline. */
	if (fragment->box->kind == LAYOUT_REPLACED) {
		list_replaced(walk, fragment->box, x, baseline - fragment->ascent);
		return;
	}

	/* There are at most as many glyphs as UTF-16 units. */
	glyphs = NULL;
	if (fragment->length != 0) {
		glyphs = wb_arena_alloc(&walk->list->arena, fragment->length * sizeof(struct paint_glyph));
		if (glyphs == NULL) {
			walk->error = ENOMEM;
			return;
		}
	}

	/* Sets each character's glyph at the pen, which moves by the advances the layout measured. */
	count = 0;
	pen = 0;
	position = 0;
	while (position < fragment->length) {
		used = wb_utf16_decode(fragment->text + position, fragment->length - position, &code_point);
		position += used;

		/* A preserved tab is as wide as several spaces and draws nothing. */
		if (code_point == 0x09U) {
			error = text_glyph(walk->text, &fragment->font, 0x20U, 0, &glyph);
			if (error != 0) {
				walk->error = error;
				return;
			}

			/* Moves the pen past the tab. */
			pen += (layout_unit)glyph.advance_units * LIST_TAB_SPACES;
			continue;
		}

		/* Measures the glyph. */
		error = text_glyph(walk->text, &fragment->font, code_point, 0, &glyph);
		if (error != 0) {
			walk->error = error;
			return;
		}

		/* Places it and moves the pen on. */
		glyphs[count].code_point = code_point;
		glyphs[count].x = pen;
		count++;
		pen += (layout_unit)glyph.advance_units;
	}

	/* The text item, faded by the inline boxes around it (the block's own opacity fades it with the block). */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_TEXT;
	item.x = x;
	item.y = baseline;
	item.width = fragment->width;
	item.color = fragment->color;
	opacity = list_inline_opacity(fragment->box);
	if (opacity < 1.0f)
		item.color = (item.color & 0x00ffffffU) | ((uint32_t)((float)(item.color >> 24) * opacity + 0.5f) << 24);
	item.font = fragment->font;
	item.glyphs = glyphs;
	item.glyph_count = count;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0) {
		walk->error = error;
		return;
	}

	/* An underline runs under the fragment, a tenth of the size below the baseline. */
	if (fragment->underline) {
		offset = layout_from_px(fragment->font.size / 10.0f + 0.5f);
		offset = (offset / LAYOUT_UNIT) * LAYOUT_UNIT;
		if (offset < LAYOUT_UNIT)
			offset = LAYOUT_UNIT;
		thickness = layout_from_px(fragment->font.size / 16.0f + 0.5f);
		thickness = (thickness / LAYOUT_UNIT) * LAYOUT_UNIT;
		if (thickness < LAYOUT_UNIT)
			thickness = LAYOUT_UNIT;
		list_rect(walk, x, baseline + offset, fragment->width, thickness, item.color);
	}
}

/*
 * Paints an inline replaced box whose margin box starts at a point: its
 * background, its borders and its image.
 */
static void
list_replaced(
	struct list_walk *walk,
	const struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	struct layout_box placed;
	layout_unit width;
	layout_unit height;

	/* The box as if its border box were placed there (the layout places only blocks). */
	placed = *box;
	placed.x = x + box->margin[CSS_LEFT];
	placed.y = y + box->margin[CSS_TOP];

	/* A form control draws itself from its state. */
	if (box->control != DOM_CONTROL_NONE) {
		list_control(walk, &placed);
		return;
	}

	/* The background under the border box. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	if ((box->style.background_color >> 24) != 0)
		list_rect(walk, placed.x, placed.y, width, height, box->style.background_color);

	/* The background image and borders, then the replaced image in the content box. */
	list_box_background(walk, &placed);
	list_borders(walk, &placed);
	list_image(walk, box, placed.x + box->border[CSS_LEFT] + box->padding[CSS_LEFT], placed.y + box->border[CSS_TOP] + box->padding[CSS_TOP]);
}

/*
 * Paints a form control whose border box is placed (ws074-p032): its
 * frame, then its text, and it records where it drew its text and where
 * the caret goes in the control's state for the page.
 *
 * A control keeps the look the user agent's sheet gives it -- all four
 * borders inset or outset -- is drawn the way Chromium's controls look: a
 * one pixel frame in the border's color around the background.  One the
 * page styled otherwise is drawn from its style like any box.
 */
static void
list_control(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct list_area painting;
	struct list_area area;
	layout_unit width;
	layout_unit height;
	layout_unit pixel;
	int native;

	/* The border box. */
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* A checkbox or a radio button is drawn as the platform draws one. */
	if (box->control == DOM_CONTROL_CHECKBOX || box->control == DOM_CONTROL_RADIO) {
		list_check(walk, box, width, height);
		return;
	}

	/* The background color under the border box. */
	if ((box->style.background_color >> 24) != 0)
		list_rect(walk, box->x, box->y, width, height, box->style.background_color);

	/* The background image, placed in the padding box and painted over the border box. */
	painting.x = box->x;
	painting.y = box->y;
	painting.width = width;
	painting.height = height;
	area.x = box->x + box->border[CSS_LEFT];
	area.y = box->y + box->border[CSS_TOP];
	area.width = box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT];
	area.height = box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM];
	list_background_image(walk, box, &area, &painting);

	/* The platform's one pixel frame, or the style's borders. */
	native = list_control_native(box);
	if (native) {
		pixel = LAYOUT_UNIT;
		list_rect(walk, box->x, box->y, width, pixel, box->style.border_color[CSS_TOP]);
		list_rect(walk, box->x, box->y + height - pixel, width, pixel, box->style.border_color[CSS_BOTTOM]);
		list_rect(walk, box->x, box->y + pixel, pixel, height - 2 * pixel, box->style.border_color[CSS_LEFT]);
		list_rect(walk, box->x + width - pixel, box->y + pixel, pixel, height - 2 * pixel, box->style.border_color[CSS_RIGHT]);
	} else {
		list_borders(walk, box);
	}

	/* The text in the content box. */
	list_control_text(walk, box);
}

/* Tells whether a control keeps the user agent's look: all four borders inset (a field) or outset (a button). */
static int
list_control_native(
	const struct layout_box *box)
{
	int side;
	int style;

	/* Every side must be inset or outset. */
	for (side = 0; side < 4; side++) {
		style = box->style.border_style[side];
		if (style != CSS_BORDER_INSET && style != CSS_BORDER_OUTSET)
			return 0;
	}

	/* The user agent's look. */
	return 1;
}

/*
 * Draws a checkbox or a radio button in its border box: a white square
 * with a gray frame, filled with blue and marked in white when it is
 * checked, as Chromium's look.
 */
static void
list_check(
	struct list_walk *walk,
	const struct layout_box *box,
	layout_unit width,
	layout_unit height)
{
	const struct dom_element *element;
	layout_unit pixel;
	layout_unit mark;
	int checked;

	/* Whether it is checked. */
	checked = 0;
	if (box->node != NULL && box->node->type == DOM_ELEMENT) {
		element = (const struct dom_element *)box->node;
		checked = dom_control_checked(element);
	}

	/* The frame and the inside: gray around white, or blue all over when checked. */
	pixel = LAYOUT_UNIT;
	if (checked) {
		list_rect(walk, box->x, box->y, width, height, LIST_CHECKED_COLOR);
	} else {
		list_rect(walk, box->x, box->y, width, height, LIST_FRAME_COLOR);
		list_rect(walk, box->x + pixel, box->y + pixel, width - 2 * pixel, height - 2 * pixel, LIST_FIELD_COLOR);
	}

	/* A checked control's white mark in its middle, a third of its size. */
	if (checked) {
		mark = width / 3;
		list_rect(walk, box->x + (width - mark) / 2, box->y + (height - mark) / 2, mark, mark, LIST_FIELD_COLOR);
	}
}

/*
 * Draws a control's text in its content box and records where it went: a
 * field's value (bullets for a password, the placeholder in gray when it
 * is empty) scrolled to keep the caret in view and clipped to the content
 * box, a button's label centered, a textarea's lines from the top.
 */
static void
list_control_text(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct dom_element *element;
	struct dom_control *control;
	struct list_area content;
	struct text_font font;
	struct wb_units shown;
	layout_unit line;
	layout_unit ascent;
	layout_unit width;
	layout_unit caret;
	layout_unit indent;
	layout_unit left;
	layout_unit top;
	uint32_t color;
	int editable;
	int composing;
	int error;

	/* Only an element's control has text. */
	if (walk->error != 0 || box->node == NULL || box->node->type != DOM_ELEMENT)
		return;
	element = (struct dom_element *)box->node;

	/* The content box. */
	content.x = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	content.y = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];
	content.width = box->width;
	content.height = box->height;
	indent = layout_from_px(box->style.text_indent.value);
	if (box->style.text_indent.unit == CSS_UNIT_PERCENT) {
		indent = (layout_unit)((float)content.width * box->style.text_indent.value / 100.0f) +
		    layout_from_px(box->style.text_indent.offset);
	}

	/* The font and the line the text is set in. */
	list_style_font(walk, &box->style, &font);
	error = layout_control_line(walk->text, &box->style, &line, &ascent);
	if (error != 0) {
		walk->error = error;
		return;
	}

	/* A textarea's lines go their own way. */
	if (box->control == DOM_CONTROL_TEXTAREA) {
		list_textarea(walk, box, element, &content, &font, line, ascent);
		return;
	}

	/* So does a select's chosen option and its arrow. */
	if (box->control == DOM_CONTROL_SELECT) {
		list_select(walk, box, element, &content, &font, line, ascent);
		return;
	}

	/* A field keeps its state for the caret; a button only shows its label. */
	editable = 0;
	if (box->control == DOM_CONTROL_TEXT || box->control == DOM_CONTROL_PASSWORD)
		editable = 1;
	control = NULL;
	if (editable) {
		control = dom_control_of(element);
		if (control == NULL) {
			walk->error = ENOMEM;
			return;
		}
	}

	/* The text shown, and its color. */
	wb_units_init(&shown);
	color = box->style.color;
	error = list_control_shown(box, element, control, &shown, &color);
	if (error == 0)
		error = layout_units_width(walk->text, &font, shown.data, shown.length, &width);
	if (error != 0) {
		wb_units_release(&shown);
		walk->error = error;
		return;
	}

	/* The line is centered in the content box's height. */
	top = content.y + (content.height - line) / 2;

	/* A button's label is placed by text-align (centered by the user agent's sheet). */
	if (!editable) {
		left = content.x + indent;
		if (box->style.text_align == CSS_TEXT_ALIGN_CENTER)
			left = content.x + indent + (content.width - indent - width) / 2;
		if (box->style.text_align == CSS_TEXT_ALIGN_RIGHT)
			left = content.x + content.width - width;
		list_text_run(walk, &font, color, shown.data, shown.length, left, top + ascent);
		wb_units_release(&shown);
		return;
	}

	/* The caret's place in the text (at the start while the placeholder shows). */
	error = list_caret_offset(walk, &font, box, element, control, &caret);
	if (error != 0) {
		wb_units_release(&shown);
		walk->error = error;
		return;
	}

	/* The scroll keeps the caret inside the content box, and no more of the box empty than it must. */
	if (indent + caret - control->scroll_x > content.width - LAYOUT_UNIT)
		control->scroll_x = indent + caret - content.width + LAYOUT_UNIT;
	if (indent + caret < control->scroll_x)
		control->scroll_x = indent + caret;
	if (control->scroll_x < 0)
		control->scroll_x = 0;
	if (control->scroll_x > 0 && indent + width - control->scroll_x < content.width - LAYOUT_UNIT) {
		control->scroll_x = indent + width - content.width + LAYOUT_UNIT;
		if (control->scroll_x < 0)
			control->scroll_x = 0;
	}

	/* The text, scrolled and clipped to the content box, with what an input method composes underlined. */
	list_clip_rect(walk, &content);
	list_text_run(walk, &font, color, shown.data, shown.length, content.x + indent - control->scroll_x, top + ascent);
	composing = list_composing(box, control);
	if (composing)
		list_preedit_underline(walk, &font, control, content.x + indent + caret - control->scroll_x, top + ascent, color);
	list_unclip(walk);
	wb_units_release(&shown);

	/*
	 * What the page needs of this drawing: drawn says the rest is current,
	 * and the caret stands at its offset, as tall as the font's glyphs.
	 */
	list_control_record(walk, box, control, &content, &font, content.x + indent,
	    content.x + indent + caret - control->scroll_x, top, ascent);
}

/*
 * Writes the text a field or a button shows: a button's label, a field's
 * value (a password's as bullets), or the placeholder in gray when the
 * field is empty.
 */
static int
list_control_shown(
	const struct layout_box *box,
	struct dom_element *element,
	const struct dom_control *control,
	struct wb_units *shown,
	uint32_t *color)
{
	struct vm_string *placeholder;
	struct wb_units value;
	size_t index;
	uint16_t unit;
	int composing;
	int error;

	/* A button shows its label. */
	if (control == NULL) {
		error = dom_control_label(element, shown);
		return error;
	}

	/* A field's value. */
	wb_units_init(&value);
	error = dom_control_value(element, &value);
	if (error != 0) {
		wb_units_release(&value);
		return error;
	}

	/* While an input method composes, the value shows with the composed text at the caret (and no placeholder). */
	composing = list_composing(box, control);
	if (composing) {
		error = list_insert_preedit(control, &value);
		if (error != 0) {
			wb_units_release(&value);
			return error;
		}

		/* The composed value is what shows. */
		error = wb_units_append(shown, value.data, value.length);
		wb_units_release(&value);
		if (error != 0)
			return error;

		/* Succeeded: the composed value is written. */
		return 0;
	}

	/* An empty field shows its placeholder in gray. */
	placeholder = dom_attribute_ascii(element, "placeholder");
	if (value.length == 0 && placeholder != NULL) {
		wb_units_release(&value);
		*color = LIST_PLACEHOLDER_COLOR;
		for (index = 0; index < placeholder->length; index++) {
			unit = vm_string_at(placeholder, index);
			error = wb_units_append(shown, &unit, 1);
			if (error != 0)
				return error;
		}

		/* The placeholder is all that shows. */
		return 0;
	}

	/* A password shows a bullet for each unit, anything else its value. */
	if (box->control == DOM_CONTROL_PASSWORD) {
		unit = LIST_PASSWORD_BULLET;
		for (index = 0; index < value.length && error == 0; index++)
			error = wb_units_append(shown, &unit, 1);
	} else {
		error = wb_units_append(shown, value.data, value.length);
	}

	/* The value is no longer needed. */
	wb_units_release(&value);
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/*
 * Measures where a field's caret is from its text's start: the width of the
 * value (or its bullets) before the caret; 0 while the field is empty.
 */
static int
list_caret_offset(
	struct list_walk *walk,
	const struct text_font *font,
	const struct layout_box *box,
	struct dom_element *element,
	struct dom_control *control,
	layout_unit *offset)
{
	struct wb_units value;
	size_t index;
	uint16_t bullet;
	layout_unit one;
	layout_unit into;
	int composing;
	int error;

	/* The value, and a caret that fell past its end moves back to it. */
	*offset = 0;
	wb_units_init(&value);
	error = dom_control_value(element, &value);
	if (error != 0) {
		wb_units_release(&value);
		return error;
	}

	/* A caret past the value's end comes back to it. */
	if (control->caret > value.length)
		control->caret = value.length;

	/* A password's caret is after as many bullets. */
	if (box->control == DOM_CONTROL_PASSWORD) {
		bullet = LIST_PASSWORD_BULLET;
		error = layout_units_width(walk->text, font, &bullet, 1, &one);
		for (index = 0; index < control->caret && error == 0; index++)
			*offset += one;
	} else {
		error = layout_units_width(walk->text, font, value.data, control->caret, offset);
	}

	/* The value is no longer needed. */
	wb_units_release(&value);
	if (error != 0)
		return error;

	/* While an input method composes, the caret is at the composed text's cursor. */
	composing = list_composing(box, control);
	if (composing) {
		error = layout_units_width(walk->text, font, control->preedit.data, control->preedit_cursor, &into);
		if (error != 0)
			return error;
		*offset += into;
	}

	/* Succeeded: the caret's offset. */
	return 0;
}

/*
 * Draws a textarea's value line by line from its content box's top,
 * clipped to the content box, and records its caret (lines do not wrap in
 * this pass).
 */
static void
list_textarea(
	struct list_walk *walk,
	const struct layout_box *box,
	struct dom_element *element,
	const struct list_area *content,
	const struct text_font *font,
	layout_unit line,
	layout_unit ascent)
{
	struct dom_control *control;
	struct wb_units value;
	layout_unit caret_x;
	layout_unit caret_top;
	layout_unit y;
	size_t caret;
	size_t start;
	size_t index;
	int composing;
	int error;

	/* The state that the caret is kept in, and the value. */
	control = dom_control_of(element);
	if (control == NULL) {
		walk->error = ENOMEM;
		return;
	}

	/* The value. */
	wb_units_init(&value);
	error = dom_control_value(element, &value);
	if (error != 0) {
		wb_units_release(&value);
		walk->error = error;
		return;
	}

	/* A caret past the value's end comes back to it. */
	if (control->caret > value.length)
		control->caret = value.length;

	/* While an input method composes, its text shows at the caret, and the caret at its cursor. */
	caret = control->caret;
	composing = list_composing(box, control);
	if (composing) {
		error = list_insert_preedit(control, &value);
		if (error != 0) {
			wb_units_release(&value);
			walk->error = error;
			return;
		}
		caret += control->preedit_cursor;
	}

	/* Each line, and the caret on the line it is in. */
	control->scroll_x = 0;
	caret_x = content->x;
	caret_top = content->y;
	list_clip_rect(walk, content);
	y = content->y;
	start = 0;
	for (index = 0; index <= value.length && walk->error == 0; index++) {
		/* A line ends at a line feed or at the end of the value. */
		if (index < value.length && value.data[index] != 0x0aU)
			continue;

		/* The caret is on this line when it falls within it. */
		if (caret >= start && caret <= index) {
			error = layout_units_width(walk->text, font, value.data + start, caret - start, &caret_x);
			if (error != 0)
				walk->error = error;
			caret_x += content->x;
			caret_top = y;
		}

		/* The line's text, with the composed text underlined when it is on it. */
		list_text_run(walk, font, box->style.color, value.data + start, index - start, content->x, y + ascent);
		if (composing &&
		    caret >= start &&
		    caret <= index)
			list_preedit_underline(walk, font, control, caret_x, y + ascent, box->style.color);

		/* The next line below it. */
		y += line;
		start = index + 1U;
	}

	/* The clip ends with the lines, and the value is no longer needed. */
	list_unclip(walk);
	wb_units_release(&value);

	/* What the page needs for the caret. */
	list_control_record(walk, box, control, content, font, content->x, caret_x, caret_top, ascent);
}

/*
 * Draws a select: its chosen option's label from the content box's left,
 * on a line centered in its height, and a small arrow pointing down in the
 * room kept at its right.
 */
static void
list_select(
	struct list_walk *walk,
	const struct layout_box *box,
	struct dom_element *element,
	const struct list_area *content,
	const struct text_font *font,
	layout_unit line,
	layout_unit ascent)
{
	struct dom_element *option;
	struct wb_units label;
	layout_unit top;
	layout_unit arrow_x;
	layout_unit arrow_y;
	layout_unit row;
	int step;
	int error;

	/* The chosen option's label, if there is an option. */
	wb_units_init(&label);
	option = dom_select_chosen(element);
	error = 0;
	if (option != NULL)
		error = dom_option_text(option, &label);
	if (error != 0) {
		wb_units_release(&label);
		walk->error = error;
		return;
	}

	/* The label on the line, clipped to the room left of the arrow. */
	top = content->y + (content->height - line) / 2;
	list_clip_rect(walk, content);
	list_text_run(walk, font, box->style.color, label.data, label.length, content->x, top + ascent);
	list_unclip(walk);
	wb_units_release(&label);

	/* The arrow: rows a pixel tall, each two pixels narrower, from a row nine pixels wide. */
	row = LAYOUT_UNIT;
	arrow_x = content->x + content->width - LIST_ARROW_RIGHT * LAYOUT_UNIT;
	arrow_y = content->y + (content->height - LIST_ARROW_ROWS * row) / 2;
	for (step = 0; step < LIST_ARROW_ROWS; step++) {
		list_rect(
			walk,
			arrow_x + (layout_unit)step * row,
			arrow_y + (layout_unit)step * row,
			(layout_unit)(LIST_ARROW_WIDTH - 2 * step) * row,
			row,
			box->style.color);
	}
}

/*
 * Records in a control's state where it was drawn and where its caret
 * goes: the content box, and a caret as tall as the font's glyphs on the
 * line whose top is given, in the text's color.
 */
static void
list_control_record(
	struct list_walk *walk,
	const struct layout_box *box,
	struct dom_control *control,
	const struct list_area *content,
	const struct text_font *font,
	layout_unit text_x,
	layout_unit caret_x,
	layout_unit line_top,
	layout_unit ascent)
{
	struct text_metrics metrics;
	int error;

	/* The glyphs' extent, which the caret spans. */
	error = text_font_metrics(walk->text, font, &metrics);
	if (error != 0) {
		walk->error = error;
		return;
	}

	/*
	 * drawn tells the page that the geometry below is the display list's;
	 * it stays set as long as the control keeps a box.
	 */
	control->drawn = 1;
	control->content_x = text_x;
	control->content_y = content->y;
	control->content_width = content->width;
	control->content_height = content->height;
	control->caret_x = caret_x;
	control->caret_top = line_top + ascent - (layout_unit)metrics.ascent * LAYOUT_UNIT;
	control->caret_height = (layout_unit)(metrics.ascent + metrics.descent) * LAYOUT_UNIT;
	control->caret_color = box->style.color;
}

/* Tells whether a field or a textarea shows text an input method is composing (a password field never does). */
static int
list_composing(
	const struct layout_box *box,
	const struct dom_control *control)
{
	/* Nothing composed. */
	if (control == NULL || control->preedit.length == 0)
		return 0;

	/* A password field shows bullets only. */
	if (box->control == DOM_CONTROL_PASSWORD)
		return 0;

	/* The composed text shows. */
	return 1;
}

/* Inserts a control's composed text into its value at the caret. */
static int
list_insert_preedit(
	const struct dom_control *control,
	struct wb_units *value)
{
	size_t caret;
	size_t length;
	int error;

	/* Room for the composed text. */
	length = control->preedit.length;
	error = wb_units_reserve(value, length);
	if (error != 0)
		return error;

	/* The part after the caret moves on, and the composed text goes between. */
	caret = control->caret;
	if (caret > value->length)
		caret = value->length;
	memmove(value->data + caret + length, value->data + caret, (value->length - caret) * sizeof(uint16_t));
	memcpy(value->data + caret, control->preedit.data, length * sizeof(uint16_t));
	value->length += length;

	/* Succeeded: the composed text is in. */
	return 0;
}

/*
 * Underlines a control's composed text, whose cursor is at caret_x, a
 * pixel below the baseline in the text's color.
 */
static void
list_preedit_underline(
	struct list_walk *walk,
	const struct text_font *font,
	const struct dom_control *control,
	layout_unit caret_x,
	layout_unit baseline,
	uint32_t color)
{
	layout_unit into;
	layout_unit width;
	layout_unit start;
	int error;

	/* How far the cursor is into the composed text, and how wide the text is. */
	error = layout_units_width(walk->text, font, control->preedit.data, control->preedit_cursor, &into);
	if (error != 0) {
		walk->error = error;
		return;
	}
	error = layout_units_width(walk->text, font, control->preedit.data, control->preedit.length, &width);
	if (error != 0) {
		walk->error = error;
		return;
	}

	/* The line under it. */
	list_rect(walk, caret_x - into, baseline + LAYOUT_UNIT, width, LAYOUT_UNIT, color);

	/* The chosen part (an input method's clause), under a line three times as thick (ws177-p019). */
	if (control->preedit_begin >= control->preedit_cursor)
		return;
	error = layout_units_width(walk->text, font, control->preedit.data, control->preedit_begin, &start);
	if (error != 0) {
		walk->error = error;
		return;
	}

	/* Its line, from its start to the cursor. */
	list_rect(walk, caret_x - into + start, baseline + LAYOUT_UNIT, into - start, 3 * LAYOUT_UNIT, color);
}

/* Picks the font a style draws text with (as the layout picks it). */
static void
list_style_font(
	struct list_walk *walk,
	const struct css_style *style,
	struct text_font *font)
{
	int monospace;

	/* The monospace family uses the monospace face. */
	monospace = 0;
	if (style->generic_family == CSS_FAMILY_MONOSPACE)
		monospace = 1;
	text_select_font(walk->text, monospace, style->font_size, style->font_weight, font);
}

/* Adds a run of UTF-16 text as a text item from a pen position on a baseline. */
static void
list_text_run(
	struct list_walk *walk,
	const struct text_font *font,
	uint32_t color,
	const uint16_t *units,
	size_t length,
	layout_unit x,
	layout_unit baseline)
{
	struct paint_item item;
	struct paint_glyph *glyphs;
	struct text_glyph glyph;
	uint32_t code_point;
	layout_unit pen;
	size_t count;
	size_t used;
	size_t position;
	int error;

	/* Nothing to add after an error, or for no text. */
	if (walk->error != 0 || length == 0)
		return;

	/* There are at most as many glyphs as UTF-16 units. */
	glyphs = wb_arena_alloc(&walk->list->arena, length * sizeof(struct paint_glyph));
	if (glyphs == NULL) {
		walk->error = ENOMEM;
		return;
	}

	/* Sets each character's glyph at the pen, which moves by its advance. */
	count = 0;
	pen = 0;
	position = 0;
	while (position < length) {
		used = wb_utf16_decode(units + position, length - position, &code_point);
		position += used;
		error = text_glyph(walk->text, font, code_point, 0, &glyph);
		if (error != 0) {
			walk->error = error;
			return;
		}

		/* Places it and moves the pen on. */
		glyphs[count].code_point = code_point;
		glyphs[count].x = pen;
		count++;
		pen += (layout_unit)glyph.advance_units;
	}

	/* The text item. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_TEXT;
	item.x = x;
	item.y = baseline;
	item.width = pen;
	item.color = color;
	item.font = *font;
	item.glyphs = glyphs;
	item.glyph_count = count;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/* Adds a replaced box's image over its content box, whose top left is at a point. */
static void
list_image(
	struct list_walk *walk,
	const struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	/* The image over the content box. */
	list_image_item(walk, box->image, x, y, box->width, box->height);
}

/* Adds an image item: an image stretched over a rectangle. */
static void
list_image_item(
	struct list_walk *walk,
	const struct img_bitmap *image,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height)
{
	struct paint_item item;
	int error;

	/* Nothing to add after an error, without an image, or for an empty rectangle. */
	if (walk->error != 0 || image == NULL)
		return;
	if (width <= 0 || height <= 0)
		return;

	/* The item. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_IMAGE;
	item.x = x;
	item.y = y;
	item.width = width;
	item.height = height;
	item.image = image;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/* Paints a box's background image in its padding box, over its border box. */
static void
list_box_background(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct list_area area;
	struct list_area painting;

	/* A box without an image paints none. */
	if (box->background == NULL)
		return;

	/* The border box paints it; the padding box places it. */
	painting.x = box->x;
	painting.y = box->y;
	painting.width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	painting.height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	area.x = box->x + box->border[CSS_LEFT];
	area.y = box->y + box->border[CSS_TOP];
	area.width = box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT];
	area.height = box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM];
	list_background_image(walk, box, &area, &painting);
}

/*
 * Paints the canvas's background image (the root's, or the body's): placed
 * in the root's padding box and painted over the whole canvas.
 */
static void
list_canvas_background(
	struct list_walk *walk,
	const struct layout_tree *tree)
{
	const struct layout_box *root;
	struct list_area area;
	struct list_area painting;

	/* A canvas without an image paints none. */
	if (walk->canvas_box == NULL || walk->canvas_box->background == NULL)
		return;

	/* The whole canvas paints it; the root's padding box places it. */
	root = tree->root;
	painting.x = 0;
	painting.y = 0;
	painting.width = walk->list->width;
	painting.height = walk->list->height;
	area.x = root->x + root->border[CSS_LEFT];
	area.y = root->y + root->border[CSS_TOP];
	area.width = root->padding[CSS_LEFT] + root->width + root->padding[CSS_RIGHT];
	area.height = root->padding[CSS_TOP] + root->height + root->padding[CSS_BOTTOM];
	list_background_image(walk, walk->canvas_box, &area, &painting);
}

/*
 * Paints a box's background image: sized in its positioning area (the
 * padding box, or the root's for the canvas), placed there by
 * background-position, and repeated as background-repeat says over the
 * painting area (the border box, or the canvas), which clips it.
 */
static void
list_background_image(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct list_area *area,
	const struct list_area *painting)
{
	const struct img_bitmap *image;
	struct list_area fixed;
	const struct list_area *positioning;
	layout_unit tile_width;
	layout_unit tile_height;
	layout_unit start_x;
	layout_unit start_y;
	layout_unit end_x;
	layout_unit end_y;
	layout_unit x;
	layout_unit y;
	size_t tiles;
	int repeat_x;
	int repeat_y;

	/* Nothing without an image, or in an area without room. */
	image = box->background;
	if (walk->error != 0 || image == NULL)
		return;
	if (area->width <= 0 || area->height <= 0 || painting->width <= 0 || painting->height <= 0)
		return;

	/* A fixed image is sized and positioned against the viewport at the document origin. */
	positioning = area;
	if (box->style.background_attachment == CSS_BACKGROUND_FIXED) {
		fixed.x = 0;
		fixed.y = 0;
		fixed.width = walk->viewport_width;
		fixed.height = walk->viewport_height;
		positioning = &fixed;
	}

	/* The size of one tile; an empty one paints nothing. */
	list_background_size(box, image, positioning, &tile_width, &tile_height);
	if (tile_width <= 0 || tile_height <= 0)
		return;

	/* The first tile's place, from the position in the room the area leaves. */
	start_x = positioning->x + list_background_offset(&box->style.background_position[0], positioning->width - tile_width);
	start_y = positioning->y + list_background_offset(&box->style.background_position[1], positioning->height - tile_height);

	/* A repeating axis starts at the tile before the painting area and ends past it; another has one tile. */
	repeat_x = box->style.background_repeat == CSS_REPEAT_BOTH || box->style.background_repeat == CSS_REPEAT_X;
	repeat_y = box->style.background_repeat == CSS_REPEAT_BOTH || box->style.background_repeat == CSS_REPEAT_Y;
	end_x = start_x + tile_width;
	end_y = start_y + tile_height;
	if (repeat_x) {
		start_x -= ((start_x - painting->x + tile_width - 1) / tile_width) * tile_width;
		end_x = painting->x + painting->width;
	}

	/* The vertical axis likewise. */
	if (repeat_y) {
		start_y -= ((start_y - painting->y + tile_height - 1) / tile_height) * tile_height;
		end_y = painting->y + painting->height;
	}

	/* The tiles, inside the painting area, up to a number no page needs. */
	list_clip_rect(walk, painting);
	tiles = 0;
	for (y = start_y; y < end_y && tiles < LIST_TILES_MAX; y += tile_height) {
		for (x = start_x; x < end_x && tiles < LIST_TILES_MAX; x += tile_width) {
			list_image_item(walk, image, x, y, tile_width, tile_height);
			tiles++;
		}
	}

	/* The painting area's clip ends. */
	list_unclip(walk);
}

/*
 * Works out the size of one tile of a background image in its area:
 * contain and cover scale the image to fit inside or to cover the area,
 * lengths and percentages size a side, and an auto side follows the other
 * through the image's ratio (or is the image's own).
 */
static void
list_background_size(
	const struct layout_box *box,
	const struct img_bitmap *image,
	const struct list_area *area,
	layout_unit *width,
	layout_unit *height)
{
	const struct css_length *sizes;
	float natural_width;
	float natural_height;
	float scale;
	float scale_height;
	int width_auto;
	int height_auto;

	/* The image's own size. */
	natural_width = (float)image->width * LAYOUT_UNIT;
	natural_height = (float)image->height * LAYOUT_UNIT;

	/* contain: the largest size that fits; cover: the smallest that covers. */
	if (box->style.background_size_keyword != CSS_BACKGROUND_SIZE_LENGTHS) {
		scale = (float)area->width / natural_width;
		scale_height = (float)area->height / natural_height;
		if (box->style.background_size_keyword == CSS_BACKGROUND_SIZE_CONTAIN && scale_height < scale)
			scale = scale_height;
		if (box->style.background_size_keyword == CSS_BACKGROUND_SIZE_COVER && scale_height > scale)
			scale = scale_height;
		*width = (layout_unit)(natural_width * scale + 0.5f);
		*height = (layout_unit)(natural_height * scale + 0.5f);
		return;
	}

	/* Each side given, or auto. */
	sizes = box->style.background_size;
	width_auto = sizes[0].unit != CSS_UNIT_PX && sizes[0].unit != CSS_UNIT_PERCENT;
	height_auto = sizes[1].unit != CSS_UNIT_PX && sizes[1].unit != CSS_UNIT_PERCENT;
	*width = (layout_unit)natural_width;
	*height = (layout_unit)natural_height;
	if (!width_auto)
		*width = list_background_length(&sizes[0], area->width);
	if (!height_auto)
		*height = list_background_length(&sizes[1], area->height);

	/* An auto side follows the other through the ratio. */
	if (width_auto && !height_auto)
		*width = (layout_unit)((float)*height * natural_width / natural_height);
	if (height_auto && !width_auto)
		*height = (layout_unit)((float)*width * natural_height / natural_width);
}

/* Resolves a length of the background against a length of its area (a percentage of it, or pixels). */
static layout_unit
list_background_length(
	const struct css_length *length,
	layout_unit whole)
{
	layout_unit value;

	/* A percentage of the whole. */
	if (length->unit == CSS_UNIT_PERCENT) {
		value = (layout_unit)((float)whole * length->value / 100.0f);
		return value;
	}

	/* Pixels. */
	value = layout_from_px(length->value);
	return value;
}

/* Resolves one axis of background-position: a percentage of the room the tile leaves, or pixels. */
static layout_unit
list_background_offset(
	const struct css_length *position,
	layout_unit room)
{
	layout_unit offset;

	/* A percentage of the room (which is negative for a tile larger than its area). */
	if (position->unit == CSS_UNIT_PERCENT) {
		offset = (layout_unit)((float)room * position->value / 100.0f);
		return offset;
	}

	/* Pixels from the area's edge. */
	offset = layout_from_px(position->value);
	return offset;
}

/* Starts a clip to a rectangle (the items until its end are drawn inside it). */
static void
list_clip_rect(
	struct list_walk *walk,
	const struct list_area *area)
{
	struct paint_item item;
	int error;

	/* Nothing to add after an error. */
	if (walk->error != 0)
		return;

	/* The rectangle. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_CLIP;
	item.x = area->x;
	item.y = area->y;
	item.width = area->width;
	item.height = area->height;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/* Adds a filled rectangle. */
static void
list_rect(
	struct list_walk *walk,
	layout_unit x,
	layout_unit y,
	layout_unit width,
	layout_unit height,
	uint32_t color)
{
	struct paint_item item;
	int error;

	/* Nothing to add after an error, for an empty rectangle, or for a transparent color. */
	if (walk->error != 0)
		return;
	if (width <= 0 || height <= 0)
		return;
	if ((color >> 24) == 0)
		return;

	/* The item. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_RECT;
	item.x = x;
	item.y = y;
	item.width = width;
	item.height = height;
	item.color = color;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/* Scales the alpha of the rectangles and text added since an index by an opacity. */
static void
list_fade(
	struct list_walk *walk,
	size_t first,
	float opacity)
{
	struct paint_item *item;
	uint32_t alpha;
	size_t index;

	/* Each rectangle's and text item's alpha (clips and images keep theirs). */
	for (index = first; index < walk->list->items.count; index++) {
		item = wb_vector_at(&walk->list->items, index);
		if (item->kind != PAINT_RECT && item->kind != PAINT_TEXT)
			continue;
		alpha = (uint32_t)((float)(item->color >> 24) * opacity + 0.5f);
		item->color = (item->color & 0x00ffffffU) | (alpha << 24);
	}
}

/* Multiplies the opacities of the inline boxes around a text box, up to the block that holds its line. */
static float
list_inline_opacity(
	const struct layout_box *box)
{
	const struct layout_box *ancestor;
	float opacity;

	/* The inline boxes up to the first box that is not one. */
	opacity = 1.0f;
	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor->kind != LAYOUT_INLINE)
			break;
		opacity *= ancestor->style.opacity;
	}

	/* The product. */
	return opacity;
}

/*
 * Finds a box's border box with its corners' radii in layout units (a
 * percentage of the border box's width or height), shrunk together when
 * two radii on a side would overlap.  Reports whether any corner is round.
 */
static int
list_border_shape(
	const struct layout_box *box,
	struct list_shape *shape)
{
	const struct css_length *length;
	layout_unit whole;
	float factor;
	float room;
	int corner;
	int axis;
	int round;

	/* The border box. */
	shape->x = box->x;
	shape->y = box->y;
	shape->width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	shape->height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* Each radius: pixels, or a percentage of the width (horizontal) or the height (vertical). */
	round = 0;
	for (corner = 0; corner < 4; corner++) {
		for (axis = 0; axis < 2; axis++) {
			length = &box->style.radius[corner][axis];
			whole = shape->width;
			if (axis == 1)
				whole = shape->height;
			shape->radius[corner][axis] = 0;
			if (length->unit == CSS_UNIT_PX)
				shape->radius[corner][axis] = layout_from_px(length->value);
			if (length->unit == CSS_UNIT_PERCENT)
				shape->radius[corner][axis] = (layout_unit)((float)whole * length->value / 100.0f) + layout_from_px(length->offset);
			if (shape->radius[corner][axis] < 0)
				shape->radius[corner][axis] = 0;
		}

		/* A corner is round only when both of its radii are. */
		if (shape->radius[corner][0] == 0 || shape->radius[corner][1] == 0) {
			shape->radius[corner][0] = 0;
			shape->radius[corner][1] = 0;
		} else {
			round = 1;
		}
	}

	/* A square box needs no shape. */
	if (!round)
		return 0;

	/* The radii shrink by the same factor until no two on a side overlap. */
	factor = 1.0f;
	room = (float)shape->width / (float)(shape->radius[CSS_TOP_LEFT][0] + shape->radius[CSS_TOP_RIGHT][0] + 1);
	if (room < factor)
		factor = room;
	room = (float)shape->width / (float)(shape->radius[CSS_BOTTOM_LEFT][0] + shape->radius[CSS_BOTTOM_RIGHT][0] + 1);
	if (room < factor)
		factor = room;
	room = (float)shape->height / (float)(shape->radius[CSS_TOP_LEFT][1] + shape->radius[CSS_BOTTOM_LEFT][1] + 1);
	if (room < factor)
		factor = room;
	room = (float)shape->height / (float)(shape->radius[CSS_TOP_RIGHT][1] + shape->radius[CSS_BOTTOM_RIGHT][1] + 1);
	if (room < factor)
		factor = room;

	/* Applies the factor to every radius. */
	if (factor < 1.0f) {
		for (corner = 0; corner < 4; corner++) {
			shape->radius[corner][0] = (layout_unit)((float)shape->radius[corner][0] * factor);
			shape->radius[corner][1] = (layout_unit)((float)shape->radius[corner][1] * factor);
		}
	}

	/* The box has round corners. */
	return 1;
}

/*
 * Fills a rounded shape: each pixel row of the corners as a rectangle
 * between the curves at the row's middle, and the straight part between
 * the corners as one rectangle.
 */
static void
list_round_fill(
	struct list_walk *walk,
	const struct list_shape *shape,
	uint32_t color)
{
	layout_unit top_band;
	layout_unit bottom_band;
	layout_unit row;
	layout_unit next;
	layout_unit bottom;

	/* An empty shape fills nothing. */
	if (shape->width <= 0 || shape->height <= 0)
		return;

	/* The straight part starts at the pixel row under the lower top corner, and ends above the higher bottom corner. */
	bottom = shape->y + shape->height;
	top_band = shape->radius[CSS_TOP_LEFT][1];
	if (shape->radius[CSS_TOP_RIGHT][1] > top_band)
		top_band = shape->radius[CSS_TOP_RIGHT][1];
	top_band = (shape->y + top_band + LAYOUT_UNIT - 1) / LAYOUT_UNIT * LAYOUT_UNIT;
	bottom_band = shape->radius[CSS_BOTTOM_LEFT][1];
	if (shape->radius[CSS_BOTTOM_RIGHT][1] > bottom_band)
		bottom_band = shape->radius[CSS_BOTTOM_RIGHT][1];
	bottom_band = (bottom - bottom_band) / LAYOUT_UNIT * LAYOUT_UNIT;
	if (top_band > bottom_band) {
		top_band = bottom;
		bottom_band = bottom;
	}

	/* The rows of the top corners, one pixel row at a time. */
	row = shape->y;
	while (row < top_band && walk->error == 0) {
		next = (row / LAYOUT_UNIT + 1) * LAYOUT_UNIT;
		if (next > top_band)
			next = top_band;
		list_round_row(walk, shape, row, next, color);
		row = next;
	}

	/* The straight part between the corners. */
	if (bottom_band > top_band)
		list_rect(walk, shape->x, top_band, shape->width, bottom_band - top_band, color);

	/* The rows of the bottom corners. */
	row = bottom_band;
	if (row < top_band)
		row = top_band;
	while (row < bottom && walk->error == 0) {
		next = (row / LAYOUT_UNIT + 1) * LAYOUT_UNIT;
		if (next > bottom)
			next = bottom;
		list_round_row(walk, shape, row, next, color);
		row = next;
	}
}

/* Fills one row of a rounded shape between its curves at the row's middle. */
static void
list_round_row(
	struct list_walk *walk,
	const struct list_shape *shape,
	layout_unit top,
	layout_unit bottom,
	uint32_t color)
{
	layout_unit left;
	layout_unit right;

	/* The shape's width at the row's middle. */
	list_round_span(shape, (top + bottom) / 2, &left, &right);

	/* The row's rectangle. */
	list_rect(walk, left, top, right - left, bottom - top, color);
}

/* Finds where a rounded shape starts and ends across at a height (both corners' curves on each side). */
static void
list_round_span(
	const struct list_shape *shape,
	layout_unit y,
	layout_unit *left,
	layout_unit *right)
{
	const layout_unit *radius;
	layout_unit inset;
	float across;
	float down;

	/* The rectangle's sides, moved in by the corners the height is beside. */
	*left = shape->x;
	*right = shape->x + shape->width;

	/* The top left corner. */
	radius = shape->radius[CSS_TOP_LEFT];
	if (radius[1] > 0 && y < shape->y + radius[1]) {
		down = (float)(shape->y + radius[1] - y) / (float)radius[1];
		across = 1.0f - sqrtf(1.0f - down * down);
		inset = (layout_unit)((float)radius[0] * across);
		if (shape->x + inset > *left)
			*left = shape->x + inset;
	}

	/* The bottom left corner. */
	radius = shape->radius[CSS_BOTTOM_LEFT];
	if (radius[1] > 0 && y > shape->y + shape->height - radius[1]) {
		down = (float)(y - (shape->y + shape->height - radius[1])) / (float)radius[1];
		if (down > 1.0f)
			down = 1.0f;
		across = 1.0f - sqrtf(1.0f - down * down);
		inset = (layout_unit)((float)radius[0] * across);
		if (shape->x + inset > *left)
			*left = shape->x + inset;
	}

	/* The top right corner. */
	radius = shape->radius[CSS_TOP_RIGHT];
	if (radius[1] > 0 && y < shape->y + radius[1]) {
		down = (float)(shape->y + radius[1] - y) / (float)radius[1];
		across = 1.0f - sqrtf(1.0f - down * down);
		inset = (layout_unit)((float)radius[0] * across);
		if (shape->x + shape->width - inset < *right)
			*right = shape->x + shape->width - inset;
	}

	/* The bottom right corner. */
	radius = shape->radius[CSS_BOTTOM_RIGHT];
	if (radius[1] > 0 && y > shape->y + shape->height - radius[1]) {
		down = (float)(y - (shape->y + shape->height - radius[1])) / (float)radius[1];
		if (down > 1.0f)
			down = 1.0f;
		across = 1.0f - sqrtf(1.0f - down * down);
		inset = (layout_unit)((float)radius[0] * across);
		if (shape->x + shape->width - inset < *right)
			*right = shape->x + shape->width - inset;
	}
}

/*
 * Paints the borders of a box with rounded corners: each pixel row between
 * the outer shape and the inner one (the padding box, its radii the outer
 * ones less the borders), in the color of the side it belongs to (the top
 * and bottom rows in theirs, the rows beside the padding box in the left
 * and right colors).
 */
static void
list_round_borders(
	struct list_walk *walk,
	const struct layout_box *box,
	const struct list_shape *outer)
{
	struct list_shape inner;
	layout_unit outer_left;
	layout_unit outer_right;
	layout_unit inner_left;
	layout_unit inner_right;
	layout_unit row;
	layout_unit next;
	layout_unit middle;
	layout_unit bottom;
	int corner;

	/* No border draws nothing. */
	if (box->border[CSS_TOP] == 0 && box->border[CSS_RIGHT] == 0 && box->border[CSS_BOTTOM] == 0 && box->border[CSS_LEFT] == 0)
		return;

	/* The padding box, its radii the outer ones less the borders beside them. */
	inner.x = outer->x + box->border[CSS_LEFT];
	inner.y = outer->y + box->border[CSS_TOP];
	inner.width = outer->width - box->border[CSS_LEFT] - box->border[CSS_RIGHT];
	inner.height = outer->height - box->border[CSS_TOP] - box->border[CSS_BOTTOM];
	for (corner = 0; corner < 4; corner++) {
		inner.radius[corner][0] = outer->radius[corner][0] - box->border[CSS_LEFT];
		if (corner == CSS_TOP_RIGHT || corner == CSS_BOTTOM_RIGHT)
			inner.radius[corner][0] = outer->radius[corner][0] - box->border[CSS_RIGHT];
		inner.radius[corner][1] = outer->radius[corner][1] - box->border[CSS_TOP];
		if (corner == CSS_BOTTOM_LEFT || corner == CSS_BOTTOM_RIGHT)
			inner.radius[corner][1] = outer->radius[corner][1] - box->border[CSS_BOTTOM];
		if (inner.radius[corner][0] < 0)
			inner.radius[corner][0] = 0;
		if (inner.radius[corner][1] < 0)
			inner.radius[corner][1] = 0;
	}

	/* Each row of the border box: at pixel rows, and at the padding box's top and bottom. */
	bottom = outer->y + outer->height;
	row = outer->y;
	while (row < bottom && walk->error == 0) {
		next = (row / LAYOUT_UNIT + 1) * LAYOUT_UNIT;
		if (row < inner.y && next > inner.y)
			next = inner.y;
		if (row < inner.y + inner.height && next > inner.y + inner.height)
			next = inner.y + inner.height;
		if (next > bottom)
			next = bottom;
		middle = (row + next) / 2;
		list_round_span(outer, middle, &outer_left, &outer_right);

		/* A row above or below the padding box is the top or bottom border. */
		if (middle < inner.y || middle >= inner.y + inner.height || inner.width <= 0) {
			if (middle < inner.y) {
				list_rect(walk, outer_left, row, outer_right - outer_left, next - row, box->style.border_color[CSS_TOP]);
			} else {
				list_rect(walk, outer_left, row, outer_right - outer_left, next - row, box->style.border_color[CSS_BOTTOM]);
			}

			/* On to the next row. */
			row = next;
			continue;
		}

		/* A row beside the padding box is the left and right borders. */
		list_round_span(&inner, middle, &inner_left, &inner_right);
		list_rect(walk, outer_left, row, inner_left - outer_left, next - row, box->style.border_color[CSS_LEFT]);
		list_rect(walk, inner_right, row, outer_right - inner_right, next - row, box->style.border_color[CSS_RIGHT]);
		row = next;
	}
}

/* Paints a box's outer shadows under it, the last listed lowest (inset shadows are not drawn in this pass). */
static void
list_shadows(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct list_shape border;
	int index;

	/* A box without shadows paints none. */
	if (box->style.shadow_count == 0)
		return;

	/* The border box's shape, which the shadows follow. */
	list_border_shape(box, &border);

	/* The shadows from the last to the first. */
	for (index = box->style.shadow_count - 1; index >= 0; index--) {
		if (box->style.shadows[index].inset)
			continue;
		list_shadow(walk, &border, &box->style.shadows[index]);
	}
}

/*
 * Paints one outer shadow: the border box's shape moved by the offset and
 * grown by the spread, and when it is blurred, translucent copies grown
 * and shrunk by the blur's quantiles, which together fade from the full
 * color inside to nothing outside as a Gaussian blur does, half at the
 * edge.
 */
static void
list_shadow(
	struct list_walk *walk,
	const struct list_shape *border,
	const struct css_shadow *shadow)
{
	struct list_shape moved;
	struct list_shape layer;
	layout_unit blur;
	layout_unit amount;
	uint32_t color;
	float alpha;
	float share;
	int index;

	/* A transparent shadow paints nothing. */
	if ((shadow->color >> 24) == 0)
		return;

	/* The shape moved and spread. */
	list_grow_shape(border, layout_from_px(shadow->spread), &moved);
	moved.x += layout_from_px(shadow->x);
	moved.y += layout_from_px(shadow->y);

	/* A sharp shadow is the shape in the color. */
	blur = layout_from_px(shadow->blur);
	if (blur < LAYOUT_UNIT / 2) {
		list_round_fill(walk, &moved, shadow->color);
		return;
	}

	/* Each copy's alpha, so that all of them together make the color's. */
	alpha = (float)(shadow->color >> 24) / 255.0f;
	share = 1.0f - powf(1.0f - alpha, 1.0f / (float)LIST_SHADOW_LAYERS);
	color = (shadow->color & 0x00ffffffU) | ((uint32_t)(share * 255.0f + 0.5f) << 24);

	/* The copies, grown by the quantiles of a deviation of half the blur radius. */
	for (index = 0; index < LIST_SHADOW_LAYERS && walk->error == 0; index++) {
		amount = (layout_unit)((float)blur * 0.5f * list_shadow_quantiles[index]);
		list_grow_shape(&moved, amount, &layer);
		list_round_fill(walk, &layer, color);
	}
}

/* Grows a shape by an amount on every side (shrinks it when the amount is negative), its radii with it. */
static void
list_grow_shape(
	const struct list_shape *shape,
	layout_unit amount,
	struct list_shape *grown)
{
	int corner;
	int axis;

	/* The rectangle, and each round corner's radii, by the amount (a square corner stays square). */
	*grown = *shape;
	grown->x = shape->x - amount;
	grown->y = shape->y - amount;
	grown->width = shape->width + 2 * amount;
	grown->height = shape->height + 2 * amount;
	for (corner = 0; corner < 4; corner++) {
		for (axis = 0; axis < 2; axis++) {
			if (shape->radius[corner][axis] == 0)
				continue;
			grown->radius[corner][axis] = shape->radius[corner][axis] + amount;
			if (grown->radius[corner][axis] < 0)
				grown->radius[corner][axis] = 0;
		}
	}
}

/* Paints a box's outline: four rectangles of its width around the border box, moved out by its offset. */
static void
list_outline(
	struct list_walk *walk,
	const struct layout_box *box)
{
	layout_unit width;
	layout_unit height;
	layout_unit thickness;
	layout_unit x;
	layout_unit y;
	uint32_t color;

	/* A box without an outline paints none. */
	thickness = layout_from_px(box->style.outline_width);
	if (thickness <= 0)
		return;

	/* The rectangle the outline goes around. */
	x = box->x - layout_from_px(box->style.outline_offset);
	y = box->y - layout_from_px(box->style.outline_offset);
	width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] +
	    2 * layout_from_px(box->style.outline_offset);
	height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM] +
	    2 * layout_from_px(box->style.outline_offset);
	color = box->style.outline_color;

	/* The top and bottom across the corners, the left and right between them. */
	list_rect(walk, x - thickness, y - thickness, width + 2 * thickness, thickness, color);
	list_rect(walk, x - thickness, y + height, width + 2 * thickness, thickness, color);
	list_rect(walk, x - thickness, y, thickness, height, color);
	list_rect(walk, x + width, y, thickness, height, color);
}

/* Writes a color as #AARRGGBB. */
static void
list_color(
	struct wb_buffer *out,
	uint32_t color)
{
	/* Eight hexadecimal digits, alpha first. */
	wb_buffer_printf(out, "#%08x", (unsigned)color);
}

/* Adds the start of a clip to a box's padding box. */
static void
list_clip(
	struct list_walk *walk,
	const struct layout_box *box)
{
	struct paint_item item;
	int error;

	/* Nothing to add after an error. */
	if (walk->error != 0)
		return;

	/* The padding box: inside the borders. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_CLIP;
	item.x = box->x + box->border[CSS_LEFT];
	item.y = box->y + box->border[CSS_TOP];
	item.width = box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT];
	item.height = box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM];
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/* Adds the end of the last clip. */
static void
list_unclip(
	struct list_walk *walk)
{
	struct paint_item item;
	int error;

	/* Nothing to add after an error. */
	if (walk->error != 0)
		return;

	/* The item. */
	memset(&item, 0, sizeof(item));
	item.kind = PAINT_UNCLIP;
	error = wb_vector_push(&walk->list->items, &item);
	if (error != 0)
		walk->error = error;
}

/*
 * Adds the clips of the clipping boxes around a positioned box that it
 * does not escape (an absolute box escapes those inside its containing
 * block), the outermost first, and reports how many were added (push zero
 * only counts them).
 */
static int
list_layer_clips(
	struct list_walk *walk,
	const struct layout_box *layer,
	int push)
{
	const struct layout_box *around[PAINT_CLIP_DEPTH];
	const struct layout_box *walk_up;
	int count;
	int reached;
	int clips;
	int positioned;

	/* A fixed box escapes every clip; an absolute one those between it and its containing block. */
	count = 0;
	if (layer->style.position == CSS_POSITION_FIXED)
		return 0;
	reached = 1;
	if (layer->style.position == CSS_POSITION_ABSOLUTE)
		reached = 0;

	/* The clipping ancestors, nearest first, from the containing block up for an absolute box. */
	for (walk_up = layer->parent; walk_up != NULL && count < PAINT_CLIP_DEPTH; walk_up = walk_up->parent) {
		clips = layout_clips(walk_up);
		positioned = layout_is_positioned(walk_up);

		/* The containing block (the nearest positioned ancestor) and the boxes around it clip an absolute box. */
		if (positioned)
			reached = 1;
		if (clips && reached)
			around[count++] = walk_up;
	}

	/* The clips, the outermost first. */
	for (clips = count; push && clips > 0; clips--)
		list_clip(walk, around[clips - 1]);

	/* Reports how many there are. */
	return count;
}
