/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binds of the frame being recorded (ws211-p003 段 1): a pipeline and a
 * descriptor set are bound into the display list only when they differ
 * from what it has bound since its recording or its last pass began, and
 * the commands recorded are counted for KWL DRAWS (review-1 M2).  A set is
 * the same binding only with the same pipeline layout (review-1 M1).
 *
 * The draws of the frame being recorded: a quad wholly outside what the
 * frame
 * draws is not drawn, and while the frame's damage is two or more
 * rectangles apart, a quad is drawn once for each of them it meets, with
 * the scissor cut to that rectangle; the rectangles do not overlap, so no
 * pixel is drawn twice.
 *
 * While a layer's picture is drawn, the draws' box is gathered
 * (kwl_compose_bounds_begin and _end), so that a frame draws only the part
 * of the picture that holds anything.
 *
 * The plane is measured in logical units and a pass draws pixels: the
 * output's pass, its layers' pictures and its maps show the plane at the
 * output's scale, a head's pass at 100 %.  A quad's corners are snapped
 * to the pixels each (scale.h), so quads that touch on the plane touch in
 * the picture; a damage is taken outward to every pixel it touches.  An
 * image is sampled a texel a pixel where it is shown one for one, and
 * smoothly where a scale stretches it by a fraction.  At 100 % a unit is a
 * pixel, nothing is rounded and images are sampled as they always were.
 */

#include "compose.h"

#include <math.h>
#include <string.h>

static int clip_active(const struct kwl_compose *compose);
static int clip_cut(const VkRect2D *clip, const VkRect2D *scissor, VkRect2D *cut);
static int clip_meets(const VkRect2D *rect, float x, float y, float width, float height);
static int clip_same(const VkRect2D *first, const VkRect2D *second);
static void clip_box(float *x, float *y, float *width, float *height);
static void bounds_add(struct kwl_compose *compose, float left, float top, float right, float bottom);

/*
 * Gives the scale the pass being recorded shows the plane at, in 120ths:
 * the output's in its own pass, its layers' pictures and its maps, and
 * 100 % in a head's pass, which shows its part of the plane a pixel for a
 * unit.
 */
unsigned
kwl_compose_pass_scale(
	const struct kwl_server *server)
{
	/* A head's pass shows its part of the plane unscaled. */
	if (server->view_width != 0U)
		return KWL_SCALE_ONE;

	/* An output whose scale is not set yet is shown at 100 %. */
	if (server->scale120 == 0U)
		return KWL_SCALE_ONE;

	/* The output's scale. */
	return server->scale120;
}

/*
 * Gives the size in pixels of the picture the pass being recorded draws
 * the plane into: the output's (the mode turned by its angle), or a
 * head's.
 */
void
kwl_compose_pass_size(
	const struct kwl_server *server,
	float *width,
	float *height)
{
	/* A head's own size. */
	if (server->view_width != 0U) {
		*width = (float)server->view_width;
		*height = (float)server->view_height;
		return;
	}

	/* The output's size in logical units while its pixels are not known yet. */
	if (server->pixel_width == 0U || server->pixel_height == 0U) {
		*width = (float)server->width;
		*height = (float)server->height;
		return;
	}

	/* The output's pixels. */
	*width = (float)server->pixel_width;
	*height = (float)server->pixel_height;
}

/*
 * Maps a rectangle of the plane (left, top, width, height, measured from
 * the pass's left top corner: a head's view taken off) to the pixels of
 * the pass being recorded: each corner snapped to a pixel.  At 100 % it
 * is given as it is.  plane and pixels may be the same array.
 */
void
kwl_compose_pass_rect(
	const struct kwl_server *server,
	const float *plane,
	float *pixels)
{
	unsigned scale;
	float left;
	float top;
	float right;
	float bottom;

	/* At 100 % a unit is a pixel, unrounded (an animation's place keeps its fraction). */
	scale = kwl_compose_pass_scale(server);
	if (scale == KWL_SCALE_ONE) {
		memmove(pixels, plane, 4U * sizeof(float));
		return;
	}

	/* The left top and the right bottom corners, snapped each. */
	left = kwl_scale_snap_float(plane[0], scale);
	top = kwl_scale_snap_float(plane[1], scale);
	right = kwl_scale_snap_float(plane[0] + plane[2], scale);
	bottom = kwl_scale_snap_float(plane[1] + plane[3], scale);

	/* The rectangle between them. */
	pixels[0] = left;
	pixels[1] = top;
	pixels[2] = right - left;
	pixels[3] = bottom - top;
}

