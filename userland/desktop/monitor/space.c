/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's 3D parts (design.md sections 3.1 to 3.3): the
 * state's core at the centre and the CPU cores' relief, and the motion
 * that drives them.
 *
 * The geometry is small (three nested boxes, two rings, a few orbits, a
 * box a CPU) and is projected on the CPU into the scene's 2D triangles,
 * shaded per face: the shaders stay the plain shape shader, which every
 * driver takes (i915's native compiler takes no gl_VertexIndex, and the
 * frame needs no depth buffer).  The translucent shells are drawn in a
 * fixed order instead of sorting: the outer shell's back faces, the
 * middle's, the inner (nearly opaque), the middle's front faces, the
 * outer's (design.md section 4.2).
 *
 * Nothing turns without a reason: the rings turn with the disks' work,
 * the orbits' points move with the network's, the middle shell breathes
 * with the GPU's use; the camera follows the pointer a little.
 */

#include "draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pi, twice. */
#define SPACE_TAU		6.283185307f

/* The longest step a spring is moved by at once, in seconds (shorter steps keep it stable at any frame rate). */
#define SPACE_SPRING_STEP	(1.0f / 120.0f)

/* The core's shells, and the points of a ring. */
#define SPACE_SHELLS		3U
#define SPACE_RING_POINTS	72U
#define SPACE_ORBITS		3U

/* The most memory points in the core. */
#define SPACE_POINTS		64U

/* A camera: where the origin falls on the screen, pixels a unit, the turn and the tilt, and the eye's distance. */
struct space_camera {
	float cx;
	float cy;
	float scale;
	float cos_azimuth;
	float sin_azimuth;
	float cos_elevation;
	float sin_elevation;
	float depth;
};

/* A tile of the relief, for drawing them from the back. */
struct space_tile {
	float x;
	float z;
	float load;
	float depth;
};

/* A box's faces: their corners (bit 0 is +x, bit 1 +y, bit 2 +z) and their normals. */
static const unsigned space_faces[6][4] = {
	{ 0U, 2U, 6U, 4U },
	{ 1U, 5U, 7U, 3U },
	{ 0U, 4U, 5U, 1U },
	{ 2U, 3U, 7U, 6U },
	{ 0U, 1U, 3U, 2U },
	{ 4U, 6U, 7U, 5U }
};

static const float space_normals[6][3] = {
	{ -1.0f, 0.0f, 0.0f },
	{ 1.0f, 0.0f, 0.0f },
	{ 0.0f, -1.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.0f, -1.0f },
	{ 0.0f, 0.0f, 1.0f }
};

/* The light, from the upper left front (normalized). */
static const float space_light[3] = { -0.36f, 0.84f, 0.41f };

static void camera_set(struct space_camera *camera, float cx, float cy, float scale, float azimuth, float elevation);
static void camera_project(const struct space_camera *camera, const float *point, float *screen);
static float camera_facing(const struct space_camera *camera, const float *normal);
static void space_face(struct sm_app *app, const struct space_camera *camera, const float *center, const float *half, unsigned face,        struct sm_color color, float edge_alpha);
static void space_box(struct sm_app *app, const struct space_camera *camera, const float *center, const float *half, struct sm_color color,       struct sm_color top, float edge_alpha, int front);
static void space_ring(struct sm_app *app, const struct space_camera *camera, float y, float radius, float phase, float arc,        struct sm_color color, struct sm_color bright);
static void space_orbits(struct sm_app *app, const struct space_camera *camera, int front);
static void space_points(struct sm_app *app, const struct space_camera *camera, struct sm_color color);
static float shade(const float *normal);
static float corner_side(unsigned bits, unsigned bit, float extent);
static float follow(float value, float target, float seconds, float time_constant);
static float spring(float *value, float *speed, float target, float seconds);
static float rate_share(double rate, double floor);
static int tile_order(const void *left, const void *right);

