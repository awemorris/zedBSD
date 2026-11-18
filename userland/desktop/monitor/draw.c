/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's drawing primitives (design.md sections 2.4 and
 * 4.2): colours and their tokens, and the shapes and text a frame's scene
 * is made of -- solid rectangles and triangles, gradients, plates (rounded
 * rectangles with an edge), discs, lines and glyphs from the atlas.  Every
 * shape is a few vertices in the scene's list; the shaders do the rest.
 */

#include "draw.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The layers' surfaces and edges, from the back (0) to the front (2). */
static const uint32_t layer_surface[3] = { TOKEN_SURFACE_0, TOKEN_SURFACE_1, TOKEN_SURFACE_2 };
static const uint32_t layer_edge[3] = { TOKEN_EDGE_0, TOKEN_EDGE_1, TOKEN_EDGE_2 };

static void draw_vertex(struct sm_scene *scene, float x, float y, float lx, float ly, const float *shape, struct sm_color color, struct sm_color second);
static void draw_use(struct sm_scene *scene, unsigned pipe, size_t first);

/*
 * A colour token (0xRRGGBB) with an alpha, as floats.
 */
struct sm_color
sm_rgb(
	uint32_t rgb,
	float alpha)
{
	struct sm_color color;

	/* The three channels and the alpha. */
	color.r = (float)((rgb >> 16) & 0xffU) / 255.0f;
	color.g = (float)((rgb >> 8) & 0xffU) / 255.0f;
	color.b = (float)(rgb & 0xffU) / 255.0f;
	color.a = alpha;

	/* Succeeded: the colour. */
	return color;
}

/*
 * A colour between two (0: the first, 1: the second).
 */
struct sm_color
sm_color_mix(
	struct sm_color from,
	struct sm_color to,
	float amount)
{
	struct sm_color color;

	/* Each channel. */
	color.r = from.r + (to.r - from.r) * amount;
	color.g = from.g + (to.g - from.g) * amount;
	color.b = from.b + (to.b - from.b) * amount;
	color.a = from.a + (to.a - from.a) * amount;

	/* Succeeded: the mixed colour. */
	return color;
}

/*
 * Makes room for more vertices and draws; returns 0 or ENOMEM.
 */
int
sm_scene_reserve(
	struct sm_scene *scene,
	size_t vertices)
{
	float *more_vertices;
	struct sm_draw *more_draws;
	size_t capacity;

	/* The vertices, doubled until they fit. */
	if (scene->vertex_count + vertices > scene->vertex_capacity) {
		capacity = scene->vertex_capacity;
		if (capacity == 0U)
			capacity = 65536U;
		while (capacity < scene->vertex_count + vertices)
			capacity *= 2U;
		more_vertices = realloc(scene->vertices, capacity * SM_VERTEX_FLOATS * sizeof(float));
		if (more_vertices == NULL)
			return ENOMEM;
		scene->vertices = more_vertices;
		scene->vertex_capacity = capacity;
	}

	/* The draws: room for a few more always. */
	if (scene->draw_count + 4U > scene->draw_capacity) {
		capacity = scene->draw_capacity * 2U;
		if (capacity == 0U)
			capacity = 256U;
		more_draws = realloc(scene->draws, capacity * sizeof(scene->draws[0]));
		if (more_draws == NULL)
			return ENOMEM;
		scene->draws = more_draws;
		scene->draw_capacity = capacity;
	}

	/* Succeeded: the room is there. */
	return 0;
}

/*
 * Adds a quad of four corners (x, y pairs, in order around it) with their local points, as two triangles.
 */
void
sm_draw_quad(
	struct sm_scene *scene,
	unsigned pipe,
	const float *corners,
	const float *locals,
	const float *shape,
	struct sm_color color,
	struct sm_color second)
{
	static const unsigned order[6] = { 0U, 1U, 2U, 0U, 2U, 3U };
	size_t first;
	unsigned index;
	unsigned corner;
	int error;

	/* Room, or nothing drawn. */
	error = sm_scene_reserve(scene, 6U);
	if (error != 0)
		return;

	/* The two triangles. */
	first = scene->vertex_count;
	for (index = 0; index < 6U; index++) {
		corner = order[index];
		draw_vertex(scene, corners[corner * 2U], corners[corner * 2U + 1U], locals[corner * 2U], locals[corner * 2U + 1U],
			     shape, color, second);
	}

