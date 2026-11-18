/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry of a Notes frame: rectangles, stroke outlines and the
 * toolbar's picture as triangles, and the draw calls that show them.
 *
 * A stroke's outline is any closed polygon, which may cross itself, filled
 * by the nonzero rule as the PDF fills it.  It is drawn in three calls
 * through the stencil buffer (render.c):
 *
 *  1. a fan of triangles from the first corner counts the winding of every
 *     pixel into the stencil (front faces up, back faces down) without
 *     drawing colour;
 *  2. a fringe, a band one pixel wide on each side of every edge, is drawn
 *     where the stencil is still zero (just outside the polygon) with an
 *     alpha that falls from the edge outwards: the anti-aliasing, without
 *     multisampling;
 *  3. the polygon's bounding box is drawn where the stencil is not zero
 *     (inside), which sets those stencil values back to zero.
 *
 * Each pixel inside is drawn once, so a translucent highlighter does not
 * darken where it crosses itself.
 *
 * ws216-p004: a stroke is drawn as its shapes (shape.c), each such a
 * polygon: a fill's contours each add their fan to the one winding, a
 * pencil's ranges leave out the fringe where they meet (their seams), and
 * a shape's paint and blend choose pipelines that draw it through the
 * paper's grain or multiply it into the page.
 */

#include "app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The fewest vertices and draws a frame's arrays grow to. */
#define GEOMETRY_VERTICES_MIN	4096U
#define GEOMETRY_DRAWS_MIN	256U

/* The half width of the anti-aliasing fringe, in pixels. */
#define GEOMETRY_FRINGE		1.0f

/* The most texels of the grain a pixel may cover before a coarser size is drawn (ws216-p004). */
#define GEOMETRY_GRAIN_TEXELS	1.5

/*
 * One filled polygon as frame_fill() draws it: its corners in page points
 * (in contours, or one contour of every corner), the view that places
 * them, the edges without a fringe, the colour and the v every vertex
 * carries, the pipelines of its fringe and cover, and whether those draws
 * go through the grain (and so need the page's place).
 */
struct frame_fill {
	const struct pdf_point *points;
	size_t count;
	const size_t *contours;
	size_t contour_count;
	const struct notes_view *view;
	size_t seam_start;
	size_t seam_end;
	float colors[4];
	float v;
	unsigned fringe_pipe;
	unsigned cover_pipe;
	int grain;
};

static void frame_fill(struct notes_frame *frame, const struct frame_fill *fill);
static size_t frame_contour(const struct frame_fill *fill, size_t contour);
static void frame_place(struct notes_frame *frame, const struct frame_fill *fill);
static float *frame_reserve(struct notes_frame *frame, size_t count);
static void frame_draw(struct notes_frame *frame, unsigned pipe, size_t first);
static float *frame_vertex(float *vertex, float x, float y, float u, float v, const float *color);
static void color_floats(uint32_t color, float *floats);

/*
 * Empties a frame for the next one, keeping its arrays.
 */
void
notes_frame_begin(
	struct notes_frame *frame)
{
	/* No vertices, no draws, no clipping, no failure. */
	frame->vertex_count = 0;
	frame->draw_count = 0;
	frame->clipped = 0;
	frame->error = 0;
}

/*
 * Frees a frame's arrays.
 */
void
notes_frame_free(
	struct notes_frame *frame)
{
	/* The arrays, and the frame is empty. */
	free(frame->vertices);
	free(frame->draws);
	memset(frame, 0, sizeof(*frame));
}

/*
 * Adds a filled rectangle in pixels, in a colour (0xRRGGBBAA).
 */
void
notes_frame_rect(
	struct notes_frame *frame,
	float x,
	float y,
	float width,
	float height,
	uint32_t color)
{
	float colors[4];
	float *vertex;
	size_t first;

	/* Room for two triangles. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, 6U);
	if (vertex == NULL)
		return;

	/* The two triangles, with no fringe distance. */
	color_floats(color, colors);
	vertex = frame_vertex(vertex, x, y, 0.0f, 0.0f, colors);
	vertex = frame_vertex(vertex, x + width, y, 0.0f, 0.0f, colors);
	vertex = frame_vertex(vertex, x, y + height, 0.0f, 0.0f, colors);
	vertex = frame_vertex(vertex, x + width, y, 0.0f, 0.0f, colors);
	vertex = frame_vertex(vertex, x + width, y + height, 0.0f, 0.0f, colors);
	(void)frame_vertex(vertex, x, y + height, 0.0f, 0.0f, colors);

	/* One plain draw. */
	frame_draw(frame, NOTES_PIPE_PLAIN, first);
}

