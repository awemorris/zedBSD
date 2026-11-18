/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display list of libpdf: the items a page's content draws, gathered
 * while the content is interpreted (content.c) and handed to the program
 * that renders the page.
 *
 * The paths of all items share one verb array and one point array, which
 * grow while the page is read; each item keeps where its path starts until
 * the list is finished and the arrays no longer move.  Every array is
 * bounded (internal.h), so a page cannot make the list grow without end.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The first capacity of each of the builder's arrays. */
#define PDF_DISPLAY_INITIAL 64

static int grow_items(struct pdf_display_builder *builder);
static int grow_verbs(struct pdf_display_builder *builder, size_t more);
static int grow_points(struct pdf_display_builder *builder, size_t more);
static int grow_images(struct pdf_display_builder *builder);
static size_t next_capacity(size_t capacity, size_t needed, size_t limit);

/*
 * Frees a display list and everything it owns.
 */
void
pdf_display_list_destroy(
	struct pdf_display_list *list)
{
	struct pdf_display_builder *builder;

	/* Nothing to free for no list. */
	if (list == NULL)
		return;

	/* The list is the first member of the builder that made it. */
	builder = (struct pdf_display_builder *)list;
	pdf_display_free(builder);
}

/*
 * Makes an empty builder.
 */
int
pdf_display_create(
	struct pdf_display_builder **builder)
{
	struct pdf_display_builder *created;

	/* Allocates the builder with every array empty. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Succeeded: the builder holds nothing yet. */
	*builder = created;
	return 0;
}

/*
 * Frees a builder, its arrays and the pixels of its images.
 */
void
pdf_display_free(
	struct pdf_display_builder *builder)
{
	size_t index;

	/* Nothing to free for no builder. */
	if (builder == NULL)
		return;

	/* Frees each image's pixels. */
	for (index = 0; index < builder->images_count; index++)
		free(builder->images[index]);

	/* Frees the arrays and the builder itself. */
	free(builder->images);
	free(builder->items);
	free(builder->verb_starts);
	free(builder->point_starts);
	free(builder->verbs);
	free(builder->points);
	free(builder);
}

/*
 * Appends a fill or a clip push whose path is given in the page's shown
 * space.
 *
 * style supplies the rule, and for a fill the colour, alpha and blend mode;
 * its other fields are ignored.  The path is copied.
 */
int
pdf_display_add_path(
	struct pdf_display_builder *builder,
	enum pdf_item_type type,
	const unsigned char *verbs,
	size_t verb_count,
	const struct pdf_point *points,
	size_t point_count,
	const struct pdf_display_item *style)
{
	struct pdf_display_item *item;
	int error;

	/* Makes room for the item. */
	error = grow_items(builder);
	if (error != 0)
		return error;

	/* Makes room for its verbs. */
	error = grow_verbs(builder, verb_count);
	if (error != 0)
		return error;

	/* Makes room for its points. */
	error = grow_points(builder, point_count);
	if (error != 0)
		return error;

	/* Copies the path to the end of the shared arrays (an empty path, a clip of nothing, copies nothing). */
	if (verb_count > 0)
		memcpy(builder->verbs + builder->verbs_count, verbs, verb_count);
	if (point_count > 0)
		memcpy(builder->points + builder->points_count, points, point_count * sizeof(*points));

	/* Fills in the item, which remembers where its path starts. */
	item = &builder->items[builder->items_count];
	memset(item, 0, sizeof(*item));
	item->type = type;
	item->rule = style->rule;
	item->verb_count = verb_count;
	item->point_count = point_count;
	item->red = style->red;
	item->green = style->green;
	item->blue = style->blue;
	item->alpha = style->alpha;
	item->blend = style->blend;
	builder->verb_starts[builder->items_count] = builder->verbs_count;
	builder->point_starts[builder->items_count] = builder->points_count;

	/* Publishes the item and its path. */
	builder->verbs_count += verb_count;
	builder->points_count += point_count;
	builder->items_count++;

	/* Succeeded: the item is the list's last. */
	return 0;
}

/*
 * Appends an image, taking its pixels (RGBA, malloc'd) over.
 *
 * style supplies the size, the matrix, the alpha, the blend mode and
 * whether to interpolate.  The pixels are the builder's even when the
 * image is refused.
 */