/*
 * Maps a place on the plane (a coordinate measured from the pass's left
 * top corner) to the pixel of the pass being recorded it is snapped to.
 */
int32_t
kwl_compose_pass_pixel(
	const struct kwl_server *server,
	int32_t plane)
{
	unsigned scale;
	int32_t pixel;

	/* The coordinate snapped at the pass's scale. */
	scale = kwl_compose_pass_scale(server);
	pixel = kwl_scale_snap(plane, scale);

	/* The pixel. */
	return pixel;
}

/*
 * Maps a damage of the output's plane (left, top, right, bottom) to the
 * output's pixels it touches: its left top corner rounded down, its right
 * bottom corner up.
 */
void
kwl_compose_damage_pixels(
	const struct kwl_server *server,
	const int32_t *plane,
	int32_t *pixels)
{
	struct kwl_scale_rect logical;
	struct kwl_scale_rect covered;
	unsigned scale;

	/* At 100 % the damage is in pixels already. */
	scale = server->scale120;
	if (scale == 0U || scale == KWL_SCALE_ONE) {
		memcpy(pixels, plane, 4U * sizeof(int32_t));
		return;
	}

	/* An empty damage touches no pixel: it stays as it is. */
	if (plane[2] <= plane[0] || plane[3] <= plane[1]) {
		memcpy(pixels, plane, 4U * sizeof(int32_t));
		return;
	}

	/* The damage as a rectangle. */
	logical.x = plane[0];
	logical.y = plane[1];
	logical.width = (uint32_t)((int64_t)plane[2] - plane[0]);
	logical.height = (uint32_t)((int64_t)plane[3] - plane[1]);

	/* Every pixel it touches. */
	kwl_scale_cover_rect(&logical, scale, &covered);
	pixels[0] = covered.x;
	pixels[1] = covered.y;
	pixels[2] = (int32_t)((int64_t)covered.x + covered.width);
	pixels[3] = (int32_t)((int64_t)covered.y + covered.height);
}

/*
 * Chooses how an image drawn as a quad of the plane (its size in logical
 * units, the part of it shown in uv) is sampled in the pass being
 * recorded: a texel a pixel (its nearest set) where it is shown one for
 * one, or at a whole-number scale from a picture of its logical size (each
 * texel a block of pixels); smoothly (its linear set) where the scale
 * stretches it by a fraction or an animation scales it.  At 100 % it is
 * sampled as it always was.
 */
VkDescriptorSet
kwl_compose_image_set(
	const struct kwl_server *server,
	const struct kwl_import *import,
	float width,
	float height,
	const float *uv)
{
	unsigned scale;
	float shown_width;
	float shown_height;
	float pixel_width;
	float pixel_height;
	float off_width;
	float off_height;

	/* At 100 %, or without a linear set, the image is sampled as before. */
	scale = kwl_compose_pass_scale(server);
	if (scale == KWL_SCALE_ONE)
		return import->set;
	if (import->linear_set == VK_NULL_HANDLE)
		return import->set;

	/* A layer's animation scales whatever it shows. */
	if (server->layer_on && server->layer_scale != 1.0f)
		return import->linear_set;

	/* The texels the quad shows, and the pixels it covers. */
	shown_width = (uv[2] - uv[0]) * (float)import->width;
	shown_height = (uv[3] - uv[1]) * (float)import->height;
	pixel_width = width * (float)scale / (float)KWL_SCALE_ONE;
	pixel_height = height * (float)scale / (float)KWL_SCALE_ONE;

	/*
	 * A buffer drawn at the output's scale, a texel a pixel: the size a
	 * client rounds its buffer to may be a pixel off the quad's.
	 */
	off_width = fabsf(pixel_width - shown_width);
	off_height = fabsf(pixel_height - shown_height);
	if (off_width <= 1.0f && off_height <= 1.0f)
		return import->set;

	/* A buffer of the logical size at a whole-number scale: each texel a block of pixels, kept sharp. */
	off_width = fabsf(width - shown_width);
	off_height = fabsf(height - shown_height);
	if (scale % KWL_SCALE_ONE == 0U && off_width <= 0.5f && off_height <= 0.5f)
		return import->set;

	/* Stretched by a fraction: sampled smoothly. */
	return import->linear_set;
}

/*
 * Binds a pipeline and a descriptor set (set 0, with the pipeline layout it
 * is used with) into the display list, each only when it differs from what
 * is bound (ws211-p003 段 1); the commands are counted for the log.
 */