/*
 * Adds a rectangle in pixels whose colour (0xRRGGBBAA) runs from one at its
 * top to another at its bottom.
 */
void
notes_frame_gradient(
	struct notes_frame *frame,
	float x,
	float y,
	float width,
	float height,
	uint32_t top,
	uint32_t bottom)
{
	float top_colors[4];
	float bottom_colors[4];
	float *vertex;
	size_t first;

	/* Room for two triangles. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, 6U);
	if (vertex == NULL)
		return;

	/* The two triangles, the top corners in one colour and the bottom ones in the other. */
	color_floats(top, top_colors);
	color_floats(bottom, bottom_colors);
	vertex = frame_vertex(vertex, x, y, 0.0f, 0.0f, top_colors);
	vertex = frame_vertex(vertex, x + width, y, 0.0f, 0.0f, top_colors);
	vertex = frame_vertex(vertex, x, y + height, 0.0f, 0.0f, bottom_colors);
	vertex = frame_vertex(vertex, x + width, y, 0.0f, 0.0f, top_colors);
	vertex = frame_vertex(vertex, x + width, y + height, 0.0f, 0.0f, bottom_colors);
	(void)frame_vertex(vertex, x, y + height, 0.0f, 0.0f, bottom_colors);

	/* One plain draw. */
	frame_draw(frame, NOTES_PIPE_PLAIN, first);
}

/*
 * Turns clipping to a rectangle in pixels on or off for the draws that follow.
 */
void
notes_frame_clip(
	struct notes_frame *frame,
	int enabled,
	float x,
	float y,
	float width,
	float height)
{
	/* The rectangle, rounded outwards to whole pixels. */
	frame->clipped = enabled;
	frame->clip[0] = (int32_t)floor((double)x);
	frame->clip[1] = (int32_t)floor((double)y);
	frame->clip[2] = (int32_t)ceil((double)(x + width)) - frame->clip[0];
	frame->clip[3] = (int32_t)ceil((double)(y + height)) - frame->clip[1];
}

/*
 * Adds a polygon in page points, filled by the nonzero rule and
 * anti-aliased, in a colour (0xRRGGBBAA), placed by a view.
 */
void
notes_frame_polygon(
	struct notes_frame *frame,
	const struct pdf_point *points,
	size_t count,
	const struct notes_view *view,
	uint32_t color)
{
	struct frame_fill fill;

	/* One contour without seams, solid. */
	memset(&fill, 0, sizeof(fill));
	fill.points = points;
	fill.count = count;
	fill.view = view;
	color_floats(color, fill.colors);
	fill.fringe_pipe = NOTES_PIPE_FRINGE;
	fill.cover_pipe = NOTES_PIPE_COVER;
	fill.seam_start = PDF_OUTLINE_NO_SEAM;
	fill.seam_end = PDF_OUTLINE_NO_SEAM;

	/* The three draws. */
	frame_fill(frame, &fill);
}

/*
 * Adds one shape of a stroke (ws216-p004), in the stroke's colour
 * (0xRRGGBBAA), placed by a view.
 *
 * Its opacity is the colour's times the shape's.  Every contour counts
 * into the one winding, the edges where a pencil's range meets its
 * neighbour get no fringe, and the shape's paint and blend choose the
 * pipelines.  A pencil's vertices carry two opacities: the flat one a
 * device without the grain draws (in the colour) and the one the grain
 * starts from (in v); a watercolour's carry its colour and opacity, which
 * the Multiply and its stand-in, source over, both take.
 */