/*
 * Advances the motion to a time: the camera's tilt towards the pointer,
 * the core's measures towards the values, and the phases of the rings,
 * the orbits and the flows.  With the clock stopped everything stands at
 * its target, so the picture is the same every time.
 */
void
sm_motion_update(
	struct sm_app *app,
	uint64_t now_ms)
{
	struct sm_motion *motion;
	const struct sm_frame *frame;
	float seconds;
	float stepped;
	uint64_t step;
	uint64_t whole;
	float memory;
	float rx;
	float tx;
	float read;
	float write;

	/*
	 * The time since the last frame, at most half a second: the animation
	 * draws twice a second, and a longer pause (a hidden window) does not
	 * jump.
	 */
	motion = &app->motion;
	frame = &app->frame;
	seconds = 0.0f;
	if (now_ms > motion->last_ms && motion->last_ms != 0U)
		seconds = (float)(now_ms - motion->last_ms) / 1000.0f;
	if (seconds > 0.5f)
		seconds = 0.5f;
	motion->last_ms = now_ms;

	/*
	 * The measures' and the phases' time: whole steps of the animation's
	 * frame time (half a second at 2 fps), at most half a second, so the
	 * hover's frames turn the camera without moving the core faster than
	 * the animation.
	 */
	if (motion->step_ms == 0U || now_ms < motion->step_ms)
		motion->step_ms = now_ms;
	whole = now_ms - motion->step_ms;
	if (app->fps != 0U) {
		step = 1000U / app->fps;
		whole -= whole % step;
	}
	motion->step_ms += whole;
	stepped = (float)whole / 1000.0f;
	if (stepped > 0.5f)
		stepped = 0.5f;

	/* The measures the core shows, from 0 to 1. */
	memory = 0.0f;
	if (app->source.info.memory_total != 0U)
		memory = (float)frame->memory_used / (float)app->source.info.memory_total;
	rx = rate_share(frame->rx_rate, 10000.0);
	tx = rate_share(frame->tx_rate, 10000.0);
	read = rate_share(frame->read_rate, 100000.0);
	write = rate_share(frame->write_rate, 100000.0);

	/* A stopped clock: everything at its target. */
	if (app->fixed_clock) {
		motion->tilt_x = motion->pointer_x;
		motion->tilt_y = motion->pointer_y;
		motion->cpu = (float)frame->cpu;
		motion->gpu = (float)frame->gpu_busy[0];
		motion->memory = memory;
		motion->network = fmaxf(rx, tx);
		motion->disk = fmaxf(read, write);
		motion->level = (float)app->level;
		return;
	}

	/* The camera follows the pointer on a spring, the measures on a short lag (by the animation's steps). */
	(void)spring(&motion->tilt_x, &motion->tilt_speed_x, motion->pointer_x, seconds);
	(void)spring(&motion->tilt_y, &motion->tilt_speed_y, motion->pointer_y, seconds);
	motion->cpu = follow(motion->cpu, (float)frame->cpu, stepped, 0.35f);
	motion->gpu = follow(motion->gpu, (float)frame->gpu_busy[0], stepped, 0.35f);
	motion->memory = follow(motion->memory, memory, stepped, 0.6f);
	motion->network = follow(motion->network, fmaxf(rx, tx), stepped, 0.35f);
	motion->disk = follow(motion->disk, fmaxf(read, write), stepped, 0.35f);
	motion->level = follow(motion->level, (float)app->level, stepped, 0.45f);

	/* The phases: the rings with the disks, the orbits and the flows with their rates. */
	motion->ring_phase += stepped * (0.01f + 0.15f * motion->disk);
	motion->orbit_phase += stepped * (0.03f + 0.4f * motion->network);
	motion->rx_phase += stepped * (0.05f + 0.6f * rx);
	motion->tx_phase += stepped * (0.05f + 0.6f * tx);
	motion->read_phase += stepped * (0.05f + 0.6f * read);
	motion->write_phase += stepped * (0.05f + 0.6f * write);
	motion->ring_phase -= floorf(motion->ring_phase);
	motion->orbit_phase -= floorf(motion->orbit_phase);
	motion->rx_phase -= floorf(motion->rx_phase);
	motion->tx_phase -= floorf(motion->tx_phase);
	motion->read_phase -= floorf(motion->read_phase);
	motion->write_phase -= floorf(motion->write_phase);
}