void
kwl_compose_bind(
	struct kwl_server *server,
	struct kwl_list *command,
	VkPipeline pipeline,
	VkPipelineLayout layout,
	VkDescriptorSet set)
{
	struct kwl_compose *compose;
	unsigned pipelines;
	unsigned sets;

	/* The pipeline, when another is bound. */
	compose = server->compose;
	pipelines = 0U;
	if (pipeline != compose->bound_pipeline) {
		kwl_list_bind_pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		compose->bound_pipeline = pipeline;
		pipelines = 1U;
	}

	/* The set, when another is bound or it was bound with another layout (review-1 M1). */
	sets = 0U;
	if (set != compose->bound_set || layout != compose->bound_layout) {
		kwl_list_bind_descriptor_sets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0U, 1U, &set, 0U, NULL);
		compose->bound_set = set;
		compose->bound_layout = layout;
		sets = 1U;
	}

	/* Counted for the log. */
	if (server->log_frames) {
		server->draw_counts.cmd_pipelines += pipelines;
		server->draw_counts.cmd_sets += sets;
	}
}

/*
 * Forgets what the display list has bound (its drawing or a pass began,
 * or a set was given back): the next draw binds its pipeline and set.
 */
void
kwl_compose_bind_forget(
	struct kwl_compose *compose)
{
	/* Nothing is known as bound. */
	compose->bound_pipeline = VK_NULL_HANDLE;
	compose->bound_layout = VK_NULL_HANDLE;
	compose->bound_set = VK_NULL_HANDLE;
}

/* Counts push constants and draws recorded, for the log (KWL DRAWS cmds). */
void
kwl_compose_count_commands(
	struct kwl_server *server,
	unsigned pushes,
	unsigned draws)
{
	/* Only with --log-frames. */
	if (!server->log_frames)
		return;
	server->draw_counts.cmd_pushes += pushes;
	server->draw_counts.cmd_draws += draws;
}

/*
 * Tells whether a quad at a place and size in the pass's pixels can show
 * in what the frame draws: the scissor, and while the damage's rectangles
 * are drawn apart, one of them.
 *
 * A pixel of slack is kept for the viewport's rounding.  The backdrop's
 * small passes draw the whole scene under a window, so every quad shows.
 */
int
kwl_compose_clip_visible(
	struct kwl_compose *compose,
	float x,
	float y,
	float width,
	float height)
{
	VkRect2D cut;
	unsigned index;
	int active;
	int meets;
	int left;

	/* The backdrop needs every source quad, including those outside the damage. */
	if (compose->backdrop_recording != 0U)
		return 1;

	/* Outside the scissor: nothing of it is drawn. */
	clip_box(&x, &y, &width, &height);
	meets = clip_meets(&compose->scissor_now, x, y, width, height);
	if (!meets)
		return 0;

	/* Without rectangles drawn apart, the scissor decides. */
	active = clip_active(compose);
	if (!active)
		return 1;

	/* One rectangle, inside the scissor, that the quad meets. */
	for (index = 0U; index < compose->clip_count; index++) {
		/* A rectangle the scissor leaves nothing of. */
		left = clip_cut(&compose->clips[index], &compose->scissor_now, &cut);
		if (!left)
			continue;

		/* The quad shows there. */
		meets = clip_meets(&cut, x, y, width, height);
		if (meets)
			return 1;
	}

	/* Between the rectangles: the image keeps what it shows there. */
	return 0;
}

/*
 * Draws a quad's vertices, whose bound pipeline, set and constants are
 * recorded, at a place and size in the pass's pixels.
 *
 * While the damage's rectangles are drawn apart it is drawn once for each
 * rectangle it meets, with the scissor cut to that rectangle; otherwise
 * once, within the scissor.
 */
