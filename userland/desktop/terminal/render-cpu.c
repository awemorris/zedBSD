/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal on the processor: the quads of each frame
 * (render-build.c) painted into a frame of 0xAARRGGBB words.  Each quad is
 * a rectangle on whole pixels; a pixel inside it takes the texel of the
 * glyph atlas under its centre (nearest, as the GPU's sampler) and the
 * colour the cell shader makes of it, the background mixed toward the
 * foreground by the texel's coverage, the frame cleared to the theme's
 * background first.
 *
 * Only what changed is painted again.  The build keeps a row the same as
 * in the frame before where it was, so a quad outside the parts built is
 * the same as when it was last painted; a quad inside them is compared
 * with its copy from then.  A quad that changed marks the band of rows it
 * covers now and the one it covered before, and so does a quad of the
 * frame before past the end of this one.  Each band is cleared and every
 * quad that meets it painted again in order, so the band ends as a whole
 * frame painted from nothing would have it.  A change of what every row
 * depends on (the theme, the font, the size) paints the whole frame.
 */

#include "render-cpu.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The glyph atlas's size in pixels, as the Vulkan drawing's. */
#define CPU_ATLAS_WIDTH		2048U
#define CPU_ATLAS_HEIGHT	2048U

/* The floats of one quad's vertices. */
#define CPU_QUAD_FLOATS		(TERMINAL_QUAD_VERTICES * TERMINAL_VERTEX_FLOATS)

static void cpu_take(void *data, size_t first, size_t count);
static void cpu_quad_band(const float *quad, int *top, int *bottom);
static void cpu_band_add(struct terminal_cpu *cpu, int top, int bottom);
static void cpu_band_merge(struct terminal_cpu *cpu);
static int cpu_band_gap(const struct terminal_cpu_band *band, int top, int bottom);
static void cpu_paint(struct terminal_cpu *cpu, uint32_t vertex_count, int top, int bottom);
static void cpu_paint_quad(struct terminal_cpu *cpu, const float *quad, int top, int bottom);
static unsigned cpu_byte(float value);
static int cpu_whole(float value);

/*
 * Makes the drawing's memory: the glyph atlas (given to the font), the
 * build and the copy of the vertices painted.  The frame comes with
 * terminal_cpu_resize.
 *
 * Returns 0, or ENOMEM.
 */
int
terminal_cpu_open(
	struct terminal_cpu *cpu,
	struct terminal_font *font)
{
	int error;

	/* Nothing is owned yet. */
	memset(cpu, 0, sizeof(*cpu));

	/* The atlas, four bytes a pixel, which the font draws into. */
	cpu->atlas_width = CPU_ATLAS_WIDTH;
	cpu->atlas_height = CPU_ATLAS_HEIGHT;
	cpu->atlas_pitch = (size_t)CPU_ATLAS_WIDTH * 4U;
	cpu->atlas = malloc(cpu->atlas_pitch * CPU_ATLAS_HEIGHT);
	if (cpu->atlas == NULL)
		return ENOMEM;

	/* The font draws its first slots into it. */
	error = terminal_font_attach(font, cpu->atlas, cpu->atlas_pitch, CPU_ATLAS_WIDTH, CPU_ATLAS_HEIGHT);
	if (error != 0)
		return error;

	/* The quads of each frame; a part built when the list is full is taken at once. */
	error = terminal_build_open(&cpu->build);
	if (error != 0)
		return error;
	cpu->build.store = cpu_take;
	cpu->build.store_data = cpu;

	/* The copy of the vertices painted (none yet). */
	cpu->previous = malloc(cpu->build.capacity * TERMINAL_VERTEX_FLOATS * sizeof(float));
	if (cpu->previous == NULL)
		return ENOMEM;
	cpu->previous_count = 0U;

	/* Succeeded: the first frame is painted whole. */
	cpu->whole = 1;
	return 0;
}

/*
 * Makes the frame a new size; the next frame is painted whole.
 *
 * Returns 0, or ENOMEM with the frame as it was.
 */
int
terminal_cpu_resize(
	struct terminal_cpu *cpu,
	uint32_t width,
	uint32_t height)
{
	uint32_t *pixels;

	/* The same size keeps the frame. */
	if (cpu->pixels != NULL &&
	    width == cpu->width &&
	    height == cpu->height)
		return 0;

	/* The frame's memory, a word a pixel (at least one pixel). */
	pixels = malloc((size_t)(width + 1U) * (size_t)(height + 1U) * sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	/* The new frame replaces the old one and is painted whole. */
	free(cpu->pixels);
	cpu->pixels = pixels;
	cpu->width = width;
	cpu->height = height;
	cpu->whole = 1;

	/* Succeeded: frames are painted at the new size. */
	return 0;
}

/*
 * Builds the frame's quads and paints what changed of them.
 *
 * Returns 1 when the frame changed, with the parts painted in parts and
 * *count (0 for the whole frame; parts holds TERMINAL_CPU_BANDS), or 0
 * when nothing changed.
 */
int
terminal_cpu_draw(
	struct terminal_cpu *cpu,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window,
	struct kl_rect *parts,
	unsigned *count)
{
	const float *quad;
	uint32_t vertex_count;
	uint32_t ground;
	unsigned index;
	size_t vertex;
	int top;
	int bottom;

	/* The quads, at the frame's size. */
	cpu->build.width = cpu->width;
	cpu->build.height = cpu->height;
	vertex_count = terminal_build_frame(&cpu->build, screen, font, window);

	/* A change of the theme, the font or the size, or another background, paints the whole frame. */
	ground = terminal_build_theme(window)->background;
	if (cpu->build.key_changed)
		cpu->whole = 1;
	if (ground != cpu->ground)
		cpu->whole = 1;
	cpu->ground = ground;

	/* The parts built: their quads that changed mark their bands. */
	for (index = 0U; index < cpu->build.range_count; index++)
		cpu_take(cpu, cpu->build.ranges[index].first, cpu->build.ranges[index].count);
	cpu->build.range_count = 0U;

	/* The quads of the frame before past this one's end leave their bands. */
	for (vertex = vertex_count;
	     vertex + TERMINAL_QUAD_VERTICES <= cpu->previous_count;
	     vertex += TERMINAL_QUAD_VERTICES) {
		quad = cpu->previous + vertex * TERMINAL_VERTEX_FLOATS;
		cpu_quad_band(quad, &top, &bottom);
		cpu_band_add(cpu, top, bottom);
	}
	cpu->previous_count = vertex_count;

	/* The whole frame, cleared and painted. */
	*count = 0U;
	if (cpu->whole) {
		cpu->whole = 0;
		cpu->band_count = 0U;
		cpu_paint(cpu, vertex_count, 0, (int)cpu->height);
		return 1;
	}

	/* Nothing changed: nothing to show. */
	if (cpu->band_count == 0U)
		return 0;

	/* Each band, cleared and painted, is a part shown. */
	for (index = 0U; index < cpu->band_count; index++) {
		cpu_paint(cpu, vertex_count, cpu->bands[index].top, cpu->bands[index].bottom);
		parts[index].x = 0;
		parts[index].y = cpu->bands[index].top;
		parts[index].width = (int)cpu->width;
		parts[index].height = cpu->bands[index].bottom - cpu->bands[index].top;
	}

	/* Succeeded: the bands are the parts that changed. */
	*count = cpu->band_count;
	cpu->band_count = 0U;
	return 1;
}

/*
 * Releases the drawing's memory (the font's atlas with it: the font is
 * closed or attached again first).
 */
void
terminal_cpu_close(
	struct terminal_cpu *cpu)
{
	/* The build, the copy, the frame and the atlas. */
	terminal_build_close(&cpu->build);
	free(cpu->previous);
	free(cpu->pixels);
	free(cpu->atlas);
	memset(cpu, 0, sizeof(*cpu));
}

/*
 * Takes a part of the frame's vertices that was built: each quad that
 * differs from its copy, or has none, marks the band it covers now and the
 * one its copy covered, and the copy follows.
 */
static void
cpu_take(
	void *data,
	size_t first,
	size_t count)
{
	struct terminal_cpu *cpu;
	const float *quad;
	float *kept;
	size_t vertex;
	size_t end;
	int differs;
	int top;
	int bottom;

	/* Each whole quad of the part. */
	cpu = data;
	end = first + count;
	for (vertex = first;
	     vertex + TERMINAL_QUAD_VERTICES <= end;
	     vertex += TERMINAL_QUAD_VERTICES) {
		quad = cpu->build.vertices + vertex * TERMINAL_VERTEX_FLOATS;
		kept = cpu->previous + vertex * TERMINAL_VERTEX_FLOATS;

		/* A quad the same as its copy changes nothing. */
		if (vertex + TERMINAL_QUAD_VERTICES <= cpu->previous_count) {
			differs = memcmp(quad, kept, CPU_QUAD_FLOATS * sizeof(float));
			if (differs == 0)
				continue;

			/* The band its copy covered. */
			cpu_quad_band(kept, &top, &bottom);
			cpu_band_add(cpu, top, bottom);
		}

		/* The band it covers now, and the copy. */
		cpu_quad_band(quad, &top, &bottom);
		cpu_band_add(cpu, top, bottom);
		memcpy(kept, quad, CPU_QUAD_FLOATS * sizeof(float));
	}
}

/* Gives the rows a quad covers: its first corner's top to its fifth corner's bottom. */
static void
cpu_quad_band(
	const float *quad,
	int *top,
	int *bottom)
{
	/* The first corner is the top left, the fifth the bottom right. */
	*top = cpu_whole(quad[1]);
	*bottom = cpu_whole(quad[4U * TERMINAL_VERTEX_FLOATS + 1U]);
}

/*
 * Adds a band of rows to paint again, within the frame: joined to a band
 * it meets, and the two nearest bands joined when there are too many.
 */
static void
cpu_band_add(
	struct terminal_cpu *cpu,
	int top,
	int bottom)
{
	struct terminal_cpu_band *band;
	unsigned index;
	int nearest;
	int gap;

	/* Only the frame's rows. */
	if (top < 0)
		top = 0;
	if (bottom > (int)cpu->height)
		bottom = (int)cpu->height;
	if (top >= bottom)
		return;

	/* A band it meets takes it in. */
	for (index = 0U; index < cpu->band_count; index++) {
		band = &cpu->bands[index];
		if (top > band->bottom || bottom < band->top)
			continue;

		/* The band reaches over both, then may meet another. */
		if (top < band->top)
			band->top = top;
		if (bottom > band->bottom)
			band->bottom = bottom;
		cpu_band_merge(cpu);
		return;
	}

	/* A full list: it joins the band nearest to it (more rows are painted, none missed). */
	if (cpu->band_count == TERMINAL_CPU_BANDS) {
		band = &cpu->bands[0];
		nearest = cpu_band_gap(band, top, bottom);
		for (index = 1U; index < cpu->band_count; index++) {
			gap = cpu_band_gap(&cpu->bands[index], top, bottom);
			if (gap < nearest) {
				band = &cpu->bands[index];
				nearest = gap;
			}
		}

		/* The band reaches over both, then may meet another. */
		if (top < band->top)
			band->top = top;
		if (bottom > band->bottom)
			band->bottom = bottom;
		cpu_band_merge(cpu);
		return;
	}

	/* Succeeded: a new band. */
	cpu->bands[cpu->band_count].top = top;
	cpu->bands[cpu->band_count].bottom = bottom;
	cpu->band_count++;
}

/* Gives how many rows lie between a band and the rows from top to bottom (0 when they meet). */
static int
cpu_band_gap(
	const struct terminal_cpu_band *band,
	int top,
	int bottom)
{
	/* The rows above the band. */
	if (bottom < band->top)
		return band->top - bottom;

	/* The rows below it. */
	if (top > band->bottom)
		return top - band->bottom;

	/* Reports rows that meet it. */
	return 0;
}

/* Joins the bands that meet, until none meets another. */
static void
cpu_band_merge(
	struct terminal_cpu *cpu)
{
	struct terminal_cpu_band *first;
	struct terminal_cpu_band *second;
	unsigned one;
	unsigned other;
	int joined;

	/* Every pair, again after each join. */
	joined = 1;
	while (joined) {
		joined = 0;
		for (one = 0U; one < cpu->band_count && !joined; one++) {
			for (other = one + 1U; other < cpu->band_count && !joined; other++) {
				first = &cpu->bands[one];
				second = &cpu->bands[other];
				if (second->top > first->bottom || second->bottom < first->top)
					continue;

				/* The first reaches over both; the last band takes the second's place. */
				if (second->top < first->top)
					first->top = second->top;
				if (second->bottom > first->bottom)
					first->bottom = second->bottom;
				*second = cpu->bands[cpu->band_count - 1U];
				cpu->band_count--;
				joined = 1;
			}
		}
	}
}

/*
 * Paints the rows from top to bottom (exclusive) as a whole frame would
 * have them: cleared to the background, then every quad that meets them,
 * in order.
 */
static void
cpu_paint(
	struct terminal_cpu *cpu,
	uint32_t vertex_count,
	int top,
	int bottom)
{
	const float *quad;
	uint32_t ground;
	uint32_t vertex;
	uint32_t *row;
	uint32_t x;
	int y;

	/* The background, opaque. */
	ground = 0xff000000U | cpu->ground;
	for (y = top; y < bottom; y++) {
		row = cpu->pixels + (size_t)y * cpu->width;
		for (x = 0U; x < cpu->width; x++)
			row[x] = ground;
	}

	/* Each quad, over what is before it. */
	for (vertex = 0U;
	     vertex + TERMINAL_QUAD_VERTICES <= vertex_count;
	     vertex += TERMINAL_QUAD_VERTICES) {
		quad = cpu->build.vertices + (size_t)vertex * TERMINAL_VERTEX_FLOATS;
		cpu_paint_quad(cpu, quad, top, bottom);
	}
}

/*
 * Paints one quad within the rows from top to bottom: each pixel the
 * atlas's texel under its centre, the background mixed toward the
 * foreground by the texel's coverage (cell.frag), rounded as a UNORM
 * target rounds.
 */
static void
cpu_paint_quad(
	struct terminal_cpu *cpu,
	const float *quad,
	int top,
	int bottom)
{
	const unsigned char *texel;
	uint32_t *row;
	unsigned foreground[3];
	unsigned background[3];
	unsigned cover;
	unsigned channel;
	unsigned mixed[3];
	int left;
	int right;
	int upper;
	int lower;
	int texel_x;
	int texel_y;
	int x;
	int y;
	int atlas_x;
	int atlas_y;

	/* The quad's rectangle (the first corner's top left, the fifth's bottom right) and its atlas place. */
	left = cpu_whole(quad[0]);
	upper = cpu_whole(quad[1]);
	right = cpu_whole(quad[4U * TERMINAL_VERTEX_FLOATS]);
	lower = cpu_whole(quad[4U * TERMINAL_VERTEX_FLOATS + 1U]);
	texel_x = cpu_whole(quad[2] * (float)cpu->atlas_width);
	texel_y = cpu_whole(quad[3] * (float)cpu->atlas_height);

	/* Its colours. */
	for (channel = 0U; channel < 3U; channel++) {
		foreground[channel] = cpu_byte(quad[4U + channel]);
		background[channel] = cpu_byte(quad[8U + channel]);
	}

	/* Only the rows asked for and the frame's columns. */
	if (upper < top)
		upper = top;
	if (lower > bottom)
		lower = bottom;
	if (left < 0)
		left = 0;
	if (right > (int)cpu->width)
		right = (int)cpu->width;

	/* Each pixel. */
	for (y = upper; y < lower; y++) {
		row = cpu->pixels + (size_t)y * cpu->width;
		atlas_y = texel_y + (y - cpu_whole(quad[1]));
		if (atlas_y < 0)
			atlas_y = 0;
		if (atlas_y >= (int)cpu->atlas_height)
			atlas_y = (int)cpu->atlas_height - 1;
		for (x = left; x < right; x++) {
			/* The texel under the pixel's centre, at the atlas's edge beyond it. */
			atlas_x = texel_x + (x - cpu_whole(quad[0]));
			if (atlas_x < 0)
				atlas_x = 0;
			if (atlas_x >= (int)cpu->atlas_width)
				atlas_x = (int)cpu->atlas_width - 1;
			texel = cpu->atlas + (size_t)atlas_y * cpu->atlas_pitch + (size_t)atlas_x * 4U;
			cover = texel[3];

			/* The background toward the foreground by the coverage. */
			for (channel = 0U; channel < 3U; channel++)
				mixed[channel] = (background[channel] * (255U - cover) + foreground[channel] * cover + 127U) / 255U;
			row[x] = 0xff000000U | (mixed[0] << 16) | (mixed[1] << 8) | mixed[2];
		}
	}
}

/* Turns a colour channel of a vertex (0 to 1) back into its byte. */
static unsigned
cpu_byte(
	float value)
{
	unsigned byte;

	/* The nearest byte, within 0 and 255. */
	byte = 0U;
	if (value > 0.0f)
		byte = (unsigned)(value * 255.0f + 0.5f);
	if (byte > 255U)
		byte = 255U;

	/* Reports the byte. */
	return byte;
}

/* Turns a coordinate on whole pixels (as the quads are) into an integer, the nearest one. */
static int
cpu_whole(
	float value)
{
	int whole;

	/* The nearest integer, either side of zero. */
	if (value < 0.0f) {
		whole = -(int)(-value + 0.5f);
	} else {
		whole = (int)(value + 0.5f);
	}

	/* Reports it. */
	return whole;
}
