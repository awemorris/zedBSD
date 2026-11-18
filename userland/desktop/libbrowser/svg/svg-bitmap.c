/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pictures drawn into bitmaps on the CPU.
 */

#include "svg/svg-bitmap.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The most bytes the coverages of all the clips of one drawing take
 * together.  A clip's coverage is kept for as long as the drawing lasts
 * (the commands that share a clip share it), so a picture with many large
 * clips is bounded by this, as the layers are by SVG_BITMAP_LAYER_BYTES_MAX;
 * the header names no limit for clips, so the choice here is the same
 * 64 MiB.
 */
#define BITMAP_CLIP_BYTES_MAX	(64UL << 20)

/*
 * What a coverage holds at one tile: nothing, a full tile, or a partial
 * tile with its alphas.
 */
enum bitmap_tile_kind {
	BITMAP_TILE_EMPTY = 0,
	BITMAP_TILE_FULL = 1,
	BITMAP_TILE_PARTIAL = 2
};

/*
 * One group that is open.  A group drawn as a layer keeps the layer and
 * the surface it will be composited onto; a group drawn into its parent
 * (layer is NULL) keeps the multiplier that was in force before it, which
 * is restored when it ends.
 */
struct bitmap_group {
	uint32_t *layer;
	uint32_t *parent;
	float opacity;
	float outer_multiplier;
};

/*
 * A paint made ready for one fill: a premultiplied color in 0 .. 1 for a
 * color paint, or the gradient and its ramp's width for a gradient paint
 * (the ramp is in the bitmap state), and the factor every pixel's source
 * is multiplied by (the paint's alpha factor and the opacities of the
 * groups drawn into their parent).
 */
struct bitmap_paint {
	int is_gradient;
	const struct vec_gradient *gradient;
	int ramp_width;
	float alpha;
	float red;
	float green;
	float blue;
	float factor;
};

/*
 * The state of one rendering: the bitmap's size, its premultiplied working
 * surface, the surface the drawing goes to now (the working surface or the
 * innermost layer), the open groups, the coverage of every clip described
 * so far, and the scratch the fills are built in.  It is one allocation
 * because of the ramp.
 */
struct bitmap {
	int width;
	int height;
	uint32_t *canvas;
	uint32_t *target;
	struct bitmap_group *groups;
	size_t group_count;
	size_t group_capacity;
	size_t layer_count;
	size_t layer_bytes;
	float multiplier;
	struct vec_coverage *clips;
	size_t clip_count;
	size_t clip_bytes;
	struct vec_irect window;
	struct vec_coverage fill;
	struct vec_coverage part;
	struct vec_coverage united;
	struct vec_coverage met;
	uint32_t ramp[VEC_RAMP_WIDTH_MAX];
};

static int bitmap_sink_clip(void *context, const struct svg_draw_clip *clip);
static int bitmap_sink_fill(void *context, const struct vec_lines *lines, enum vec_fill_rule rule, const struct svg_draw_paint *paint, int clip);
static int bitmap_sink_group(void *context, float opacity);
static int bitmap_sink_group_end(void *context);
static int bitmap_clip_build(struct bitmap *bitmap, const struct svg_draw_clip *clip, struct vec_coverage *result);
static int bitmap_paint_make(struct bitmap *bitmap, const struct svg_draw_paint *paint, struct bitmap_paint *made);
static void bitmap_coverage_paint(struct bitmap *bitmap, const struct bitmap_paint *paint, const struct vec_coverage *clip);
static void bitmap_tile_paint(struct bitmap *bitmap, const struct bitmap_paint *paint, int32_t tx, int32_t ty, const unsigned char *alphas, enum bitmap_tile_kind clip_kind, const struct vec_tile *clip_tile);
static enum bitmap_tile_kind bitmap_tile_find(const struct vec_coverage *coverage, int32_t tx, int32_t ty, const struct vec_tile **tile);
static void bitmap_blend(uint32_t *destination, float alpha, float red, float green, float blue);
static void bitmap_layer_composite(const struct bitmap *bitmap, const uint32_t *layer, uint32_t *parent, float opacity);
static void bitmap_export(const struct bitmap *bitmap, uint32_t *pixels, int stride);
static void bitmap_clear(uint32_t *pixels, int width, int height, int stride);
static void bitmap_release(struct bitmap *bitmap);
static float bitmap_unit(float value);
static uint32_t bitmap_round_channel(float value);
static size_t bitmap_coverage_bytes(const struct vec_coverage *coverage);
static int bitmap_coverage_empty(const struct vec_coverage *coverage);