	/* In the draw of the pipeline. */
	draw_use(scene, pipe, first);
}

/*
 * A solid rectangle.
 */
void
sm_draw_rect(
	struct sm_scene *scene,
	float x,
	float y,
	float width,
	float height,
	struct sm_color color)
{
	float corners[8];
	float locals[8];
	float shape[4];

	/* The corners; the local points do not matter to a solid fill. */
	corners[0] = x;
	corners[1] = y;
	corners[2] = x + width;
	corners[3] = y;
	corners[4] = x + width;
	corners[5] = y + height;
	corners[6] = x;
	corners[7] = y + height;
	memset(locals, 0, sizeof(locals));
	shape[0] = width * 0.5f;
	shape[1] = height * 0.5f;
	shape[2] = 0.0f;
	shape[3] = SM_SHAPE_SOLID;
	sm_draw_quad(scene, SM_PIPE_SHAPE, corners, locals, shape, color, color);
}

/*
 * A rectangle shaded from one colour at the top to another at the bottom.
 */
void
sm_draw_gradient(
	struct sm_scene *scene,
	float x,
	float y,
	float width,
	float height,
	struct sm_color top,
	struct sm_color bottom)
{
	float corners[8];
	float locals[8];
	float shape[4];

	/* The corners, and their points from the centre. */
	corners[0] = x;
	corners[1] = y;
	corners[2] = x + width;
	corners[3] = y;
	corners[4] = x + width;
	corners[5] = y + height;
	corners[6] = x;
	corners[7] = y + height;
	locals[0] = -width * 0.5f;
	locals[1] = -height * 0.5f;
	locals[2] = width * 0.5f;
	locals[3] = -height * 0.5f;
	locals[4] = width * 0.5f;
	locals[5] = height * 0.5f;
	locals[6] = -width * 0.5f;
	locals[7] = height * 0.5f;
	shape[0] = width * 0.5f;
	shape[1] = height * 0.5f;
	shape[2] = 0.0f;
	shape[3] = SM_SHAPE_GRADIENT;
	sm_draw_quad(scene, SM_PIPE_SHAPE, corners, locals, shape, top, bottom);
}

/*
 * A rounded rectangle with an edge: the shader's plate (a pixel larger for the edge's smoothing).
 */
void
sm_draw_round(
	struct sm_scene *scene,
	float x,
	float y,
	float width,
	float height,
	float radius,
	struct sm_color fill,
	struct sm_color edge)
{
	float corners[8];
	float locals[8];
	float shape[4];
	float half_width;
	float half_height;

	/* The quad a pixel beyond the shape, and its points from the centre. */
	half_width = width * 0.5f;
	half_height = height * 0.5f;
	corners[0] = x - 1.0f;
	corners[1] = y - 1.0f;
	corners[2] = x + width + 1.0f;
	corners[3] = y - 1.0f;
	corners[4] = x + width + 1.0f;
	corners[5] = y + height + 1.0f;
	corners[6] = x - 1.0f;
	corners[7] = y + height + 1.0f;
	locals[0] = -half_width - 1.0f;
	locals[1] = -half_height - 1.0f;
	locals[2] = half_width + 1.0f;
	locals[3] = -half_height - 1.0f;
	locals[4] = half_width + 1.0f;
	locals[5] = half_height + 1.0f;
	locals[6] = -half_width - 1.0f;
	locals[7] = half_height + 1.0f;
	shape[0] = half_width;
	shape[1] = half_height;
	shape[2] = radius;
	shape[3] = SM_SHAPE_PLATE;
	sm_draw_quad(scene, SM_PIPE_SHAPE, corners, locals, shape, fill, edge);
}

/*
 * A plate of a layer: its surface and edge, its corner radius, and its shadow-free depth by tone.
 */
void
sm_draw_plate(
	struct sm_scene *scene,
	const struct sm_box *box,
	unsigned layer,
	float radius)
{
	/* The plate in its layer's colours. */
	sm_draw_round(scene, box->x, box->y, box->width, box->height, radius, sm_rgb(layer_surface[layer], 1.0f), sm_rgb(layer_edge[layer], 1.0f));
}

/*
 * A disc.
 */