/*
 * Draws the state's core in a box: a soft glow, the rings below, the
 * orbits, the three shells with the memory's points between the inner and
 * the middle one (design.md section 3.2).
 */
void
sm_space_core(
	struct sm_app *app,
	const struct sm_box *box,
	uint64_t now_ms)
{
	const struct sm_motion *motion;
	struct space_camera camera;
	struct sm_color tone;
	struct sm_color outer;
	struct sm_color outer_top;
	struct sm_color middle;
	struct sm_color inner;
	struct sm_color ring;
	struct sm_color bright;
	float center[3];
	float half[3];
	float scale;
	float breath;
	float pulse;
	float size;
	float cx;
	float cy;
	unsigned glow;

	/*
	 * The camera over the box's middle: a little from above and from the
	 * left, following the pointer a few degrees, and turned by a drag on
	 * the core (ws134-p004; it comes back when let go).
	 */
	motion = &app->motion;
	scale = box->height * 0.145f;
	cx = box->x + box->width * 0.5f;
	cy = box->y + box->height * 0.44f;
	camera_set(&camera, cx, cy, scale, -0.52f + motion->tilt_x * 0.026f + app->touch.core_turn, 0.38f + motion->tilt_y * 0.017f);

	/* The colours: the level's tone through the shells, brighter inwards; the breath and the GPU's pulse. */
	tone = sm_level_color(app->level);
	breath = 0.5f + 0.5f * sinf((float)(now_ms % 4000U) / 4000.0f * SPACE_TAU);
	pulse = sinf((float)(now_ms % 3000U) / 3000.0f * SPACE_TAU);
	outer = sm_color_mix(sm_rgb(0x3a5a78U, 0.16f), tone, 0.12f);
	outer.a = 0.16f;
	outer_top = sm_color_mix(outer, sm_rgb(TOKEN_ICE, 1.0f), 0.2f + 0.5f * motion->cpu);
	outer_top.a = 0.2f + 0.22f * motion->cpu;
	middle = sm_color_mix(sm_rgb(0x46709aU, 1.0f), tone, 0.25f);
	middle.a = 0.24f;
	inner = sm_color_mix(sm_rgb(TOKEN_CYAN, 1.0f), tone, 0.55f);
	if (app->level >= SM_LEVEL_WARNING)
		inner = sm_color_mix(sm_rgb(TOKEN_CYAN, 1.0f), tone, 0.9f);
	inner = sm_color_mix(inner, sm_rgb(0xffffffU, 1.0f), 0.08f + 0.1f * breath);
	inner.a = 0.92f;
	ring = sm_rgb(TOKEN_CYAN, 0.28f);
	bright = sm_color_mix(sm_rgb(TOKEN_ICE, 0.9f), tone, 0.3f);

	/* The glow behind: three faint discs. */
	for (glow = 0; glow < 3U; glow++) {
		sm_draw_disc(&app->scene, cx, cy, scale * (1.9f - 0.4f * (float)glow), sm_color_mix(sm_rgb(0x000000U, 0.0f), tone, 0.05f));
	}

	/* The rings below, turning with the disks' work; the orbits' far halves. */
	space_ring(app, &camera, -1.25f, 1.6f, motion->ring_phase, 0.9f, ring, bright);
	space_ring(app, &camera, -1.38f, 1.85f, 1.0f - motion->ring_phase * 0.7f, 0.6f, ring, bright);
	space_orbits(app, &camera, 0);

	/* The outer shell's back faces, then the middle's. */
	center[0] = 0.0f;
	center[1] = 0.0f;
	center[2] = 0.0f;
	half[0] = 1.0f;
	half[1] = 0.95f;
	half[2] = 1.0f;
	space_box(app, &camera, center, half, outer, outer_top, 0.0f, 0);
	size = 0.64f * (1.0f + 0.08f * motion->gpu + 0.015f * motion->gpu * pulse);
	half[0] = size;
	half[1] = size * 0.95f;
	half[2] = size;
	space_box(app, &camera, center, half, middle, middle, 0.0f, 0);

	/* The inner core, all its faces, nearly opaque; the memory's points around it. */
	half[0] = 0.34f;
	half[1] = 0.32f;
	half[2] = 0.34f;
	space_box(app, &camera, center, half, inner, inner, 0.0f, 0);
	space_box(app, &camera, center, half, inner, sm_color_mix(inner, sm_rgb(0xffffffU, 1.0f), 0.2f), 0.5f, 1);
	space_points(app, &camera, sm_rgb(TOKEN_ICE, 0.85f));

	/* The middle shell's front faces, then the outer's, with their edges. */
	half[0] = size;
	half[1] = size * 0.95f;
	half[2] = size;
	space_box(app, &camera, center, half, middle, middle, 0.45f, 1);
	half[0] = 1.0f;
	half[1] = 0.95f;
	half[2] = 1.0f;
	space_box(app, &camera, center, half, outer, outer_top, 0.55f, 1);

	/* The orbits' near halves, in front. */
	space_orbits(app, &camera, 1);
}