void
notes_frame_shape(
	struct notes_frame *frame,
	const struct notes_shape *shape,
	const struct notes_view *view,
	uint32_t color)
{
	struct frame_fill fill;
	float alpha;

	/* The shape's corners, contours and seams. */
	memset(&fill, 0, sizeof(fill));
	fill.points = shape->polygon;
	fill.count = shape->corner_count;
	fill.contours = shape->contours;
	fill.contour_count = shape->contour_count;
	fill.view = view;
	fill.seam_start = shape->seam_start;
	fill.seam_end = shape->seam_end;

	/* The colour at the stroke's opacity times the shape's. */
	color_floats(color, fill.colors);
	alpha = fill.colors[3] * shape->alpha;
	fill.colors[3] = alpha;

	/* The pipelines and what the vertices carry for them. */
	if (shape->paint == NOTES_PAINT_GRAIN) {
		/* Through the grain: the flat opacity of the grain's mean as the colour's, the grain's start in v. */
		fill.colors[3] = 1.0f - (1.0f - alpha * NOTES_GRAIN_FLOOR) * (1.0f - alpha * NOTES_GRAIN_MEAN);
		fill.v = alpha;
		fill.fringe_pipe = NOTES_PIPE_FRINGE_GRAIN;
		fill.cover_pipe = NOTES_PIPE_COVER_GRAIN;
		fill.grain = 1;
	} else if (shape->blend == NOTES_BLEND_MULTIPLY) {
		/* Multiplied into what is under it. */
		fill.fringe_pipe = NOTES_PIPE_FRINGE_MULTIPLY;
		fill.cover_pipe = NOTES_PIPE_COVER_MULTIPLY;
	} else {
		/* Laid over what is under it, as the pen is. */
		fill.fringe_pipe = NOTES_PIPE_FRINGE;
		fill.cover_pipe = NOTES_PIPE_COVER;
	}

	/* The three draws. */
	frame_fill(frame, &fill);
}

/*
 * Adds a stroke: each of its shapes in order (ws216-p004), placed by a
 * view.  The stroke's shapes must be made (notes_stroke_outline()).
 */
void
notes_frame_stroke(
	struct notes_frame *frame,
	const struct notes_stroke *stroke,
	const struct notes_view *view)
{
	size_t index;

	/* Each shape, bottom first. */
	for (index = 0; index < stroke->shape_count; index++)
		notes_frame_shape(frame, &stroke->shapes[index], view, stroke->color);
}

/*
 * Chooses the size of the grain to draw at a scale in pixels a point
 * (ws216-p004): the finest whose texels a pixel are at most
 * GEOMETRY_GRAIN_TEXELS, so that a smaller page does not alias the grain;
 * the coarsest when none is.  Returns 0 (the full tile) to
 * NOTES_GRAIN_LEVELS - 1.
 */
unsigned
notes_frame_grain_level(
	float scale)
{
	double texels;
	unsigned level;

	/* The finest level that is not too fine for the scale. */
	for (level = 0; level + 1U < NOTES_GRAIN_LEVELS; level++) {
		texels = (double)(NOTES_GRAIN_SIDE >> level) / (NOTES_GRAIN_CELL * (double)scale);
		if (texels <= GEOMETRY_GRAIN_TEXELS)
			break;
	}

	/* Reports the level (the coarsest when every finer one was too fine). */
	return level;
}

/*
 * Adds a picture (NOTES_TEXTURE_*) over a rectangle in pixels.
 */
void
notes_frame_texture(
	struct notes_frame *frame,
	unsigned texture,
	float x,
	float y,
	float width,
	float height)
{
	float white[4];
	float *vertex;
	size_t first;

	/* Room for two triangles. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, 6U);
	if (vertex == NULL)
		return;

	/* The two triangles, with the picture's corners, untinted. */
	white[0] = 1.0f;
	white[1] = 1.0f;
	white[2] = 1.0f;
	white[3] = 1.0f;
	vertex = frame_vertex(vertex, x, y, 0.0f, 0.0f, white);
	vertex = frame_vertex(vertex, x + width, y, 1.0f, 0.0f, white);
	vertex = frame_vertex(vertex, x, y + height, 0.0f, 1.0f, white);
	vertex = frame_vertex(vertex, x + width, y, 1.0f, 0.0f, white);
	vertex = frame_vertex(vertex, x + width, y + height, 1.0f, 1.0f, white);
	(void)frame_vertex(vertex, x, y + height, 0.0f, 1.0f, white);

	/* One textured draw, which names its picture. */
	frame_draw(frame, NOTES_PIPE_TEXTURE, first);
	if (frame->error == 0)
		frame->draws[frame->draw_count - 1U].texture = texture;
}