void
sm_draw_disc(
	struct sm_scene *scene,
	float cx,
	float cy,
	float radius,
	struct sm_color color)
{
	float corners[8];
	float locals[8];
	float shape[4];
	float reach;

	/* A square a pixel beyond the disc. */
	reach = radius + 1.0f;
	corners[0] = cx - reach;
	corners[1] = cy - reach;
	corners[2] = cx + reach;
	corners[3] = cy - reach;
	corners[4] = cx + reach;
	corners[5] = cy + reach;
	corners[6] = cx - reach;
	corners[7] = cy + reach;
	locals[0] = -reach;
	locals[1] = -reach;
	locals[2] = reach;
	locals[3] = -reach;
	locals[4] = reach;
	locals[5] = reach;
	locals[6] = -reach;
	locals[7] = reach;
	shape[0] = radius;
	shape[1] = radius;
	shape[2] = 0.0f;
	shape[3] = SM_SHAPE_DISC;
	sm_draw_quad(scene, SM_PIPE_SHAPE, corners, locals, shape, color, color);
}

/*
 * Draws a string in a style from the atlas at a baseline: from x (align
 * 0), centred on x (1) or ending at x (2).  Reports its width.
 */
float
sm_draw_text(
	struct sm_app *app,
	enum sm_style style,
	float x,
	float baseline,
	const char *text,
	struct sm_color color,
	int align)
{
	const struct sm_atlas *atlas;
	const struct sm_glyph *glyph;
	float corners[8];
	float locals[8];
	float shape[4];
	float width;
	float pen;
	float top;
	float u0;
	float v0;
	float u1;
	float v1;
	unsigned character;

	/* Nothing without the atlas. */
	atlas = &app->atlas;
	if (atlas->pixels == NULL)
		return 0.0f;

	/* Where the pen starts. */
	width = sm_atlas_width(atlas, style, text);
	pen = x;
	if (align == 1)
		pen = x - width * 0.5f;
	else if (align == 2)
		pen = x - width;
	pen = floorf(pen + 0.5f);
	top = floorf(baseline + 0.5f) - (float)atlas->ascent[style] - 2.0f;
	memset(shape, 0, sizeof(shape));

	/* One quad a character: its cell, sampled from the atlas. */
	for (; *text != '\0'; text++) {
		character = (unsigned char)*text;
		if (character < SM_GLYPH_FIRST || character > SM_GLYPH_LAST)
			character = ' ';
		glyph = &atlas->glyphs[style][character - SM_GLYPH_FIRST];
		if (character != ' ') {
			/* The cell's corners and its place in the atlas. */
			corners[0] = pen - 2.0f;
			corners[1] = top;
			corners[2] = pen - 2.0f + (float)glyph->width;
			corners[3] = top;
			corners[4] = corners[2];
			corners[5] = top + (float)glyph->height;
			corners[6] = corners[0];
			corners[7] = corners[5];
			u0 = (float)glyph->x / (float)atlas->width;
			v0 = (float)glyph->y / (float)atlas->height;
			u1 = (float)(glyph->x + glyph->width) / (float)atlas->width;
			v1 = (float)(glyph->y + glyph->height) / (float)atlas->height;
			locals[0] = u0;
			locals[1] = v0;
			locals[2] = u1;
			locals[3] = v0;
			locals[4] = u1;
			locals[5] = v1;
			locals[6] = u0;
			locals[7] = v1;
			sm_draw_quad(&app->scene, SM_PIPE_GLYPH, corners, locals, shape, color, color);
		}

		/* The pen moves past it. */
		pen += (float)glyph->advance;
	}

	/* Succeeded: the width drawn. */
	return width;
}

/*
 * A line from one point to another, of a thickness, smoothed at its sides.
 */
void
sm_draw_segment(
	struct sm_scene *scene,
	float x0,
	float y0,
	float x1,
	float y1,
	float thickness,
	struct sm_color color)
{
	float corners[8];
	float locals[8];
	float shape[4];
	float dx;
	float dy;
	float length;
	float nx;
	float ny;
	float reach;

	/* The direction and its normal; a point is nothing. */
	dx = x1 - x0;
	dy = y1 - y0;
	length = sqrtf(dx * dx + dy * dy);
	if (length < 0.001f)
		return;
	nx = -dy / length;
	ny = dx / length;