/*
 * Draws the CPU cores' relief in a box: a shallow box a core on a tilted
 * field, raised by its load, lit at the edge above 85% (design.md section
 * 3.3).
 */
void
sm_space_relief(
	struct sm_app *app,
	const struct sm_box *box)
{
	static struct space_tile tiles[SM_CPU_MAX];
	struct space_camera camera;
	struct sm_color cold;
	struct sm_color hot;
	struct sm_color top;
	float center[3];
	float half[3];
	float screen[3];
	float corner[3];
	float low_x;
	float high_x;
	float low_y;
	float high_y;
	float step;
	float fit;
	float area_width;
	float area_height;
	float edge;
	unsigned count;
	unsigned columns;
	unsigned rows;
	unsigned index;
	unsigned corner_index;

	/* The grid: wider than deep. */
	count = app->source.info.cpu_count;
	if (count == 0U)
		return;
	columns = (unsigned)ceilf(sqrtf((float)count * 1.6f));
	if (columns > count)
		columns = count;
	rows = (count + columns - 1U) / columns;
	step = 1.25f;

	/* Each tile's place on the field, centred. */
	for (index = 0; index < count; index++) {
		tiles[index].x = ((float)(index % columns) - (float)(columns - 1U) * 0.5f) * step;
		tiles[index].z = ((float)(index / columns) - (float)(rows - 1U) * 0.5f) * step;
		tiles[index].load = (float)app->frame.cpu_core[index];
	}

	/* The field's extent on the screen at a unit scale, to fit it to the area under the title. */
	camera_set(&camera, 0.0f, 0.0f, 1.0f, -0.56f + app->motion.tilt_x * 0.02f, 0.66f + app->motion.tilt_y * 0.015f);
	low_x = 1.0e9f;
	high_x = -1.0e9f;
	low_y = 1.0e9f;
	high_y = -1.0e9f;
	for (corner_index = 0; corner_index < 8U; corner_index++) {
		corner[0] = corner_side(corner_index, 1U, 0.5f * (float)columns * step);
		corner[1] = corner_side(corner_index, 2U, 0.3f) + 0.3f;
		corner[2] = corner_side(corner_index, 4U, 0.5f * (float)rows * step);
		camera_project(&camera, corner, screen);
		low_x = fminf(low_x, screen[0]);
		high_x = fmaxf(high_x, screen[0]);
		low_y = fminf(low_y, screen[1]);
		high_y = fmaxf(high_y, screen[1]);
	}

	/* The scale and the centre that fit it. */
	area_width = box->width - 40.0f * app->layout.scale;
	area_height = box->height - 110.0f * app->layout.scale;
	fit = fminf(area_width / (high_x - low_x), area_height / (high_y - low_y));
	if (fit > 64.0f * app->layout.scale)
		fit = 64.0f * app->layout.scale;
	camera_set(&camera, box->x + box->width * 0.5f - (low_x + high_x) * 0.5f * fit,
		   box->y + 50.0f * app->layout.scale + area_height * 0.5f - (low_y + high_y) * 0.5f * fit, fit,
		   -0.56f + app->motion.tilt_x * 0.02f, 0.66f + app->motion.tilt_y * 0.015f);

	/* The tiles from the back: their depth on the screen. */
	for (index = 0; index < count; index++) {
		corner[0] = tiles[index].x;
		corner[1] = 0.0f;
		corner[2] = tiles[index].z;
		camera_project(&camera, corner, screen);
		tiles[index].depth = screen[2];
	}

	/* Sorted from the back. */
	qsort(tiles, count, sizeof(tiles[0]), tile_order);

	/* Each tile: its sides and its top, raised by its load. */
	cold = sm_rgb(0x2a3749U, 1.0f);
	hot = sm_color_mix(cold, sm_rgb(TOKEN_CYAN, 1.0f), 0.85f);
	for (index = 0; index < count; index++) {
		half[0] = 0.5f;
		half[1] = 0.03f + 0.28f * tiles[index].load;
		half[2] = 0.5f;
		center[0] = tiles[index].x;
		center[1] = half[1];
		center[2] = tiles[index].z;
		top = sm_color_mix(cold, hot, tiles[index].load);
		edge = 0.18f;
		if (tiles[index].load > 0.85f)
			edge = 0.9f;
		space_box(app, &camera, center, half, top, top, edge, 1);
	}
}