/*
 * Places a page in a window below the toolbar: as large as fits with a
 * margin, centred.
 */
void
notes_view_layout(
	struct notes_view *view,
	uint32_t width,
	uint32_t height,
	float page_width,
	float page_height)
{
	float room_width;
	float room_height;
	float scale;

	/* The room below the toolbar, inside the margin. */
	room_width = (float)width - 2.0f * NOTES_PAGE_MARGIN;
	room_height = (float)height - (float)NOTES_TOOLBAR_HEIGHT - 2.0f * NOTES_PAGE_MARGIN;
	if (room_width < 16.0f)
		room_width = 16.0f;
	if (room_height < 16.0f)
		room_height = 16.0f;

	/* The larger scale at which both sides fit. */
	scale = room_width / page_width;
	if (room_height / page_height < scale)
		scale = room_height / page_height;

	/* Centred in the room, on whole pixels. */
	view->scale = scale;
	view->x = (float)floor(((double)width - (double)(page_width * scale)) / 2.0);
	view->y = (float)floor((double)NOTES_TOOLBAR_HEIGHT + ((double)height - (double)NOTES_TOOLBAR_HEIGHT - (double)(page_height * scale)) / 2.0);
}

/*
 * Adds the three draws of a filled polygon: the fan of each contour, which
 * counts the winding into the stencil; the fringe across every edge but
 * the seams; and the box of every contour, drawn inside.
 */