/*
 * Draws a picture into a bitmap.
 *
 * Values referenced: Compositing and Blending 1 §5.1 and §9.1.4
 * (source-over on premultiplied colors), §8.2 (an isolated group is drawn
 * apart and composited back), SVG 2 §3.6.1 (opacity of a group), §13.4.2
 * (coverage by the fill rule).
 *
 * The drawing goes through svg_picture_draw with a sink that composites
 * each fill into a premultiplied working surface and sends each group to a
 * layer.  The pixels are written once at the end (straight alpha), so a
 * picture that is refused leaves them as they were.
 *
 * Choices where the header is silent: a picture svg_picture_draw refuses
 * (a malformed one) gives its error with the pixels untouched; a fill
 * whose coverage the rasterizer refuses for its size (ENOBUFS) or its
 * numbers (EINVAL) is left out and the rest is drawn; the coverages of
 * the clips are kept up to BITMAP_CLIP_BYTES_MAX and a picture that needs
 * more gives ENOMEM; the layers budget counts the group layers only, not
 * the working surface.
 */
int
svg_picture_render(
	const struct svg_picture *picture,
	const struct vec_matrix *placement,
	uint32_t *pixels,
	int width,
	int height,
	int stride)
{
	struct svg_sink sink;
	struct bitmap *bitmap;
	size_t bytes;
	int placement_valid;
	int error;

	/* Refuses missing arguments. */
	if (picture == NULL || placement == NULL || pixels == NULL)
		return EINVAL;

	/* Refuses a size outside 1 .. SVG_BITMAP_SIDE_MAX and a stride narrower than a row. */
	if (width < 1 || width > SVG_BITMAP_SIDE_MAX)
		return EINVAL;
	if (height < 1 || height > SVG_BITMAP_SIDE_MAX)
		return EINVAL;
	if (stride < width)
		return EINVAL;

	/* Refuses a placement that is not valid. */
	placement_valid = vec_matrix_valid(placement);
	if (!placement_valid)
		return EINVAL;

	/* Allocates the state of the rendering. */
	bitmap = calloc(1, sizeof(*bitmap));
	if (bitmap == NULL) {
		bitmap_clear(pixels, width, height, stride);
		return ENOMEM;
	}

	/* Starts the state: the size, the window of the whole bitmap and the empty coverages. */
	bitmap->width = width;
	bitmap->height = height;
	bitmap->window.left = 0;
	bitmap->window.top = 0;
	bitmap->window.right = width;
	bitmap->window.bottom = height;
	bitmap->multiplier = 1.0f;
	vec_coverage_init(&bitmap->fill);
	vec_coverage_init(&bitmap->part);
	vec_coverage_init(&bitmap->united);
	vec_coverage_init(&bitmap->met);

	/* Allocates the working surface, transparent. */
	bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
	bitmap->canvas = calloc(1, bytes);
	if (bitmap->canvas == NULL) {
		bitmap_release(bitmap);
		bitmap_clear(pixels, width, height, stride);
		return ENOMEM;
	}
	bitmap->target = bitmap->canvas;

	/* Allocates the table of open groups: a group is a command, so the command count is the most. */
	if (picture->command_count != 0) {
		bitmap->groups = calloc(picture->command_count, sizeof(*bitmap->groups));
		if (bitmap->groups == NULL) {
			bitmap_release(bitmap);
			bitmap_clear(pixels, width, height, stride);
			return ENOMEM;
		}
		bitmap->group_capacity = picture->command_count;
	}

	/* Allocates the table of the clips' coverages. */
	if (picture->clip_count != 0) {
		bitmap->clips = calloc(picture->clip_count, sizeof(*bitmap->clips));
		if (bitmap->clips == NULL) {
			bitmap_release(bitmap);
			bitmap_clear(pixels, width, height, stride);
			return ENOMEM;
		}
		bitmap->clip_count = picture->clip_count;
	}

	/* Draws the picture through the compositing sink. */
	sink.context = bitmap;
	sink.clip = bitmap_sink_clip;
	sink.fill = bitmap_sink_fill;
	sink.group = bitmap_sink_group;
	sink.group_end = bitmap_sink_group_end;
	error = svg_picture_draw(picture, placement, VEC_TOLERANCE_DEFAULT, &sink);

	/* Writes the bitmap when the drawing went through. */
	if (error == 0)
		bitmap_export(bitmap, pixels, stride);

	/* Leaves a transparent bitmap when memory ran out. */
	if (error == ENOMEM)
		bitmap_clear(pixels, width, height, stride);

	/* Frees what the rendering held. */
	bitmap_release(bitmap);

	/* Reports why the bitmap was not drawn. */
	if (error != 0)
		return error;

	/* Succeeded: the bitmap holds the picture. */
	return 0;
}