int
pdf_display_add_image(
	struct pdf_display_builder *builder,
	unsigned char *pixels,
	const struct pdf_display_item *style)
{
	struct pdf_display_item *item;
	size_t pixel_count;
	int error;

	/* Keeps the pixels first, so that they are freed whatever happens next. */
	error = grow_images(builder);
	if (error != 0) {
		free(pixels);
		return error;
	}

	/* The list owns the pixels from here on. */
	builder->images[builder->images_count] = pixels;
	builder->images_count++;

	/* Refuses more pixels than one list may hold. */
	pixel_count = style->image_width * style->image_height;
	if (pixel_count > PDF_DISPLAY_PIXELS_MAX - builder->pixels_count)
		return ENOMEM;

	/* Makes room for the item. */
	error = grow_items(builder);
	if (error != 0)
		return error;

	/* Fills in the item. */
	item = &builder->items[builder->items_count];
	memset(item, 0, sizeof(*item));
	item->type = PDF_ITEM_IMAGE;
	item->alpha = style->alpha;
	item->blend = style->blend;
	item->pixels = pixels;
	item->image_width = style->image_width;
	item->image_height = style->image_height;
	memcpy(item->matrix, style->matrix, sizeof(item->matrix));
	item->interpolate = style->interpolate;
	builder->verb_starts[builder->items_count] = 0;
	builder->point_starts[builder->items_count] = 0;

	/* Publishes the item and counts its pixels. */
	builder->items_count++;
	builder->pixels_count += pixel_count;

	/* Succeeded: the image is the list's last item. */
	return 0;
}

/*
 * Takes pixels (RGBA, malloc'd) over for the list, which items then share
 * (ws216-p002: one tile of a tiling pattern, drawn by every fill of the
 * pattern), counting them once against the list's limit.  The pixels are
 * the builder's even when they are refused.
 */
int
pdf_display_keep_pixels(
	struct pdf_display_builder *builder,
	unsigned char *pixels,
	size_t width,
	size_t height)
{
	size_t pixel_count;
	int error;

	/* Keeps the pixels first, so that they are freed whatever happens next. */
	error = grow_images(builder);
	if (error != 0) {
		free(pixels);
		return error;
	}

	/* The list owns the pixels from here on. */
	builder->images[builder->images_count] = pixels;
	builder->images_count++;

	/* Refuses more pixels than one list may hold. */
	pixel_count = width * height;
	if (pixel_count > PDF_DISPLAY_PIXELS_MAX - builder->pixels_count)
		return ENOMEM;

	/* Counts them. */
	builder->pixels_count += pixel_count;

	/* Succeeded: the pixels are the list's. */
	return 0;
}

/*
 * Appends a pattern's fill (ws216-p002): a path in the page's shown space,
 * painted with pixels the list keeps (pdf_display_keep_pixels()) repeated.
 *
 * style supplies the rule, the alpha, the blend mode, the pixels and their
 * size, the matrix of one repeat and whether to interpolate.  The path is
 * copied; the pixels are not.
 */
int
pdf_display_add_pattern(
	struct pdf_display_builder *builder,
	const unsigned char *verbs,
	size_t verb_count,
	const struct pdf_point *points,
	size_t point_count,
	const struct pdf_display_item *style)
{
	struct pdf_display_item *item;
	int error;

	/* Appends the path as a fill would. */
	error = pdf_display_add_path(builder, PDF_ITEM_PATTERN, verbs, verb_count, points, point_count, style);
	if (error != 0)
		return error;

	/* Adds what the pattern paints with. */
	item = &builder->items[builder->items_count - 1];
	item->pixels = style->pixels;
	item->image_width = style->image_width;
	item->image_height = style->image_height;
	memcpy(item->matrix, style->matrix, sizeof(item->matrix));
	item->interpolate = style->interpolate;

	/* Succeeded: the pattern's fill is the list's last item. */
	return 0;
}

/*
 * Appends the end of the innermost clip.
 */
int
pdf_display_add_clip_pop(
	struct pdf_display_builder *builder)
{
	struct pdf_display_item *item;
	int error;

	/* Makes room for the item. */
	error = grow_items(builder);
	if (error != 0)
		return error;

	/* Fills in the item, which has no path. */
	item = &builder->items[builder->items_count];
	memset(item, 0, sizeof(*item));
	item->type = PDF_ITEM_CLIP_POP;
	builder->verb_starts[builder->items_count] = 0;
	builder->point_starts[builder->items_count] = 0;
	builder->items_count++;

	/* Succeeded: the clip ends here. */
	return 0;
}

/*
 * Turns the items' path offsets into pointers and publishes the items in
 * the list.
 *
 * The builder takes no more items afterwards.
 */
void
pdf_display_finish(
	struct pdf_display_builder *builder)
{
	struct pdf_display_item *item;
	size_t index;

	/* Points each path item at its verbs and points, which no longer move. */
	for (index = 0; index < builder->items_count; index++) {
		item = &builder->items[index];
		if (item->type == PDF_ITEM_FILL ||
		    item->type == PDF_ITEM_CLIP_PUSH ||
		    item->type == PDF_ITEM_PATTERN) {
			item->verbs = builder->verbs + builder->verb_starts[index];
			item->points = builder->points + builder->point_starts[index];
		}
	}