static void
frame_fill(
	struct notes_frame *frame,
	const struct frame_fill *fill)
{
	const struct notes_view *view;
	float *vertex;
	float x0;
	float y0;
	float xa;
	float ya;
	float xb;
	float yb;
	float nx;
	float ny;
	float length;
	float box[4];
	size_t contour;
	size_t contour_count;
	size_t start;
	size_t corners;
	size_t fan;
	size_t edges;
	size_t first;
	size_t index;
	size_t next;

	/* A polygon needs three corners. */
	view = fill->view;
	if (fill->count < 3U)
		return;

	/* One contour of every corner, unless the polygon lists its contours. */
	contour_count = 1U;
	if (fill->contours != NULL)
		contour_count = fill->contour_count;

	/* The vertices of the fans: a triangle for each corner of a contour but two. */
	fan = 0;
	start = 0;
	for (contour = 0; contour < contour_count; contour++) {
		corners = frame_contour(fill, contour);
		start += corners;
		if (corners >= 3U)
			fan += (corners - 2U) * 3U;
	}

	/* Contours that do not list every corner once are not drawn (the loops below walk the corners by them). */
	if (start != fill->count)
		return;

	/* The edges with a fringe: one a corner, but the seams. */
	edges = 0;
	for (index = 0; index < fill->count; index++) {
		if (index == fill->seam_start || index == fill->seam_end)
			continue;
		edges++;
	}

	/* The box starts at the first corner. */
	box[0] = view->x + (float)fill->points[0].x * view->scale;
	box[1] = view->y + (float)fill->points[0].y * view->scale;
	box[2] = box[0];
	box[3] = box[1];

	/* The fan from each contour's first corner, which counts the winding into the stencil. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, fan);
	if (vertex == NULL)
		return;
	start = 0;
	for (contour = 0; contour < contour_count; contour++) {
		/* A contour of fewer than three corners encloses nothing. */
		corners = frame_contour(fill, contour);
		if (corners < 3U) {
			start += corners;
			continue;
		}

		/* Its triangles from its first corner. */
		x0 = view->x + (float)fill->points[start].x * view->scale;
		y0 = view->y + (float)fill->points[start].y * view->scale;
		for (index = start + 1U; index + 1U < start + corners; index++) {
			xa = view->x + (float)fill->points[index].x * view->scale;
			ya = view->y + (float)fill->points[index].y * view->scale;
			xb = view->x + (float)fill->points[index + 1U].x * view->scale;
			yb = view->y + (float)fill->points[index + 1U].y * view->scale;
			vertex = frame_vertex(vertex, x0, y0, 0.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xa, ya, 0.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xb, yb, 0.0f, fill->v, fill->colors);
		}

		/* The next contour starts after this one's corners. */
		start += corners;
	}

	/* The box grows to every corner. */
	for (index = 0; index < fill->count; index++) {
		xa = view->x + (float)fill->points[index].x * view->scale;
		ya = view->y + (float)fill->points[index].y * view->scale;

		/* Horizontally. */
		if (xa < box[0])
			box[0] = xa;
		if (xa > box[2])
			box[2] = xa;

		/* And vertically. */
		if (ya < box[1])
			box[1] = ya;
		if (ya > box[3])
			box[3] = ya;
	}

	/* One draw of every fan. */
	frame_draw(frame, NOTES_PIPE_STENCIL, first);

	/* The fringe: a band across every edge but the seams, its distance -1 on one side and +1 on the other. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, edges * 6U);
	if (vertex == NULL)
		return;
	start = 0;
	for (contour = 0; contour < contour_count; contour++) {
		corners = frame_contour(fill, contour);
		for (index = start; index < start + corners; index++) {
			/* A seam, where the next range of the stroke goes on, has no soft edge. */
			if (index == fill->seam_start || index == fill->seam_end)
				continue;

			/* The edge from this corner to the next (the contour's last closes it). */
			next = index + 1U;
			if (next == start + corners)
				next = start;
			xa = view->x + (float)fill->points[index].x * view->scale;
			ya = view->y + (float)fill->points[index].y * view->scale;
			xb = view->x + (float)fill->points[next].x * view->scale;
			yb = view->y + (float)fill->points[next].y * view->scale;

			/* The edge's unit normal, scaled to the fringe; a degenerate edge gives no band. */
			nx = ya - yb;
			ny = xb - xa;
			length = (float)sqrt((double)(nx * nx + ny * ny));
			if (length < 1e-4f) {
				nx = 0.0f;
				ny = 0.0f;
			} else {
				nx = nx / length * GEOMETRY_FRINGE;
				ny = ny / length * GEOMETRY_FRINGE;
			}

			/* The band's two triangles. */
			vertex = frame_vertex(vertex, xa + nx, ya + ny, 1.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xa - nx, ya - ny, -1.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xb + nx, yb + ny, 1.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xa - nx, ya - ny, -1.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xb - nx, yb - ny, -1.0f, fill->v, fill->colors);
			vertex = frame_vertex(vertex, xb + nx, yb + ny, 1.0f, fill->v, fill->colors);
		}

		/* The next contour starts after this one's corners. */
		start += corners;
	}

	/* One draw of the whole fringe. */
	frame_draw(frame, fill->fringe_pipe, first);
	frame_place(frame, fill);

	/* The cover: the box, drawn inside and clearing the stencil. */
	first = frame->vertex_count;
	vertex = frame_reserve(frame, 6U);
	if (vertex == NULL)
		return;
	box[0] -= 1.0f;
	box[1] -= 1.0f;
	box[2] += 1.0f;
	box[3] += 1.0f;
	vertex = frame_vertex(vertex, box[0], box[1], 0.0f, fill->v, fill->colors);
	vertex = frame_vertex(vertex, box[2], box[1], 0.0f, fill->v, fill->colors);
	vertex = frame_vertex(vertex, box[0], box[3], 0.0f, fill->v, fill->colors);
	vertex = frame_vertex(vertex, box[2], box[1], 0.0f, fill->v, fill->colors);
	vertex = frame_vertex(vertex, box[2], box[3], 0.0f, fill->v, fill->colors);
	(void)frame_vertex(vertex, box[0], box[3], 0.0f, fill->v, fill->colors);
	frame_draw(frame, fill->cover_pipe, first);
	frame_place(frame, fill);
}

/* Reports the corner count of a polygon's contour (every corner for a polygon of one). */
static size_t
frame_contour(
	const struct frame_fill *fill,
	size_t contour)
{
	/* A polygon of one contour. */
	if (fill->contours == NULL)
		return fill->count;

	/* Reports the listed count. */
	return fill->contours[contour];
}