/*
 * Describes a clip: its coverage is built from its parts and its parent
 * and kept for the fills that name it.
 */
static int
bitmap_sink_clip(
	void *context,
	const struct svg_draw_clip *clip)
{
	struct bitmap *bitmap;
	struct vec_coverage result;
	size_t bytes;
	int error;

	bitmap = (struct bitmap *)context;

	/* Refuses a clip the picture does not have. */
	if (clip->index < 0 || (size_t)clip->index >= bitmap->clip_count)
		return EINVAL;

	/* Builds the coverage of the clip. */
	vec_coverage_init(&result);
	error = bitmap_clip_build(bitmap, clip, &result);
	if (error != 0) {
		vec_coverage_release(&result);
		return error;
	}

	/* Refuses a clip that would take the clips' memory beyond its budget. */
	bytes = bitmap_coverage_bytes(&result);
	if (bytes > BITMAP_CLIP_BYTES_MAX || bitmap->clip_bytes > BITMAP_CLIP_BYTES_MAX - bytes) {
		vec_coverage_release(&result);
		return ENOMEM;
	}

	/* Keeps the coverage for the fills. */
	bitmap->clips[clip->index] = result;
	bitmap->clip_bytes += bytes;

	/* Succeeded: the clip is ready for the fills. */
	return 0;
}

/*
 * Builds a clip's coverage into result: the union of its parts (the
 * larger alpha), met with its parent's coverage (the product).  A part
 * the rasterizer refuses for its size or numbers is left out, so the clip
 * only shrinks.  The union is built in bitmap->united, the scratch the
 * clip's calls share.
 */
static int
bitmap_clip_build(
	struct bitmap *bitmap,
	const struct svg_draw_clip *clip,
	struct vec_coverage *result)
{
	size_t index;
	int error;

	/* Starts the union empty. */
	vec_coverage_reset(&bitmap->united);

	/* Rasterizes each part and unites it with those before. */
	for (index = 0;
	     index < clip->part_count;
	     index++) {
		error = vec_rasterize(clip->parts[index].lines, clip->parts[index].rule, 0.0f, 0.0f, &bitmap->window, &bitmap->part);
		if (error == ENOMEM)
			return ENOMEM;
		if (error != 0)
			continue;

		/* Unites the part with the union so far, into the other scratch, and keeps that as the union. */
		error = vec_coverage_unite(&bitmap->met, &bitmap->united, &bitmap->part);
		if (error != 0)
			return error;
		vec_coverage_release(&bitmap->united);
		bitmap->united = bitmap->met;
		vec_coverage_init(&bitmap->met);
	}

	/* Meets the union with the parent's coverage, which an earlier clip call described. */
	if (clip->parent >= 0) {
		if ((size_t)clip->parent >= bitmap->clip_count)
			return EINVAL;
		error = vec_coverage_intersect(result, &bitmap->united, &bitmap->clips[clip->parent]);
		if (error != 0)
			return error;
		return 0;
	}