	/* Publishes the items. */
	builder->list.items = builder->items;
	builder->list.count = builder->items_count;
}

/* Makes room for one more item, within the limit. */
static int
grow_items(
	struct pdf_display_builder *builder)
{
	struct pdf_display_item *items;
	size_t *verb_starts;
	size_t *point_starts;
	size_t capacity;

	/* Nothing to do while there is room. */
	if (builder->items_count < builder->items_capacity)
		return 0;

	/* Refuses an item past the limit. */
	capacity = next_capacity(builder->items_capacity, builder->items_count + 1, PDF_DISPLAY_ITEMS_MAX);
	if (capacity == 0)
		return ENOMEM;

	/* Grows the items. */
	items = realloc(builder->items, capacity * sizeof(*items));
	if (items == NULL)
		return ENOMEM;
	builder->items = items;

	/* Grows where their verbs start. */
	verb_starts = realloc(builder->verb_starts, capacity * sizeof(*verb_starts));
	if (verb_starts == NULL)
		return ENOMEM;
	builder->verb_starts = verb_starts;

	/* Grows where their points start; only now do all three arrays have the room. */
	point_starts = realloc(builder->point_starts, capacity * sizeof(*point_starts));
	if (point_starts == NULL)
		return ENOMEM;
	builder->point_starts = point_starts;
	builder->items_capacity = capacity;

	/* Succeeded: one more item fits. */
	return 0;
}

/* Makes room for more verbs, within the limit. */
static int
grow_verbs(
	struct pdf_display_builder *builder,
	size_t more)
{
	unsigned char *verbs;
	size_t capacity;

	/* Refuses verbs past the limit. */
	if (more > PDF_DISPLAY_POINTS_MAX - builder->verbs_count)
		return ENOMEM;

	/* Nothing to do while there is room. */
	if (builder->verbs_count + more <= builder->verbs_capacity)
		return 0;

	/* Grows the array. */
	capacity = next_capacity(builder->verbs_capacity, builder->verbs_count + more, PDF_DISPLAY_POINTS_MAX);
	verbs = realloc(builder->verbs, capacity);
	if (verbs == NULL)
		return ENOMEM;
	builder->verbs = verbs;
	builder->verbs_capacity = capacity;

	/* Succeeded: the verbs fit. */
	return 0;
}

/* Makes room for more points, within the limit. */
static int
grow_points(
	struct pdf_display_builder *builder,
	size_t more)
{
	struct pdf_point *points;
	size_t capacity;

	/* Refuses points past the limit. */
	if (more > PDF_DISPLAY_POINTS_MAX - builder->points_count)
		return ENOMEM;

	/* Nothing to do while there is room. */
	if (builder->points_count + more <= builder->points_capacity)
		return 0;

	/* Grows the array. */
	capacity = next_capacity(builder->points_capacity, builder->points_count + more, PDF_DISPLAY_POINTS_MAX);
	points = realloc(builder->points, capacity * sizeof(*points));
	if (points == NULL)
		return ENOMEM;
	builder->points = points;
	builder->points_capacity = capacity;

	/* Succeeded: the points fit. */
	return 0;
}

/* Makes room for one more image's pixels. */
static int
grow_images(
	struct pdf_display_builder *builder)
{
	unsigned char **images;
	size_t capacity;

	/* Nothing to do while there is room. */
	if (builder->images_count < builder->images_capacity)
		return 0;

	/* Refuses an image past the item limit, which bounds the images too. */
	capacity = next_capacity(builder->images_capacity, builder->images_count + 1, PDF_DISPLAY_ITEMS_MAX);
	if (capacity == 0)
		return ENOMEM;

	/* Grows the array. */
	images = realloc(builder->images, capacity * sizeof(*images));
	if (images == NULL)
		return ENOMEM;
	builder->images = images;
	builder->images_capacity = capacity;

	/* Succeeded: one more image fits. */
	return 0;
}

/*
 * Chooses a capacity of at least needed, doubling the old one, within a
 * limit; 0 when needed is past the limit.
 */
static size_t
next_capacity(
	size_t capacity,
	size_t needed,
	size_t limit)
{
	/* Refuses a need past the limit. */
	if (needed > limit)
		return 0;

	/* Doubles from the first capacity until the need fits. */
	if (capacity == 0)
		capacity = PDF_DISPLAY_INITIAL;
	while (capacity < needed)
		capacity *= 2;

	/* Keeps the capacity within the limit. */
	if (capacity > limit)
		capacity = limit;

	/* Succeeded: the new capacity. */
	return capacity;
}