/* Gives the draw added last the page's place, when it is drawn through the grain. */
static void
frame_place(
	struct notes_frame *frame,
	const struct frame_fill *fill)
{
	struct notes_draw *draw;

	/* Only a grain's draw on a frame that has not failed needs it. */
	if (!fill->grain || frame->error != 0)
		return;

	/* The page's top left in the target's pixels and its pixels a point. */
	draw = &frame->draws[frame->draw_count - 1U];
	draw->place[0] = fill->view->x;
	draw->place[1] = fill->view->y;
	draw->place[2] = fill->view->scale;
}

/* Makes room for vertices and returns where they go, or NULL once the frame has failed. */
static float *
frame_reserve(
	struct notes_frame *frame,
	size_t count)
{
	float *larger;
	size_t capacity;
	float *vertex;

	/* A failed frame takes nothing more. */
	if (frame->error != 0)
		return NULL;

	/* Grows the array, doubling from a small start, when the vertices do not fit. */
	if (frame->vertex_count + count > frame->vertex_capacity) {
		capacity = frame->vertex_capacity;
		if (capacity < GEOMETRY_VERTICES_MIN)
			capacity = GEOMETRY_VERTICES_MIN;
		while (capacity < frame->vertex_count + count)
			capacity *= 2U;
		larger = realloc(frame->vertices, capacity * NOTES_VERTEX_FLOATS * sizeof(float));
		if (larger == NULL) {
			frame->error = 1;
			return NULL;
		}

		/* The larger array. */
		frame->vertices = larger;
		frame->vertex_capacity = capacity;
	}

	/* Succeeded: the vertices go after the ones already there. */
	vertex = frame->vertices + frame->vertex_count * NOTES_VERTEX_FLOATS;
	frame->vertex_count += count;
	return vertex;
}

/* Adds a draw of the vertices from first to the end, with the current clipping. */
static void
frame_draw(
	struct notes_frame *frame,
	unsigned pipe,
	size_t first)
{
	struct notes_draw *larger;
	size_t capacity;

	/* Grows the array when it is full. */
	if (frame->draw_count == frame->draw_capacity) {
		capacity = frame->draw_capacity * 2U;
		if (capacity < GEOMETRY_DRAWS_MIN)
			capacity = GEOMETRY_DRAWS_MIN;
		larger = realloc(frame->draws, capacity * sizeof(*larger));
		if (larger == NULL) {
			frame->error = 1;
			return;
		}

		/* The larger array. */
		frame->draws = larger;
		frame->draw_capacity = capacity;
	}

	/* Succeeded: the draw is the frame's last. */
	frame->draws[frame->draw_count].pipe = pipe;
	frame->draws[frame->draw_count].texture = NOTES_TEXTURE_TOOLBAR;
	frame->draws[frame->draw_count].first = (uint32_t)first;
	frame->draws[frame->draw_count].count = (uint32_t)(frame->vertex_count - first);
	frame->draws[frame->draw_count].clipped = frame->clipped;
	memcpy(frame->draws[frame->draw_count].clip, frame->clip, sizeof(frame->clip));
	frame->draws[frame->draw_count].place[0] = 0.0f;
	frame->draws[frame->draw_count].place[1] = 0.0f;
	frame->draws[frame->draw_count].place[2] = 0.0f;
	frame->draw_count++;
}

/* Writes one vertex and returns where the next goes. */
static float *
frame_vertex(
	float *vertex,
	float x,
	float y,
	float u,
	float v,
	const float *color)
{
	/* The position, the extra pair, the colour. */
	vertex[0] = x;
	vertex[1] = y;
	vertex[2] = u;
	vertex[3] = v;
	vertex[4] = color[0];
	vertex[5] = color[1];
	vertex[6] = color[2];
	vertex[7] = color[3];

	/* Reports the next vertex's place. */
	return vertex + NOTES_VERTEX_FLOATS;
}

/* Converts a colour 0xRRGGBBAA to four floats from 0 to 1. */
static void
color_floats(
	uint32_t color,
	float *floats)
{
	/* Each channel. */
	floats[0] = (float)((color >> 24) & 0xffU) / 255.0f;
	floats[1] = (float)((color >> 16) & 0xffU) / 255.0f;
	floats[2] = (float)((color >> 8) & 0xffU) / 255.0f;
	floats[3] = (float)(color & 0xffU) / 255.0f;
}
