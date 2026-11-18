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
 */

#include "compose.h"

static int clip_active(const struct kwl_compose *compose);
static int clip_cut(const VkRect2D *clip, const VkRect2D *scissor, VkRect2D *cut);
static int clip_meets(const VkRect2D *rect, float x, float y, float width, float height);
static int clip_same(const VkRect2D *first, const VkRect2D *second);
static void clip_box(float *x, float *y, float *width, float *height);
static void bounds_add(struct kwl_compose *compose, float left, float top, float right, float bottom);

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