	/* Takes the union as the clip when there is no parent: its arrays move to the result. */
	*result = bitmap->united;
	vec_coverage_init(&bitmap->united);

	/* Succeeded: result holds the clip's coverage. */
	return 0;
}

/*
 * Composites one fill: the coverage of its polylines times the clip's, the
 * paint, source-over onto the surface the drawing goes to.
 */
static int
bitmap_sink_fill(
	void *context,
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	const struct svg_draw_paint *paint,
	int clip)
{
	struct bitmap *bitmap;
	struct bitmap_paint made;
	const struct vec_coverage *clip_coverage;
	int usable;
	int empty;
	int error;

	bitmap = (struct bitmap *)context;

	/* Refuses a clip the picture does not have. */
	if (clip >= 0 && (size_t)clip >= bitmap->clip_count)
		return EINVAL;

	/* Skips a fill whose clip lets nothing through. */
	clip_coverage = NULL;
	if (clip >= 0) {
		clip_coverage = &bitmap->clips[clip];
		empty = bitmap_coverage_empty(clip_coverage);
		if (empty)
			return 0;
	}

	/* Makes the paint ready; a paint that paints nothing is skipped. */
	usable = bitmap_paint_make(bitmap, paint, &made);
	if (!usable)
		return 0;

	/* Rasterizes the polylines over the bitmap. */
	error = vec_rasterize(lines, rule, 0.0f, 0.0f, &bitmap->window, &bitmap->fill);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0) {
		/* A shape the rasterizer refuses for its size or numbers is left out. */
		return 0;
	}

	/* Composites the coverage with the paint. */
	bitmap_coverage_paint(bitmap, &made, clip_coverage);

	/* Succeeded: the fill is on the surface. */
	return 0;
}

/*
 * Makes a paint ready: the premultiplied color of a color paint, or the
 * ramp of a gradient.  Reports 0 when the paint paints nothing.
 */
static int
bitmap_paint_make(
	struct bitmap *bitmap,
	const struct svg_draw_paint *paint,
	struct bitmap_paint *made)
{
	struct vec_rect area;
	float length;
	float alpha;
	int error;

	/* Starts with a paint that applies the paint's alpha factor and the opacities of the groups drawn into their parent. */
	memset(made, 0, sizeof(*made));
	made->factor = bitmap_unit(paint->alpha) * bitmap->multiplier;

	/* Takes a color's channels premultiplied, in 0 .. 1. */
	if (paint->kind == SVG_SERVER_COLOR) {
		alpha = (float)((paint->color >> 24) & 0xffU) / 255.0f;
		made->alpha = alpha;
		made->red = (float)((paint->color >> 16) & 0xffU) / 255.0f * alpha;
		made->green = (float)((paint->color >> 8) & 0xffU) / 255.0f * alpha;
		made->blue = (float)(paint->color & 0xffU) / 255.0f * alpha;
		return 1;
	}

	/* Skips a paint that is neither a color nor a gradient. */
	if (paint->kind != SVG_SERVER_GRADIENT || paint->gradient == NULL)
		return 0;

	/* Picks the ramp's width from the gradient's length over the bitmap, as every renderer does. */
	area.left = 0.0f;
	area.top = 0.0f;
	area.right = (float)bitmap->width;
	area.bottom = (float)bitmap->height;
	length = vec_gradient_line_length(paint->gradient, &area);
	made->ramp_width = vec_gradient_ramp_width(length);

	/* Fills the ramp; a gradient whose stops are out of range paints nothing. */
	error = vec_gradient_ramp(paint->gradient, made->ramp_width, bitmap->ramp);
	if (error != 0)
		return 0;
	made->is_gradient = 1;
	made->gradient = paint->gradient;

	/* Succeeded: the gradient is sampled per pixel. */
	return 1;
}

/*
 * Composites the fill coverage (bitmap->fill) with the paint onto the
 * target: the full tiles by their runs, then the partial tiles.  A clip
 * is NULL for none.
 */