/* Sets a camera: the origin's place, pixels a unit, the turn about the vertical and the tilt down, in radians. */
static void
camera_set(
	struct space_camera *camera,
	float cx,
	float cy,
	float scale,
	float azimuth,
	float elevation)
{
	/* The turn and the tilt as their sines and cosines; a distant eye (a long lens). */
	camera->cx = cx;
	camera->cy = cy;
	camera->scale = scale;
	camera->cos_azimuth = cosf(azimuth);
	camera->sin_azimuth = sinf(azimuth);
	camera->cos_elevation = cosf(elevation);
	camera->sin_elevation = sinf(elevation);
	camera->depth = 9.0f;
}

/* Projects a point: its x and y on the screen, and its depth (larger is nearer). */
static void
camera_project(
	const struct space_camera *camera,
	const float *point,
	float *screen)
{
	float x;
	float y;
	float z;
	float perspective;

	/* Turned about the vertical, then tilted. */
	x = point[0] * camera->cos_azimuth + point[2] * camera->sin_azimuth;
	z = -point[0] * camera->sin_azimuth + point[2] * camera->cos_azimuth;
	y = point[1] * camera->cos_elevation - z * camera->sin_elevation;
	z = point[1] * camera->sin_elevation + z * camera->cos_elevation;

	/* A slight perspective. */
	perspective = camera->depth / (camera->depth - z);
	screen[0] = camera->cx + x * camera->scale * perspective;
	screen[1] = camera->cy - y * camera->scale * perspective;
	screen[2] = z;
}

/* How much a face's normal points at the eye (above 0: it faces the eye). */
static float
camera_facing(
	const struct space_camera *camera,
	const float *normal)
{
	float z;

	/* The normal turned and tilted like a point, its depth only. */
	z = -normal[0] * camera->sin_azimuth + normal[2] * camera->cos_azimuth;

	/* Succeeded: its depth. */
	return normal[1] * camera->sin_elevation + z * camera->cos_elevation;
}