void
kwl_compose_clip_draw(
	struct kwl_server *server,
	struct kwl_list *command,
	float x,
	float y,
	float width,
	float height,
	uint32_t vertices)
{
	struct kwl_compose *compose;
	VkRect2D cut;
	unsigned index;
	int known;
	int active;
	int meets;
	int left;
	int same;

	/* Whether the scissor this file set last is still the list's (no one set another since). */
	compose = server->compose;
	known = 0;
	if (compose->clip_scissor_valid && command->scissor_serial == compose->clip_serial)
		known = 1;

	/* The box of what is drawn, when it is gathered. */
	if (compose->bounding)
		bounds_add(compose, x, y, x + width, y + height);

	/* Without rectangles drawn apart: one draw, within the frame's scissor, set back if a rectangle's was left. */
	active = clip_active(compose);
	if (!active) {
		if (known) {
			kwl_list_set_scissor(command, 0U, 1U, &compose->scissor_now);
			compose->clip_scissor_valid = 0U;
		}
		kwl_list_draw(command, vertices, 1U, 0U, 0U);
		return;
	}

	/* Each rectangle the quad meets, inside the scissor. */
	clip_box(&x, &y, &width, &height);
	for (index = 0U; index < compose->clip_count; index++) {
		/* A rectangle the scissor leaves nothing of. */
		left = clip_cut(&compose->clips[index], &compose->scissor_now, &cut);
		if (!left)
			continue;

		/* A rectangle the quad does not reach. */
		meets = clip_meets(&cut, x, y, width, height);
		if (!meets)
			continue;

		/*
		 * The scissor cut to the rectangle, unless the list has it already:
		 * the draws of one place follow one another, so most need none.
		 */
		same = 0;
		if (known)
			same = clip_same(&compose->clip_scissor, &cut);
		if (!same) {
			kwl_list_set_scissor(command, 0U, 1U, &cut);
			compose->clip_scissor = cut;
			compose->clip_serial = command->scissor_serial;
			compose->clip_scissor_valid = 1U;
			known = 1;
		}

		/* The quad's part in the rectangle. */
		kwl_list_draw(command, vertices, 1U, 0U, 0U);
	}
}

/*
 * Draws a quad's vertices, whose bound pipeline, set and constants are
 * recorded, only inside a rectangle of the pass's pixels: the scissor is
 * the rectangle cut to the frame's scissor and, while the damage's
 * rectangles are drawn apart, to each of them.  A shape is drawn this way
 * in parts (its opaque inside and its blended edges, glass.c), every pixel
 * by exactly one part.
 */
void
kwl_compose_clip_draw_rect(
	struct kwl_server *server,
	struct kwl_list *command,
	const VkRect2D *rect,
	uint32_t vertices)
{
	struct kwl_compose *compose;
	VkRect2D inside;
	VkRect2D cut;
	unsigned index;
	int active;
	int left;

	/* The rectangle inside the frame's scissor; nothing of it is drawn outside. */
	compose = server->compose;
	left = clip_cut(rect, &compose->scissor_now, &inside);
	if (!left)
		return;

	/* The box of what is drawn, when it is gathered. */
	if (compose->bounding)
		bounds_add(compose, (float)inside.offset.x, (float)inside.offset.y, (float)inside.offset.x + (float)inside.extent.width, (float)inside.offset.y + (float)inside.extent.height);

	/* Without rectangles drawn apart: one draw within the rectangle. */
	active = clip_active(compose);
	if (!active) {
		kwl_list_set_scissor(command, 0U, 1U, &inside);
		compose->clip_scissor = inside;
		compose->clip_serial = command->scissor_serial;
		compose->clip_scissor_valid = 1U;
		kwl_list_draw(command, vertices, 1U, 0U, 0U);
		return;
	}

	/* Each damage rectangle the rectangle meets. */
	for (index = 0U; index < compose->clip_count; index++) {
		left = clip_cut(&compose->clips[index], &inside, &cut);
		if (!left)
			continue;
		kwl_list_set_scissor(command, 0U, 1U, &cut);
		compose->clip_scissor = cut;
		compose->clip_serial = command->scissor_serial;
		compose->clip_scissor_valid = 1U;
		kwl_list_draw(command, vertices, 1U, 0U, 0U);
	}
}

/* Starts gathering the box of the draws (a layer's picture being drawn). */
void
kwl_compose_bounds_begin(
	struct kwl_compose *compose)
{
	/* Nothing drawn yet. */
	compose->bounding = 1U;
	compose->bounded = 0U;
}

/*
 * Ends gathering the box of the draws, giving it (left, top, right,
 * bottom) through bounds.  Returns 1 when anything was drawn.
 */
int
kwl_compose_bounds_end(
	struct kwl_compose *compose,
	float *bounds)
{
	/* No longer gathered. */
	compose->bounding = 0U;
	if (!compose->bounded)
		return 0;

	/* Succeeded: the box. */
	bounds[0] = compose->bounds[0];
	bounds[1] = compose->bounds[1];
	bounds[2] = compose->bounds[2];
	bounds[3] = compose->bounds[3];
	return 1;
}

/* Tells whether the damage's rectangles are drawn apart in the pass being recorded: only the anchor's, not a backdrop's or a layer's. */
static int
clip_active(
	const struct kwl_compose *compose)
{
	/* One rectangle is the scissor itself. */
	if (compose->clip_count < 2U)
		return 0;

	/* The backdrop's small passes draw the whole scene in their own coordinates. */
	if (compose->backdrop_recording != 0U)
		return 0;

	/* Another pass than the anchor's (a layer's picture drawn offscreen). */
	if (compose->framebuffer_now != compose->clip_framebuffer)
		return 0;

	/* Succeeded: the anchor's pass draws the rectangles apart. */
	return 1;
}