static void
bitmap_coverage_paint(
	struct bitmap *bitmap,
	const struct bitmap_paint *paint,
	const struct vec_coverage *clip)
{
	const struct vec_coverage *fill;
	const struct vec_run *run;
	const struct vec_tile *tile;
	const struct vec_tile *clip_tile;
	enum bitmap_tile_kind clip_kind;
	size_t index;
	int32_t step;

	fill = &bitmap->fill;

	/* Composites each run of full tiles. */
	for (index = 0;
	     index < fill->run_count;
	     index++) {
		run = &fill->runs[index];
		for (step = 0;
		     step < run->count;
		     step++) {
			clip_kind = BITMAP_TILE_FULL;
			clip_tile = NULL;
			if (clip != NULL)
				clip_kind = bitmap_tile_find(clip, run->tx + step, run->ty, &clip_tile);
			if (clip_kind == BITMAP_TILE_EMPTY)
				continue;
			bitmap_tile_paint(bitmap, paint, run->tx + step, run->ty, NULL, clip_kind, clip_tile);
		}
	}

	/* Composites each partial tile. */
	for (index = 0;
	     index < fill->tile_count;
	     index++) {
		tile = &fill->tiles[index];
		clip_kind = BITMAP_TILE_FULL;
		clip_tile = NULL;
		if (clip != NULL)
			clip_kind = bitmap_tile_find(clip, tile->tx, tile->ty, &clip_tile);
		if (clip_kind == BITMAP_TILE_EMPTY)
			continue;
		bitmap_tile_paint(bitmap, paint, tile->tx, tile->ty, tile->alpha, clip_kind, clip_tile);
	}
}

/*
 * Composites one tile: each pixel's source is the paint times the fill's
 * coverage over 255 times the clip's over 255 (a full tile has alpha 255,
 * and no clip is a full clip), blended source-over onto the target.  The
 * tile's alphas are NULL for a full tile.
 */
static void
bitmap_tile_paint(
	struct bitmap *bitmap,
	const struct bitmap_paint *paint,
	int32_t tx,
	int32_t ty,
	const unsigned char *alphas,
	enum bitmap_tile_kind clip_kind,
	const struct vec_tile *clip_tile)
{
	uint32_t sample;
	int tile_left;
	int tile_top;
	int left;
	int top;
	int right;
	int bottom;
	int row;
	int column;
	int at;
	int fill_alpha;
	int clip_alpha;
	float factor;
	float red;
	float green;
	float blue;
	float alpha;

	/* Finds the tile's pixels inside the bitmap (the tile grid is inside the window, so this only trims). */
	tile_left = (int)tx * VEC_TILE_SIZE;
	tile_top = (int)ty * VEC_TILE_SIZE;
	left = tile_left;
	top = tile_top;
	right = tile_left + VEC_TILE_SIZE;
	bottom = tile_top + VEC_TILE_SIZE;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > bitmap->width)
		right = bitmap->width;
	if (bottom > bitmap->height)
		bottom = bitmap->height;

	/* Composites each pixel of the tile. */
	for (row = top;
	     row < bottom;
	     row++) {
		for (column = left;
		     column < right;
		     column++) {
			at = (row - tile_top) * VEC_TILE_SIZE + (column - tile_left);

			/* Takes the fill's and the clip's coverage here and skips a pixel nothing covers. */
			fill_alpha = 255;
			if (alphas != NULL)
				fill_alpha = alphas[at];
			if (fill_alpha == 0)
				continue;
			clip_alpha = 255;
			if (clip_kind == BITMAP_TILE_PARTIAL)
				clip_alpha = clip_tile->alpha[at];
			if (clip_alpha == 0)
				continue;

			/* Takes the paint's color; a gradient's is the ramp's at this pixel's centre. */
			alpha = paint->alpha;
			red = paint->red;
			green = paint->green;
			blue = paint->blue;
			if (paint->is_gradient) {
				sample = vec_gradient_sample(paint->gradient, bitmap->ramp, paint->ramp_width, (float)column + 0.5f, (float)row + 0.5f);
				if (sample == 0)
					continue;
				alpha = (float)(sample >> 24) / 255.0f;
				red = (float)((sample >> 16) & 0xffU) / 255.0f;
				green = (float)((sample >> 8) & 0xffU) / 255.0f;
				blue = (float)(sample & 0xffU) / 255.0f;
			}

			/* Multiplies the source by the coverage and the paint's factor and blends it. */
			factor = ((float)fill_alpha / 255.0f) * ((float)clip_alpha / 255.0f) * paint->factor;
			bitmap_blend(&bitmap->target[(size_t)row * (size_t)bitmap->width + (size_t)column], alpha * factor, red * factor, green * factor, blue * factor);
		}
	}
}