/* Draws one face of a box, shaded by the light, and its edges at an alpha (0: none). */
static void
space_face(
	struct sm_app *app,
	const struct space_camera *camera,
	const float *center,
	const float *half,
	unsigned face,
	struct sm_color color,
	float edge_alpha)
{
	float screen[4][3];
	float corner[3];
	float triangle[6];
	float light;
	unsigned index;
	unsigned bits;
	struct sm_color edge;

	/* The face's four corners on the screen. */
	for (index = 0; index < 4U; index++) {
		bits = space_faces[face][index];
		corner[0] = center[0] + corner_side(bits, 1U, half[0]);
		corner[1] = center[1] + corner_side(bits, 2U, half[1]);
		corner[2] = center[2] + corner_side(bits, 4U, half[2]);
		camera_project(camera, corner, screen[index]);
	}

	/* Shaded by the light. */
	light = shade(space_normals[face]);
	color.r *= light;
	color.g *= light;
	color.b *= light;

	/* Two triangles. */
	triangle[0] = screen[0][0];
	triangle[1] = screen[0][1];
	triangle[2] = screen[1][0];
	triangle[3] = screen[1][1];
	triangle[4] = screen[2][0];
	triangle[5] = screen[2][1];
	sm_draw_triangle(&app->scene, triangle, color);
	triangle[2] = screen[2][0];
	triangle[3] = screen[2][1];
	triangle[4] = screen[3][0];
	triangle[5] = screen[3][1];
	sm_draw_triangle(&app->scene, triangle, color);

	/* The edges, thin and pale. */
	if (edge_alpha <= 0.0f)
		return;
	edge = sm_rgb(TOKEN_ICE, edge_alpha);
	for (index = 0; index < 4U; index++) {
		sm_draw_segment(&app->scene, screen[index][0], screen[index][1], screen[(index + 1U) % 4U][0], screen[(index + 1U) % 4U][1],
				1.0f, edge);
	}
}

/* Draws a box's faces that face away from the eye (front 0) or towards it (front 1); the top face in its own colour. */
static void
space_box(
	struct sm_app *app,
	const struct space_camera *camera,
	const float *center,
	const float *half,
	struct sm_color color,
	struct sm_color top,
	float edge_alpha,
	int front)
{
	float facing;
	unsigned face;

	/* Each face on the side asked for. */
	for (face = 0; face < 6U; face++) {
		facing = camera_facing(camera, space_normals[face]);
		if ((facing > 0.0f) != (front != 0))
			continue;

		/* The top in its own colour. */
		if (face == 3U)
			space_face(app, camera, center, half, face, top, edge_alpha);
		else
			space_face(app, camera, center, half, face, color, edge_alpha);
	}
}

/* Draws a level ring below the core, faint, with a bright arc of a share of the circle at a phase. */
static void
space_ring(
	struct sm_app *app,
	const struct space_camera *camera,
	float y,
	float radius,
	float phase,
	float arc,
	struct sm_color color,
	struct sm_color bright)
{
	float point[3];
	float from[3];
	float to[3];
	float angle;
	float position;
	unsigned index;
	struct sm_color shade_color;

	/* The ring's segments; those within the arc from the phase are bright. */
	point[1] = y;
	for (index = 0; index < SPACE_RING_POINTS; index++) {
		angle = (float)index / (float)SPACE_RING_POINTS * SPACE_TAU;
		point[0] = cosf(angle) * radius;
		point[2] = sinf(angle) * radius;
		camera_project(camera, point, from);
		angle = (float)(index + 1U) / (float)SPACE_RING_POINTS * SPACE_TAU;
		point[0] = cosf(angle) * radius;
		point[2] = sinf(angle) * radius;
		camera_project(camera, point, to);

		/* The arc fades from its head. */
		position = (float)index / (float)SPACE_RING_POINTS - phase;
		position -= floorf(position);
		shade_color = color;
		if (position < arc * 0.25f)
			shade_color = sm_color_mix(color, bright, 1.0f - position / (arc * 0.25f));
		sm_draw_segment(&app->scene, from[0], from[1], to[0], to[1], 1.2f, shade_color);
	}
}