/* Cuts a rectangle to the scissor; returns 0 when nothing is left. */
static int
clip_cut(
	const VkRect2D *clip,
	const VkRect2D *scissor,
	VkRect2D *cut)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	/* The larger of the left and top edges. */
	left = clip->offset.x;
	if (scissor->offset.x > left)
		left = scissor->offset.x;
	top = clip->offset.y;
	if (scissor->offset.y > top)
		top = scissor->offset.y;

	/* The smaller of the right and bottom edges. */
	right = (int64_t)clip->offset.x + clip->extent.width;
	if ((int64_t)scissor->offset.x + scissor->extent.width < right)
		right = (int64_t)scissor->offset.x + scissor->extent.width;
	bottom = (int64_t)clip->offset.y + clip->extent.height;
	if ((int64_t)scissor->offset.y + scissor->extent.height < bottom)
		bottom = (int64_t)scissor->offset.y + scissor->extent.height;

	/* Nothing in common. */
	if (right <= left || bottom <= top)
		return 0;

	/* Succeeded: the part of the rectangle inside the scissor. */
	cut->offset.x = (int32_t)left;
	cut->offset.y = (int32_t)top;
	cut->extent.width = (uint32_t)(right - left);
	cut->extent.height = (uint32_t)(bottom - top);
	return 1;
}

/* Tells whether a quad (with a pixel of slack for the viewport's rounding) meets a rectangle. */
static int
clip_meets(
	const VkRect2D *rect,
	float x,
	float y,
	float width,
	float height)
{
	/* Wholly left of it, or above it. */
	if (x + width < (float)rect->offset.x - 1.0f)
		return 0;
	if (y + height < (float)rect->offset.y - 1.0f)
		return 0;

	/* Wholly right of it, or below it. */
	if (x > (float)rect->offset.x + (float)rect->extent.width + 1.0f)
		return 0;
	if (y > (float)rect->offset.y + (float)rect->extent.height + 1.0f)
		return 0;

	/* It meets. */
	return 1;
}

/* Tells whether two scissors are the same rectangle. */
static int
clip_same(
	const VkRect2D *first,
	const VkRect2D *second)
{
	/* Another place. */
	if (first->offset.x != second->offset.x || first->offset.y != second->offset.y)
		return 0;

	/* Another size. */
	if (first->extent.width != second->extent.width || first->extent.height != second->extent.height)
		return 0;

	/* The same. */
	return 1;
}

/* Turns a quad drawn with a negative width or height (mirrored) into the box it covers. */
static void
clip_box(
	float *x,
	float *y,
	float *width,
	float *height)
{
	/* Drawn to the left of its place. */
	if (*width < 0.0f) {
		*x += *width;
		*width = -*width;
	}

	/* Drawn above its place. */
	if (*height < 0.0f) {
		*y += *height;
		*height = -*height;
	}
}

/* Grows the box of the draws by a draw's rectangle (left, top, right, bottom), within the scissor. */
static void
bounds_add(
	struct kwl_compose *compose,
	float left,
	float top,
	float right,
	float bottom)
{
	const VkRect2D *scissor;

	/* The rectangle within the scissor; nothing of it is drawn outside. */
	scissor = &compose->scissor_now;
	if (left < (float)scissor->offset.x)
		left = (float)scissor->offset.x;
	if (top < (float)scissor->offset.y)
		top = (float)scissor->offset.y;
	if (right > (float)scissor->offset.x + (float)scissor->extent.width)
		right = (float)scissor->offset.x + (float)scissor->extent.width;
	if (bottom > (float)scissor->offset.y + (float)scissor->extent.height)
		bottom = (float)scissor->offset.y + (float)scissor->extent.height;
	if (right <= left || bottom <= top)
		return;

	/* The first rectangle is the box; later ones grow it. */
	if (!compose->bounded) {
		compose->bounds[0] = left;
		compose->bounds[1] = top;
		compose->bounds[2] = right;
		compose->bounds[3] = bottom;
		compose->bounded = 1U;
		return;
	}
	if (left < compose->bounds[0])
		compose->bounds[0] = left;
	if (top < compose->bounds[1])
		compose->bounds[1] = top;
	if (right > compose->bounds[2])
		compose->bounds[2] = right;
	if (bottom > compose->bounds[3])
		compose->bounds[3] = bottom;
}