/*
 * Finds what a coverage holds at one tile: the partial tile (reported
 * through tile), a full tile, or nothing.  Both arrays are sorted by row
 * then column.
 */
static enum bitmap_tile_kind
bitmap_tile_find(
	const struct vec_coverage *coverage,
	int32_t tx,
	int32_t ty,
	const struct vec_tile **tile)
{
	size_t low;
	size_t high;
	size_t middle;
	int before;

	/* Searches the partial tiles for the first one at or after the tile. */
	low = 0;
	high = coverage->tile_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		before = 0;
		if (coverage->tiles[middle].ty < ty)
			before = 1;
		else if (coverage->tiles[middle].ty == ty && coverage->tiles[middle].tx < tx)
			before = 1;
		if (before)
			low = middle + 1;
		else
			high = middle;
	}
	if (low < coverage->tile_count && coverage->tiles[low].ty == ty && coverage->tiles[low].tx == tx) {
		*tile = &coverage->tiles[low];
		return BITMAP_TILE_PARTIAL;
	}

	/* Searches the runs for the first one that starts after the tile. */
	low = 0;
	high = coverage->run_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		before = 0;
		if (coverage->runs[middle].ty < ty)
			before = 1;
		else if (coverage->runs[middle].ty == ty && coverage->runs[middle].tx <= tx)
			before = 1;
		if (before)
			low = middle + 1;
		else
			high = middle;
	}

	/* The run before it, when it is in the tile's row, is the only one that can hold the tile. */
	if (low > 0 && coverage->runs[low - 1].ty == ty) {
		if (tx - coverage->runs[low - 1].tx < coverage->runs[low - 1].count)
			return BITMAP_TILE_FULL;
	}

	/* Nothing covers the tile. */
	return BITMAP_TILE_EMPTY;
}

/*
 * Blends a premultiplied source (components in 0 .. 1, already times the
 * coverage and opacity) over a premultiplied destination pixel:
 * result = source + destination * (1 - source alpha), in single
 * precision, each channel rounded to the nearest 8-bit value.
 */
static void
bitmap_blend(
	uint32_t *destination,
	float alpha,
	float red,
	float green,
	float blue)
{
	uint32_t old;
	float inverse;
	float result_alpha;
	float result_red;
	float result_green;
	float result_blue;

	/* Takes the weight the destination keeps. */
	old = *destination;
	inverse = 1.0f - alpha;

	/* Sums the source and the weighted destination for each channel. */
	result_alpha = alpha * 255.0f + (float)(old >> 24) * inverse;
	result_red = red * 255.0f + (float)((old >> 16) & 0xffU) * inverse;
	result_green = green * 255.0f + (float)((old >> 8) & 0xffU) * inverse;
	result_blue = blue * 255.0f + (float)(old & 0xffU) * inverse;

	/* Stores the rounded channels. */
	*destination = (bitmap_round_channel(result_alpha) << 24) | (bitmap_round_channel(result_red) << 16) |
	    (bitmap_round_channel(result_green) << 8) | bitmap_round_channel(result_blue);
}

/*
 * Starts a group: a layer of the bitmap's size, cleared and drawn into
 * until the group ends, or, when the nesting or the memory is at its
 * limit, no layer and the opacity applied to each fill inside.
 */