/* Draws the orbits' faint lines and their moving points, the far half (front 0) or the near (front 1). */
static void
space_orbits(
	struct sm_app *app,
	const struct space_camera *camera,
	int front)
{
	static const float tilts[SPACE_ORBITS] = { 0.42f, -0.35f, 1.05f };
	float point[3];
	float from[3];
	float to[3];
	float angle;
	float flat_x;
	float flat_z;
	float cos_tilt;
	float sin_tilt;
	unsigned orbit;
	unsigned index;
	unsigned dot;
	struct sm_color line;
	struct sm_color mark;

	/* Each orbit: a circle of radius 1.45 in a plane tilted about the x axis. */
	line = sm_rgb(TOKEN_MINT, 0.12f + 0.15f * app->motion.network);
	mark = sm_rgb(TOKEN_MINT, 0.9f);
	for (orbit = 0; orbit < SPACE_ORBITS; orbit++) {
		cos_tilt = cosf(tilts[orbit]);
		sin_tilt = sinf(tilts[orbit]);

		/* Its line, segment by segment, on the side asked for. */
		for (index = 0; index < SPACE_RING_POINTS; index++) {
			angle = (float)index / (float)SPACE_RING_POINTS * SPACE_TAU;
			flat_x = cosf(angle) * 1.45f;
			flat_z = sinf(angle) * 1.45f;
			point[0] = flat_x;
			point[1] = -flat_z * sin_tilt;
			point[2] = flat_z * cos_tilt;
			camera_project(camera, point, from);
			angle = (float)(index + 1U) / (float)SPACE_RING_POINTS * SPACE_TAU;
			flat_x = cosf(angle) * 1.45f;
			flat_z = sinf(angle) * 1.45f;
			point[0] = flat_x;
			point[1] = -flat_z * sin_tilt;
			point[2] = flat_z * cos_tilt;
			camera_project(camera, point, to);
			if ((from[2] >= 0.0f) != (front != 0))
				continue;
			sm_draw_segment(&app->scene, from[0], from[1], to[0], to[1], 1.0f, line);
		}

		/* Its two points, moving with the network. */
		for (dot = 0; dot < 2U; dot++) {
			angle = (app->motion.orbit_phase + (float)dot * 0.5f + (float)orbit * 0.31f) * SPACE_TAU;
			if ((orbit & 1U) != 0U)
				angle = -angle;
			flat_x = cosf(angle) * 1.45f;
			flat_z = sinf(angle) * 1.45f;
			point[0] = flat_x;
			point[1] = -flat_z * sin_tilt;
			point[2] = flat_z * cos_tilt;
			camera_project(camera, point, from);
			if ((from[2] >= 0.0f) != (front != 0))
				continue;
			sm_draw_disc(&app->scene, from[0], from[1], 2.2f * app->layout.scale, mark);
		}
	}
}

/* Draws the memory's points between the inner core and the middle shell: more as the memory fills. */
static void
space_points(
	struct sm_app *app,
	const struct space_camera *camera,
	struct sm_color color)
{
	float point[3];
	float screen[3];
	uint32_t hash;
	unsigned count;
	unsigned index;
	unsigned axis;
	float available;
	float inside;

	/* How many: eight to the most, by the share in use. */
	count = 8U + (unsigned)(app->motion.memory * (float)(SPACE_POINTS - 8U));
	if (count > SPACE_POINTS)
		count = SPACE_POINTS;