	/* The quad half the thickness and a pixel to each side. */
	reach = thickness * 0.5f + 1.0f;
	corners[0] = x0 + nx * reach;
	corners[1] = y0 + ny * reach;
	corners[2] = x1 + nx * reach;
	corners[3] = y1 + ny * reach;
	corners[4] = x1 - nx * reach;
	corners[5] = y1 - ny * reach;
	corners[6] = x0 - nx * reach;
	corners[7] = y0 - ny * reach;
	locals[0] = 0.0f;
	locals[1] = reach;
	locals[2] = 0.0f;
	locals[3] = reach;
	locals[4] = 0.0f;
	locals[5] = -reach;
	locals[6] = 0.0f;
	locals[7] = -reach;
	shape[0] = length * 0.5f;
	shape[1] = thickness * 0.5f + 0.5f;
	shape[2] = 0.0f;
	shape[3] = SM_SHAPE_LINE;
	sm_draw_quad(scene, SM_PIPE_SHAPE, corners, locals, shape, color, color);
}

/*
 * The colour of a level (design.md section 3.2).
 */
struct sm_color
sm_level_color(
	enum sm_level level)
{
	/* Cyan, ice, amber and the muted coral. */
	switch (level) {
	case SM_LEVEL_ELEVATED:
		return sm_rgb(TOKEN_ICE, 1.0f);
	case SM_LEVEL_WARNING:
		return sm_rgb(TOKEN_AMBER, 1.0f);
	case SM_LEVEL_CRITICAL:
		return sm_rgb(TOKEN_CORAL, 1.0f);
	default:
		return sm_rgb(TOKEN_MINT, 1.0f);
	}
}

/*
 * Adds a solid triangle (three x, y pairs), for the faces of the 3D parts.
 */
void
sm_draw_triangle(
	struct sm_scene *scene,
	const float *corners,
	struct sm_color color)
{
	static const float shape[4] = { 0.0f, 0.0f, 0.0f, SM_SHAPE_SOLID };
	size_t first;
	unsigned index;
	int error;

	/* Room, or nothing drawn. */
	error = sm_scene_reserve(scene, 3U);
	if (error != 0)
		return;

	/* The three corners. */
	first = scene->vertex_count;
	for (index = 0; index < 3U; index++)
		draw_vertex(scene, corners[index * 2U], corners[index * 2U + 1U], 0.0f, 0.0f, shape, color, color);

	/* In the draw of the shapes. */
	draw_use(scene, SM_PIPE_SHAPE, first);
}

/* Adds one vertex (the caller made room). */
static void
draw_vertex(
	struct sm_scene *scene,
	float x,
	float y,
	float lx,
	float ly,
	const float *shape,
	struct sm_color color,
	struct sm_color second)
{
	float *vertex;

	/* The four vec4s. */
	vertex = scene->vertices + scene->vertex_count * SM_VERTEX_FLOATS;
	vertex[0] = x;
	vertex[1] = y;
	vertex[2] = lx;
	vertex[3] = ly;
	vertex[4] = shape[0];
	vertex[5] = shape[1];
	vertex[6] = shape[2];
	vertex[7] = shape[3];
	vertex[8] = color.r;
	vertex[9] = color.g;
	vertex[10] = color.b;
	vertex[11] = color.a;
	vertex[12] = second.r;
	vertex[13] = second.g;
	vertex[14] = second.b;
	vertex[15] = second.a;
	scene->vertex_count++;
}

/* Counts the vertices from first in a draw of a pipeline: the last draw grows, or a new one starts. */
static void
draw_use(
	struct sm_scene *scene,
	unsigned pipe,
	size_t first)
{
	struct sm_draw *draw;
	size_t count;

	/* The vertices just added. */
	count = scene->vertex_count - first;

	/* The last draw of the same pipeline takes them. */
	if (scene->draw_count != 0U) {
		draw = &scene->draws[scene->draw_count - 1U];
		if (draw->pipe == pipe && (size_t)draw->first + draw->count == first) {
			draw->count += (uint32_t)count;
			return;
		}
	}

	/* A new draw. */
	draw = &scene->draws[scene->draw_count++];
	draw->pipe = pipe;
	draw->first = (uint32_t)first;
	draw->count = (uint32_t)count;
}