static int
bitmap_sink_group(
	void *context,
	float opacity)
{
	struct bitmap *bitmap;
	struct bitmap_group *group;
	size_t bytes;
	int layered;

	bitmap = (struct bitmap *)context;

	/* Refuses more groups than the picture has commands. */
	if (bitmap->group_count >= bitmap->group_capacity)
		return EINVAL;
	group = &bitmap->groups[bitmap->group_count];
	opacity = bitmap_unit(opacity);

	/*
	 * Decides whether this group gets a layer: not deeper than
	 * SVG_BITMAP_LAYERS_MAX and not beyond SVG_BITMAP_LAYER_BYTES_MAX of
	 * layers alive.  A group inside one that is drawn into its parent is
	 * decided by the same two numbers, which have not changed, so it is
	 * drawn into its parent as well.
	 */
	bytes = (size_t)bitmap->width * (size_t)bitmap->height * sizeof(uint32_t);
	layered = 1;
	if (bitmap->layer_count >= SVG_BITMAP_LAYERS_MAX)
		layered = 0;
	else if (bytes > SVG_BITMAP_LAYER_BYTES_MAX || bitmap->layer_bytes > SVG_BITMAP_LAYER_BYTES_MAX - bytes)
		layered = 0;

	/* Draws into the parent with the opacity multiplied into each fill. */
	if (!layered) {
		group->layer = NULL;
		group->parent = bitmap->target;
		group->opacity = opacity;
		group->outer_multiplier = bitmap->multiplier;
		bitmap->multiplier *= opacity;
		bitmap->group_count++;
		return 0;
	}

	/* Allocates the layer, transparent, and draws into it from now on. */
	group->layer = calloc(1, bytes);
	if (group->layer == NULL)
		return ENOMEM;
	group->parent = bitmap->target;
	group->opacity = opacity;
	group->outer_multiplier = bitmap->multiplier;
	bitmap->target = group->layer;
	bitmap->layer_count++;
	bitmap->layer_bytes += bytes;
	bitmap->group_count++;

	/* Succeeded: the group draws into its layer. */
	return 0;
}

/*
 * Ends the innermost group: a layer is composited onto its parent with the
 * group's opacity and freed, and a group drawn into its parent gives back
 * the multiplier it had taken.
 */
static int
bitmap_sink_group_end(
	void *context)
{
	struct bitmap *bitmap;
	struct bitmap_group *group;
	size_t bytes;

	bitmap = (struct bitmap *)context;

	/* Refuses an end with no group open. */
	if (bitmap->group_count == 0)
		return EINVAL;
	bitmap->group_count--;
	group = &bitmap->groups[bitmap->group_count];

	/* Gives back the multiplier of a group that was drawn into its parent. */
	if (group->layer == NULL) {
		bitmap->multiplier = group->outer_multiplier;
		return 0;
	}

	/* Composites the layer onto the surface that was being drawn into, and frees it. */
	bitmap_layer_composite(bitmap, group->layer, group->parent, group->opacity);
	bytes = (size_t)bitmap->width * (size_t)bitmap->height * sizeof(uint32_t);
	free(group->layer);
	group->layer = NULL;
	bitmap->target = group->parent;
	bitmap->layer_count--;
	bitmap->layer_bytes -= bytes;

	/* Succeeded: the group is on its parent. */
	return 0;
}

/*
 * Composites a layer onto its parent: each pixel of the layer times the
 * opacity, source-over.
 */
static void
bitmap_layer_composite(
	const struct bitmap *bitmap,
	const uint32_t *layer,
	uint32_t *parent,
	float opacity)
{
	size_t count;
	size_t index;
	uint32_t pixel;
	float alpha;
	float red;
	float green;
	float blue;

	/* Blends every pixel that is not transparent. */
	count = (size_t)bitmap->width * (size_t)bitmap->height;
	for (index = 0;
	     index < count;
	     index++) {
		pixel = layer[index];
		if (pixel == 0)
			continue;
		alpha = (float)(pixel >> 24) / 255.0f * opacity;
		red = (float)((pixel >> 16) & 0xffU) / 255.0f * opacity;
		green = (float)((pixel >> 8) & 0xffU) / 255.0f * opacity;
		blue = (float)(pixel & 0xffU) / 255.0f * opacity;
		bitmap_blend(&parent[index], alpha, red, green, blue);
	}
}

/*
 * Writes the working surface to the caller's pixels with straight alpha:
 * each premultiplied channel divided by the alpha, rounded to nearest.
 */