	/* Amber when little is available. */
	available = 1.0f;
	if (app->source.info.memory_total != 0U)
		available = (float)app->frame.memory_available / (float)app->source.info.memory_total;
	if (available < 0.1f)
		color = sm_rgb(TOKEN_AMBER, 0.9f);

	/* Each point at a place of its own (a hash of its number), out of the inner core. */
	for (index = 0; index < count; index++) {
		hash = index * 2654435761U + 12345U;
		for (axis = 0; axis < 3U; axis++) {
			hash ^= hash >> 13;
			hash *= 0x5bd1e995U;
			hash ^= hash >> 15;
			point[axis] = ((float)(hash & 0xffffU) / 65535.0f - 0.5f) * 1.1f;
		}

		/* Pushed out of the inner core, up or down. */
		inside = fmaxf(fabsf(point[0]), fmaxf(fabsf(point[1]), fabsf(point[2])));
		if (inside < 0.4f) {
			point[1] = 0.45f;
			if ((hash & 2U) != 0U)
				point[1] = -0.45f;
		}

		/* The point on the screen. */
		camera_project(camera, point, screen);
		sm_draw_disc(&app->scene, screen[0], screen[1], 1.6f * app->layout.scale, color);
	}
}

/* The light a face gets: some always, more as it faces the light. */
static float
shade(
	const float *normal)
{
	float facing;

	/* The face's normal against the light. */
	facing = normal[0] * space_light[0] + normal[1] * space_light[1] + normal[2] * space_light[2];
	if (facing < 0.0f)
		facing = 0.0f;

	/* Succeeded: the shade, never quite dark. */
	return 0.55f + 0.45f * facing;
}

/* A value moved towards its target with a time constant (a first-order lag). */
static float
follow(
	float value,
	float target,
	float seconds,
	float time_constant)
{
	/* The share of the way covered in the time. */
	return value + (target - value) * (1.0f - expf(-seconds / time_constant));
}

/* A critically damped spring (2 Hz) moving a value towards its target; returns the new value. */
static float
spring(
	float *value,
	float *speed,
	float target,
	float seconds)
{
	float omega;
	float acceleration;
	float step;

	/*
	 * The pull towards the target and the damping, in steps of at most
	 * SPACE_SPRING_STEP: one step of a whole slow frame (a tenth of a
	 * second at 2 Hz) is unstable and grows without end (ws134-p010,
	 * T1-072: the tilt ran off at 4 frames a second and the background's
	 * grid filled the vertex memory).
	 */
	omega = 2.0f * SPACE_TAU;
	while (seconds > 0.0f) {
		step = fminf(seconds, SPACE_SPRING_STEP);
		acceleration = omega * omega * (target - *value) - 2.0f * omega * *speed;
		*speed += acceleration * step;
		*value += *speed * step;
		seconds -= step;
	}

	/* Succeeded: the value where the spring took it. */
	return *value;
}

/* A rate as a share from 0 to 1 on a logarithmic scale of three decades above a floor. */
static float
rate_share(
	double rate,
	double floor_rate)
{
	double share;

	/* Three decades above the floor fill it. */
	share = log10(1.0 + rate / floor_rate) / 3.0;
	if (share < 0.0)
		share = 0.0;
	if (share > 1.0)
		share = 1.0;

	/* Succeeded: the share. */
	return (float)share;
}

/* Orders tiles from the back (smaller depth) to the front. */
static int
tile_order(
	const void *left,
	const void *right)
{
	const struct space_tile *a;
	const struct space_tile *b;

	/* By depth. */
	a = left;
	b = right;
	if (a->depth < b->depth)
		return -1;
	if (a->depth > b->depth)
		return 1;

	/* As deep. */
	return 0;
}

/* A corner's coordinate on one axis: +extent when its bit is set, -extent otherwise. */
static float
corner_side(
	unsigned bits,
	unsigned bit,
	float extent)
{
	/* The bit says which side. */
	if ((bits & bit) != 0U)
		return extent;

	/* Otherwise the other side. */
	return -extent;
}