static void
bitmap_export(
	const struct bitmap *bitmap,
	uint32_t *pixels,
	int stride)
{
	uint32_t pixel;
	uint32_t alpha;
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	int row;
	int column;

	/* Converts every pixel of every row. */
	for (row = 0;
	     row < bitmap->height;
	     row++) {
		for (column = 0;
		     column < bitmap->width;
		     column++) {
			pixel = bitmap->canvas[(size_t)row * (size_t)bitmap->width + (size_t)column];
			alpha = pixel >> 24;
			if (alpha == 0) {
				/* A transparent pixel has no color. */
				pixels[(size_t)row * (size_t)stride + (size_t)column] = 0;
				continue;
			}
			red = (((pixel >> 16) & 0xffU) * 255U + alpha / 2U) / alpha;
			green = (((pixel >> 8) & 0xffU) * 255U + alpha / 2U) / alpha;
			blue = ((pixel & 0xffU) * 255U + alpha / 2U) / alpha;
			if (red > 255U)
				red = 255U;
			if (green > 255U)
				green = 255U;
			if (blue > 255U)
				blue = 255U;
			pixels[(size_t)row * (size_t)stride + (size_t)column] = (alpha << 24) | (red << 16) | (green << 8) | blue;
		}
	}
}

/* Makes the bitmap's width by height pixels transparent, leaving what the stride skips alone. */
static void
bitmap_clear(
	uint32_t *pixels,
	int width,
	int height,
	int stride)
{
	int row;

	/* Clears each row's visible pixels. */
	for (row = 0;
	     row < height;
	     row++)
		memset(&pixels[(size_t)row * (size_t)stride], 0, (size_t)width * sizeof(uint32_t));
}

/* Frees everything the state holds, the layers of groups still open included. */
static void
bitmap_release(
	struct bitmap *bitmap)
{
	size_t index;

	/* Frees the layers of groups a failed drawing left open. */
	for (index = 0;
	     index < bitmap->group_count;
	     index++)
		free(bitmap->groups[index].layer);

	/* Frees the clips' coverages. */
	for (index = 0;
	     index < bitmap->clip_count;
	     index++)
		vec_coverage_release(&bitmap->clips[index]);

	/* Frees the scratch coverages and the arrays. */
	vec_coverage_release(&bitmap->fill);
	vec_coverage_release(&bitmap->part);
	vec_coverage_release(&bitmap->united);
	vec_coverage_release(&bitmap->met);
	free(bitmap->clips);
	free(bitmap->groups);
	free(bitmap->canvas);
	free(bitmap);
}

/* Clamps an opacity or alpha factor to 0 .. 1; a value that is not a number is 0. */
static float
bitmap_unit(
	float value)
{
	/* Takes a NaN as 0 (it is the only value not equal to itself). */
	if (value != value)
		return 0.0f;
	if (value < 0.0f)
		return 0.0f;
	if (value > 1.0f)
		return 1.0f;

	/* Reports the value as it is. */
	return value;
}

/* Rounds a channel in 0 .. 255 (float) to the nearest 8-bit value, clamping first. */
static uint32_t
bitmap_round_channel(
	float value)
{
	/* Clamps so that the conversion to an integer is defined (a NaN gives 0). */
	if (!(value > 0.0f))
		return 0;
	if (value >= 255.0f)
		return 255;

	/* Reports the nearest integer. */
	return (uint32_t)(value + 0.5f);
}

/* Reports the bytes a coverage's arrays take. */
static size_t
bitmap_coverage_bytes(
	const struct vec_coverage *coverage)
{
	size_t bytes;

	/* Counts the arrays' capacities. */
	bytes = coverage->tile_capacity * sizeof(struct vec_tile);
	bytes += coverage->run_capacity * sizeof(struct vec_run);

	/* Reports the total. */
	return bytes;
}

/* Tells whether a coverage holds no pixel at all. */
static int
bitmap_coverage_empty(
	const struct vec_coverage *coverage)
{
	/* A coverage with neither a partial tile nor a run is empty. */
	if (coverage->tile_count == 0 && coverage->run_count == 0)
		return 1;

	/* It holds something. */
	return 0;
}
