/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look of window mode (ws035-p059, a proof of the look and feel).
 *
 * The wallpaper is a pale blue landscape the CPU draws once.  A quarter-size,
 * blurred copy of it is the frosted glass: a glass panel samples it at its
 * own place on the output and whitens it (like Windows' Mica, only the
 * wallpaper shows through; windows under a panel do not).  Each window has a
 * title bar floating above its body with a gap: a rounded glass panel with
 * the window's title and its minimize, maximize and close buttons.  The body
 * has rounded corners and a shadow.  The system bar along the top is a
 * mock-up: the launcher and system menu on the left, the notification area
 * on the right, with no actions yet.
 *
 * Text is drawn from a glyph atlas made with libtruetype from the font at
 * server->font_path (printable ASCII and the multiplication sign); without a
 * font the look is drawn without text.
 */

#include "compose.h"

#include <truetype.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The atlas holds printable ASCII and the multiplication sign (the close button) at three sizes. */
#define GLASS_GLYPHS		96U
#define GLASS_CLOSE_GLYPH	95U
#define GLASS_SIZES		3U
#define GLASS_ATLAS_WIDTH	1024U
#define GLASS_ATLAS_HEIGHT	128U
#define GLASS_FILE_MAX		(16U * 1024U * 1024U)

/* The blurred wallpaper is this many times smaller than the output. */
#define GLASS_BLUR_SCALE	4U
#define GLASS_BLUR_RADIUS	6U
#define GLASS_BLUR_PASSES	3U

/* The shapes the panel shader draws (shaders/panel.frag). */
#define MODE_GLASS		0.0f
#define MODE_SHADOW		1.0f
#define MODE_IMAGE		2.0f
#define MODE_SOLID		3.0f
#define MODE_RING		4.0f
#define MODE_TEXT		5.0f

/* The title bar's buttons, counted from the right edge. */
#define BUTTON_CLOSE		0
#define BUTTON_MAXIMIZE		1
#define BUTTON_MINIMIZE		2
#define BUTTON_COUNT		3
#define BUTTON_SPACING		34
#define BUTTON_WIDTH		30
#define BUTTON_HEIGHT		28

/* The corner radius of title bars and bodies. */
#define GLASS_RADIUS		14.0f

/* The text sizes: the system bar, the titles, the close sign. */
enum glass_size {
	SIZE_BAR,
	SIZE_TITLE,
	SIZE_SIGN
};

/* Where the pointer is over a window. */
enum glass_hit {
	HIT_NONE,
	HIT_TITLE,
	HIT_BODY
};

/* One glyph's place in the atlas and its metrics in pixels. */
struct glass_glyph {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	int32_t left;
	int32_t top;
	int32_t advance;
};

/* The look's images and glyphs. */
struct zwl_glass {
	struct zwl_import wallpaper;
	struct zwl_import blurred;
	struct zwl_import atlas;
	struct glass_glyph glyphs[GLASS_SIZES][GLASS_GLYPHS];
	unsigned text;
};

/* One shape for the panel shader, in output pixels. */
struct glass_shape {
	float quad[4];
	float box[4];
	float uv[4];
	float color[4];
	float radius;
	float mode;
	float soft;
	float opaque;
	float edge;
	float opacity;
	VkDescriptorSet set;
};

static const unsigned glass_pixels[GLASS_SIZES] = { 14U, 15U, 20U };

static int wallpaper_create(struct zwl_server *server, struct zwl_glass *glass);
static void wallpaper_pixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float *rgb);
static float ridge(float x, float base, float amplitude, float phase);
static int blur_create(struct zwl_server *server, struct zwl_glass *glass, const float *pixels);
static void blur_pass(float *pixels, float *scratch, uint32_t width, uint32_t height, int horizontal);
static uint32_t pack_pixel(const float *rgb);
static int atlas_create(struct zwl_server *server, struct zwl_glass *glass);
static int atlas_fill(struct zwl_glass *glass, struct truetype_face *face);
static void *file_read(const char *path, size_t *size);
static float *wallpaper_load(const char *path, uint32_t width, uint32_t height);
static int ppm_number(const unsigned char *data, size_t size, size_t *at, uint32_t *number);
static void shape_init(struct glass_shape *shape, float x, float y, float width, float height);
static void shape_draw(struct zwl_server *server, VkCommandBuffer command, const struct glass_shape *shape);
static void draw_solid(struct zwl_server *server, VkCommandBuffer command, float x, float y, float width, float height, float radius, const float *color);
static int32_t text_width(const struct zwl_glass *glass, enum glass_size size, const char *text);
static void draw_text(struct zwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color);
static void draw_glyph(struct zwl_server *server, VkCommandBuffer command, enum glass_size size, unsigned index, int32_t x, int32_t baseline, const float *color);
static void draw_window(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, unsigned focused);
static void draw_title_bar(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, unsigned focused);
static void draw_button(struct zwl_server *server, VkCommandBuffer command, struct zwl_object *surface, int button, const float *ink);
static void draw_system_bar(struct zwl_server *server, VkCommandBuffer command);
static void window_size(const struct zwl_object *surface, int32_t *width, int32_t *height);
static enum glass_hit window_hit(const struct zwl_object *surface, int32_t x, int32_t y);
static int button_at(const struct zwl_object *surface, int32_t x, int32_t y);
static void button_centre(const struct zwl_object *surface, int button, int32_t *x, int32_t *y);
static struct zwl_object *window_at(struct zwl_server *server, int32_t x, int32_t y, enum glass_hit *hit);
static void window_raise(struct zwl_server *server, struct zwl_object *surface);
static void window_maximize(struct zwl_server *server, struct zwl_object *surface);

/*
 * Makes the wallpaper, its blurred copy and the glyph atlas.
 */
int
zwl_glass_open(
	struct zwl_server *server)
{
	struct zwl_glass *glass;
	int error;

	/* The look's state lives as long as window mode's device. */
	glass = calloc(1, sizeof(*glass));
	if (glass == NULL)
		return ENOMEM;
	server->compose->glass = glass;

	/* The wallpaper and the frosted glass made from it. */
	error = wallpaper_create(server, glass);
	if (error != 0)
		return error;

	/* The glyphs; the look is drawn without text when the font cannot be read. */
	error = atlas_create(server, glass);
	if (error != 0)
		printf("ZWL GLASS no text: font=%s errno=%d\n", server->font_path, error);

	/* Succeeded. */
	printf("ZWL GLASS ready text=%u\n", glass->text);
	return 0;
}

/*
 * Releases the look's images.
 */
void
zwl_glass_close(
	struct zwl_server *server)
{
	struct zwl_glass *glass;

	/* Nothing was made. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return;

	/* The images, then the record. */
	glass = server->compose->glass;
	zwl_host_image_release(server->compose, &glass->wallpaper);
	zwl_host_image_release(server->compose, &glass->blurred);
	zwl_host_image_release(server->compose, &glass->atlas);
	free(glass);
	server->compose->glass = NULL;
}

/*
 * Draws the wallpaper, the windows from the bottom with their shadows and
 * title bars, and the system bar over them.
 */
void
zwl_glass_draw(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object **windows,
	unsigned count)
{
	struct glass_shape shape;
	struct zwl_object *top;
	unsigned index;
	unsigned focused;

	/* The wallpaper over the whole output. */
	shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_IMAGE;
	shape.opaque = 1.0f;
	shape.set = server->compose->glass->wallpaper.set;
	shape_draw(server, command, &shape);

	/* The windows; the top one has the focus. */
	top = zwl_top_window(server);
	for (index = 0; index < count; index++) {
		focused = 0;
		if (windows[index] == top)
			focused = 1;
		draw_window(server, command, windows[index], focused);
	}

	/* The system bar over everything but the cursor. */
	draw_system_bar(server, command);
}

/*
 * Handles a pointer button in the glass look.  A press raises the window
 * under the pointer; on its title bar it starts a move or presses a button.
 * Returns 1 when the button is zdesktop's, 0 when it goes to the client.
 */
int
zwl_glass_button(
	struct zwl_server *server,
	uint32_t button,
	uint32_t state)
{
	struct zwl_object *surface;
	enum glass_hit hit;
	int pressed;

	/* A release ends a move. */
	if (state == 0) {
		if (server->drag == NULL)
			return 0;
		printf("ZWL GLASS moved surface=%u x=%d y=%d\n", server->drag->id, server->drag->x, server->drag->y);
		server->drag = NULL;
		return 1;
	}

	/* Only the left button acts on windows. */
	surface = window_at(server, server->pointer_x, server->pointer_y, &hit);
	if (button != ZWL_BUTTON_LEFT)
		return hit != HIT_BODY;

	/* A press on the desktop is zdesktop's. */
	if (surface == NULL)
		return 1;

	/* The window comes to the top and takes the focus. */
	window_raise(server, surface);

	/* On the body the client has the press. */
	if (hit == HIT_BODY)
		return 0;

	/* On a button, its action. */
	pressed = button_at(surface, server->pointer_x, server->pointer_y);
	if (pressed == BUTTON_CLOSE) {
		(void)zwl_emit(surface->client, surface->role->top->id, 1U, NULL, 0U);
		printf("ZWL GLASS close surface=%u\n", surface->id);
		return 1;
	}

	/* Maximize, or back. */
	if (pressed == BUTTON_MAXIMIZE) {
		window_maximize(server, surface);
		return 1;
	}

	/* Minimize has no action yet. */
	if (pressed == BUTTON_MINIMIZE)
		return 1;

	/* Elsewhere on the title bar, a move (not of a maximized window). */
	if (!surface->maximized) {
		server->drag = surface;
		server->drag_dx = server->pointer_x - surface->x;
		server->drag_dy = server->pointer_y - surface->y;
	}

	/* Succeeded: the press was zdesktop's. */
	return 1;
}

/*
 * Moves the window being moved with the pointer.  Returns 1 when the motion
 * is zdesktop's.
 */
int
zwl_glass_motion(
	struct zwl_server *server)
{
	struct zwl_object *surface;
	int32_t lowest;

	/* Without a move the client hears the motion; the hover of buttons is redrawn. */
	surface = server->drag;
	server->dirty = 1;
	if (surface == NULL)
		return 0;

	/* A window that went away ends the move. */
	if (surface->dead || !surface->mapped) {
		server->drag = NULL;
		return 1;
	}

	/* The body follows the pointer; the title bar stays below the system bar. */
	surface->x = server->pointer_x - server->drag_dx;
	surface->y = server->pointer_y - server->drag_dy;
	lowest = ZWL_GLASS_BAR + ZWL_GLASS_GAP + ZWL_GLASS_TITLE;
	if (surface->y < lowest)
		surface->y = lowest;

	/* Succeeded: the motion was zdesktop's. */
	return 1;
}

/*
 * Places a new window in the glass look: centred in the space below the
 * system bar, cascaded like the plain look.
 */
void
zwl_glass_place(
	struct zwl_server *server,
	struct zwl_object *surface,
	int32_t width,
	int32_t height,
	int32_t step)
{
	int32_t space;

	/* The space for bodies under the system bar and a title bar. */
	space = (int32_t)server->height - ZWL_GLASS_TOP - ZWL_GLASS_MARGIN;
	surface->x = ((int32_t)server->width - width) / 2 + step;
	surface->y = ZWL_GLASS_TOP + (space - height) / 2 + step;

	/* Never above the space, nor left of the output. */
	if (surface->x < ZWL_GLASS_MARGIN)
		surface->x = ZWL_GLASS_MARGIN;
	if (surface->y < ZWL_GLASS_TOP)
		surface->y = ZWL_GLASS_TOP;
}

/*
 * Redraws when the system bar's clock shows a new minute.
 */
void
zwl_glass_tick(
	struct zwl_server *server)
{
	time_t now;

	/* The minute of the clock. */
	now = time(NULL);
	if ((int64_t)now / 60 == server->clock_minute)
		return;

	/* A new minute. */
	server->clock_minute = (int64_t)now / 60;
	server->dirty = 1;
}

/* Draws the wallpaper once, and its blurred copy. */
static int
wallpaper_create(
	struct zwl_server *server,
	struct zwl_glass *glass)
{
	uint32_t *row;
	float *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	VkResult result;
	int error;

	/* The output-sized image. */
	width = server->width;
	height = server->height;
	result = zwl_host_image_create(server->compose, width, height, server->compose->sampler, &glass->wallpaper);
	if (result != VK_SUCCESS)
		return EIO;

	/* The picture given, in floating point and kept for the blur. */
	pixels = NULL;
	if (server->wallpaper_path != NULL) {
		pixels = wallpaper_load(server->wallpaper_path, width, height);
		if (pixels == NULL)
			printf("ZWL GLASS no wallpaper: path=%s errno=%d\n", server->wallpaper_path, errno);
	}

	/* Otherwise the landscape drawn here. */
	if (pixels == NULL) {
		pixels = malloc((size_t)width * height * 3U * sizeof(float));
		if (pixels == NULL)
			return ENOMEM;
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++)
				wallpaper_pixel(x, y, width, height, &pixels[((size_t)y * width + x) * 3U]);
		}
	}

	/* Into the image. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)((unsigned char *)glass->wallpaper.map + y * glass->wallpaper.row_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&pixels[((size_t)y * width + x) * 3U]);
	}

	/* The frosted glass. */
	error = blur_create(server, glass, pixels);
	free(pixels);
	return error;
}

/*
 * Colors one pixel of the landscape: a sky that pales towards the horizon
 * with a soft sun, three ranges of misty mountains, and a still lake that
 * reflects them.
 */
static void
wallpaper_pixel(
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	float *rgb)
{
	static const float ranges[3][5] = {
		/* base, amplitude, phase, and the color's green and blue (red is below). */
		{ 0.50f, 0.10f, 0.3f, 0.82f, 0.94f },
		{ 0.58f, 0.09f, 2.1f, 0.74f, 0.90f },
		{ 0.66f, 0.07f, 4.7f, 0.64f, 0.85f }
	};
	static const float reds[3] = { 0.74f, 0.62f, 0.50f };
	float u;
	float v;
	float mirror;
	float top;
	float mist;
	float sun;
	float lake;
	unsigned index;

	/* The place, as fractions of the output. */
	u = (float)x / (float)width;
	v = (float)y / (float)height;
	lake = 0.80f;

	/* Under the lake's edge the landscape is seen in the water. */
	mirror = v;
	if (v > lake)
		mirror = 2.0f * lake - v;

	/* The sky: pale blue above, nearly white at the horizon, a glow to the upper right. */
	rgb[0] = 0.78f + 0.18f * mirror;
	rgb[1] = 0.86f + 0.11f * mirror;
	rgb[2] = 0.97f + 0.02f * mirror;
	sun = expf(-((u - 0.78f) * (u - 0.78f) * 18.0f + (mirror - 0.18f) * (mirror - 0.18f) * 30.0f));
	rgb[0] += 0.12f * sun;
	rgb[1] += 0.09f * sun;
	rgb[2] += 0.03f * sun;

	/* Each range in front of the ones behind it, misty towards its foot. */
	for (index = 0; index < 3U; index++) {
		top = ridge(u, ranges[index][0], ranges[index][1], ranges[index][2]);
		if (mirror < top)
			continue;
		mist = (mirror - top) / 0.18f;
		if (mist > 1.0f)
			mist = 1.0f;
		rgb[0] = reds[index] + (0.92f - reds[index]) * mist * 0.55f;
		rgb[1] = ranges[index][3] + (0.95f - ranges[index][3]) * mist * 0.55f;
		rgb[2] = ranges[index][4] + (0.99f - ranges[index][4]) * mist * 0.55f;
	}

	/* The water is darker and bluer than what it reflects, deeper towards the bottom. */
	if (v > lake) {
		rgb[0] = rgb[0] * 0.88f - (v - lake) * 0.35f;
		rgb[1] = rgb[1] * 0.92f - (v - lake) * 0.25f;
		rgb[2] = rgb[2] * 0.98f - (v - lake) * 0.08f;
		rgb[0] += 0.006f * sinf((float)y * 0.9f + u * 3.0f);
		rgb[1] += 0.006f * sinf((float)y * 0.9f + u * 3.0f);
	}

	/* Each channel within 0..1. */
	for (index = 0; index < 3U; index++) {
		if (rgb[index] > 1.0f)
			rgb[index] = 1.0f;
		if (rgb[index] < 0.0f)
			rgb[index] = 0.0f;
	}
}

/* The height (as a fraction of the output) of a mountain range's ridge at x. */
static float
ridge(
	float x,
	float base,
	float amplitude,
	float phase)
{
	float wave;
	float peak;

	/* A few waves of falling length make saddles, and a sharp term the peaks. */
	wave = 0.55f * sinf(x * 5.1f + phase);
	wave += 0.30f * sinf(x * 11.3f + phase * 1.7f);
	wave += 0.15f * sinf(x * 23.9f + phase * 2.3f);
	peak = 1.0f - fabsf(sinf(x * 4.3f + phase * 0.7f));
	wave += 0.60f * peak * peak * peak;

	/* Higher waves are lower on the output. */
	return base - amplitude * wave;
}

/*
 * Makes the frosted glass: the wallpaper averaged down by GLASS_BLUR_SCALE
 * and blurred by repeated box passes (close to a Gaussian).
 */
static int
blur_create(
	struct zwl_server *server,
	struct zwl_glass *glass,
	const float *pixels)
{
	uint32_t *row;
	float *small;
	float *scratch;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	uint32_t dx;
	uint32_t dy;
	uint32_t channel;
	unsigned pass;
	size_t at;
	VkResult result;

	/* The small image, sampled linearly. */
	width = server->width / GLASS_BLUR_SCALE;
	height = server->height / GLASS_BLUR_SCALE;
	result = zwl_host_image_create(server->compose, width, height, server->compose->linear_sampler, &glass->blurred);
	if (result != VK_SUCCESS)
		return EIO;

	/* Its working copies. */
	small = calloc((size_t)width * height * 3U, sizeof(float));
	scratch = calloc((size_t)width * height * 3U, sizeof(float));
	if (small == NULL || scratch == NULL) {
		free(small);
		free(scratch);
		return ENOMEM;
	}

	/* Each small pixel is the average of the block it covers. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (dy = 0; dy < GLASS_BLUR_SCALE; dy++) {
				for (dx = 0; dx < GLASS_BLUR_SCALE; dx++) {
					at = ((size_t)(y * GLASS_BLUR_SCALE + dy) * server->width + x * GLASS_BLUR_SCALE + dx) * 3U;
					for (channel = 0; channel < 3U; channel++)
						small[((size_t)y * width + x) * 3U + channel] += pixels[at + channel];
				}
			}

			/* The sum becomes the average. */
			for (channel = 0; channel < 3U; channel++)
				small[((size_t)y * width + x) * 3U + channel] /= (float)(GLASS_BLUR_SCALE * GLASS_BLUR_SCALE);
		}
	}

	/* Box passes across and down. */
	for (pass = 0; pass < GLASS_BLUR_PASSES; pass++) {
		blur_pass(small, scratch, width, height, 1);
		blur_pass(small, scratch, width, height, 0);
	}

	/* Into the image. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)((unsigned char *)glass->blurred.map + y * glass->blurred.row_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&small[((size_t)y * width + x) * 3U]);
	}

	/* Succeeded. */
	free(small);
	free(scratch);
	return 0;
}

/* Averages each pixel with its GLASS_BLUR_RADIUS neighbours on each side, across or down (clamped at the edges). */
static void
blur_pass(
	float *pixels,
	float *scratch,
	uint32_t width,
	uint32_t height,
	int horizontal)
{
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	int32_t offset;
	int32_t sx;
	int32_t sy;
	float sum;

	/* Each output pixel from the input. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (channel = 0; channel < 3U; channel++) {
				sum = 0.0f;
				for (offset = -(int32_t)GLASS_BLUR_RADIUS; offset <= (int32_t)GLASS_BLUR_RADIUS; offset++) {
					/* The neighbour, clamped to the image. */
					sx = (int32_t)x;
					sy = (int32_t)y;
					if (horizontal)
						sx += offset;
					else
						sy += offset;
					if (sx < 0)
						sx = 0;
					if (sy < 0)
						sy = 0;
					if (sx >= (int32_t)width)
						sx = (int32_t)width - 1;
					if (sy >= (int32_t)height)
						sy = (int32_t)height - 1;
					sum += pixels[((size_t)sy * width + (size_t)sx) * 3U + channel];
				}

				/* The average of the window. */
				scratch[((size_t)y * width + x) * 3U + channel] = sum / (float)(2U * GLASS_BLUR_RADIUS + 1U);
			}
		}
	}

	/* The result replaces the input. */
	memcpy(pixels, scratch, (size_t)width * height * 3U * sizeof(float));
}

/* Packs a color (0..1 each) as an opaque BGRA pixel. */
static uint32_t
pack_pixel(
	const float *rgb)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* Each channel rounded to eight bits. */
	red = (uint32_t)(rgb[0] * 255.0f + 0.5f);
	green = (uint32_t)(rgb[1] * 255.0f + 0.5f);
	blue = (uint32_t)(rgb[2] * 255.0f + 0.5f);

	/* A, R, G, B from the top byte (B, G, R, A in memory). */
	return 0xff000000U | (red << 16) | (green << 8) | blue;
}

/*
 * Makes the glyph atlas from the font.  Returns 0 with text available, or an
 * error with the look to be drawn without text.
 */
static int
atlas_create(
	struct zwl_server *server,
	struct zwl_glass *glass)
{
	struct truetype_face *face;
	void *data;
	size_t size;
	VkResult result;
	int error;

	/* The font file. */
	data = file_read(server->font_path, &size);
	if (data == NULL)
		return errno;
	error = truetype_open(data, size, 0U, &face);
	if (error != 0) {
		free(data);
		return EINVAL;
	}

	/* The atlas image, transparent where nothing is drawn. */
	result = zwl_host_image_create(server->compose, GLASS_ATLAS_WIDTH, GLASS_ATLAS_HEIGHT, server->compose->sampler, &glass->atlas);
	if (result != VK_SUCCESS) {
		truetype_close(face);
		free(data);
		return EIO;
	}

	/* The glyphs at each size. */
	error = atlas_fill(glass, face);
	truetype_close(face);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded. */
	glass->text = 1;
	return 0;
}

/* Renders every glyph at every size into the atlas, row by row. */
static int
atlas_fill(
	struct zwl_glass *glass,
	struct truetype_face *face)
{
	static uint8_t bitmap[64U * 64U];
	struct truetype_glyph metrics;
	struct glass_glyph *glyph;
	uint32_t *row;
	uint32_t codepoint;
	uint32_t pen_x;
	uint32_t pen_y;
	uint32_t line;
	uint32_t x;
	uint32_t y;
	uint32_t value;
	unsigned size;
	unsigned index;
	unsigned id;
	int error;

	/* The pen starts at the top left; each row is as tall as its tallest glyph. */
	memset(glass->atlas.map, 0, glass->atlas.row_pitch * GLASS_ATLAS_HEIGHT);
	pen_x = 0;
	pen_y = 0;
	line = 0;
	for (size = 0; size < GLASS_SIZES; size++) {
		error = truetype_set_pixel_size(face, glass_pixels[size]);
		if (error != 0)
			return EINVAL;

		/* Each glyph of this size. */
		for (index = 0; index < GLASS_GLYPHS; index++) {
			codepoint = 32U + index;
			if (index == GLASS_CLOSE_GLYPH)
				codepoint = 0xd7U;
			id = truetype_glyph_index(face, codepoint);
			error = truetype_glyph_metrics(face, id, &metrics);
			if (error != 0 || metrics.width > 64U || metrics.height > 64U)
				continue;

			/* A full row moves the pen down. */
			if (pen_x + metrics.width + 1U > GLASS_ATLAS_WIDTH) {
				pen_x = 0;
				pen_y += line + 1U;
				line = 0;
			}

			/* The atlas must hold it. */
			if (pen_y + metrics.height > GLASS_ATLAS_HEIGHT)
				return ENOSPC;

			/* The glyph's place and metrics. */
			glyph = &glass->glyphs[size][index];
			glyph->x = pen_x;
			glyph->y = pen_y;
			glyph->width = metrics.width;
			glyph->height = metrics.height;
			glyph->left = metrics.left;
			glyph->top = metrics.top;
			glyph->advance = metrics.advance;

			/* Its coverage as premultiplied white. */
			if (metrics.width != 0U && metrics.height != 0U) {
				memset(bitmap, 0, sizeof(bitmap));
				error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
				if (error != 0)
					continue;
				for (y = 0; y < metrics.height; y++) {
					row = (uint32_t *)((unsigned char *)glass->atlas.map + (pen_y + y) * glass->atlas.row_pitch);
					for (x = 0; x < metrics.width; x++) {
						value = bitmap[y * metrics.width + x];
						row[pen_x + x] = (value << 24) | (value << 16) | (value << 8) | value;
					}
				}
			}

			/* The pen moves past it. */
			pen_x += metrics.width + 1U;
			if (metrics.height > line)
				line = metrics.height;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Reads a whole file of up to GLASS_FILE_MAX bytes; NULL with errno set when it cannot. */
static void *
file_read(
	const char *path,
	size_t *size)
{
	unsigned char *data;
	ssize_t count;
	size_t length;
	int descriptor;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return NULL;

	/* Its bytes, up to GLASS_FILE_MAX. */
	data = malloc(GLASS_FILE_MAX);
	if (data == NULL) {
		close(descriptor);
		errno = ENOMEM;
		return NULL;
	}

	/* Read to the end or the bound. */
	length = 0;
	for (;;) {
		count = read(descriptor, data + length, GLASS_FILE_MAX - length);
		if (count <= 0)
			break;
		length += (size_t)count;
		if (length == GLASS_FILE_MAX)
			break;
	}

	/* The file is no longer needed. */
	close(descriptor);

	/* An empty or unreadable file has nothing to use. */
	if (length == 0) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* Succeeded. */
	*size = length;
	return data;
}

/*
 * Reads a binary PPM (P6, maximum 255) as the wallpaper, scaled to the
 * output by the nearest pixel.  Returns the colors (0..1, three per pixel),
 * or NULL with errno set.
 */
static float *
wallpaper_load(
	const char *path,
	uint32_t width,
	uint32_t height)
{
	const unsigned char *pixel;
	unsigned char *data;
	float *pixels;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t maximum;
	uint32_t x;
	uint32_t y;
	size_t size;
	size_t at;
	int error;

	/* The file, with the P6 magic. */
	data = file_read(path, &size);
	if (data == NULL)
		return NULL;
	if (size < 2U || data[0] != 'P' || data[1] != '6') {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* The width, the height and the maximum value. */
	at = 2U;
	error = ppm_number(data, size, &at, &source_width);
	if (error == 0)
		error = ppm_number(data, size, &at, &source_height);
	if (error == 0)
		error = ppm_number(data, size, &at, &maximum);
	if (error != 0 || maximum != 255U || source_width == 0U || source_height == 0U) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* One whitespace byte, then three bytes a pixel. */
	at++;
	if (at > size || (size - at) / 3U / source_width < source_height) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* The output's pixels, each from the nearest source pixel. */
	pixels = malloc((size_t)width * height * 3U * sizeof(float));
	if (pixels == NULL) {
		free(data);
		errno = ENOMEM;
		return NULL;
	}

	/* Each output pixel takes the source pixel it falls on. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			pixel = data + at + ((size_t)(y * source_height / height) * source_width + (size_t)(x * source_width / width)) * 3U;
			pixels[((size_t)y * width + x) * 3U] = (float)pixel[0] / 255.0f;
			pixels[((size_t)y * width + x) * 3U + 1U] = (float)pixel[1] / 255.0f;
			pixels[((size_t)y * width + x) * 3U + 2U] = (float)pixel[2] / 255.0f;
		}
	}

	/* Succeeded. */
	free(data);
	return pixels;
}

/* Reads one decimal number of a PPM header, after whitespace and comments. */
static int
ppm_number(
	const unsigned char *data,
	size_t size,
	size_t *at,
	uint32_t *number)
{
	uint32_t value;
	unsigned digits;

	/* Whitespace, and comments to the end of their line. */
	while (*at < size) {
		if (data[*at] == '#') {
			while (*at < size && data[*at] != '\n')
				(*at)++;
			continue;
		}

		/* The first byte that is not whitespace starts the number. */
		if (data[*at] != ' ' && data[*at] != '\t' && data[*at] != '\r' && data[*at] != '\n')
			break;
		(*at)++;
	}

	/* The digits, within a bound. */
	value = 0;
	digits = 0;
	while (*at < size && data[*at] >= '0' && data[*at] <= '9' && digits < 6U) {
		value = value * 10U + (uint32_t)(data[*at] - '0');
		(*at)++;
		digits++;
	}

	/* A number has at least one digit. */
	if (digits == 0U)
		return EINVAL;

	/* Succeeded. */
	*number = value;
	return 0;
}

/* Starts a shape over a box: the quad is the box, with no image and no color. */
static void
shape_init(
	struct glass_shape *shape,
	float x,
	float y,
	float width,
	float height)
{
	/* The quad and the box are the same until the caller widens the quad. */
	memset(shape, 0, sizeof(*shape));
	shape->quad[0] = x;
	shape->quad[1] = y;
	shape->quad[2] = width;
	shape->quad[3] = height;
	shape->box[0] = x;
	shape->box[1] = y;
	shape->box[2] = width;
	shape->box[3] = height;

	/* The whole image, not faded. */
	shape->uv[2] = 1.0f;
	shape->uv[3] = 1.0f;
	shape->opacity = 1.0f;
}

/* Records one shape: its constants for both shader stages and the strip. */
static void
shape_draw(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct glass_shape *shape)
{
	struct zwl_compose *compose;
	float constants[ZWL_PANEL_CONSTANTS];
	float width;
	float height;
	VkDescriptorSet set;

	/* The quad in normalized device coordinates. */
	compose = server->compose;
	width = (float)server->width;
	height = (float)server->height;
	constants[0] = 2.0f * shape->quad[0] / width - 1.0f;
	constants[1] = 2.0f * shape->quad[1] / height - 1.0f;
	constants[2] = 2.0f * (shape->quad[0] + shape->quad[2]) / width - 1.0f;
	constants[3] = 2.0f * (shape->quad[1] + shape->quad[3]) / height - 1.0f;

	/* The part of the image, the box, the color. */
	memcpy(&constants[4], shape->uv, sizeof(shape->uv));
	memcpy(&constants[8], shape->box, sizeof(shape->box));
	memcpy(&constants[12], shape->color, sizeof(shape->color));

	/* The shape and the output. */
	constants[16] = shape->radius;
	constants[17] = shape->mode;
	constants[18] = shape->soft;
	constants[19] = shape->opaque;
	constants[20] = width;
	constants[21] = height;
	constants[22] = shape->edge;
	constants[23] = shape->opacity;

	/* A shape without an image of its own is given the blurred wallpaper (it is not sampled). */
	set = shape->set;
	if (set == VK_NULL_HANDLE)
		set = compose->glass->blurred.set;

	/* The pipeline, the image and the strip. */
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_layout, 0U, 1U, &set, 0U, NULL);
	vkCmdPushConstants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	vkCmdDraw(command, 4U, 1U, 0U, 0U);
}

/* Draws a rounded rectangle in a solid color. */
static void
draw_solid(
	struct zwl_server *server,
	VkCommandBuffer command,
	float x,
	float y,
	float width,
	float height,
	float radius,
	const float *color)
{
	struct glass_shape shape;

	/* One solid shape. */
	shape_init(&shape, x, y, width, height);
	shape.mode = MODE_SOLID;
	shape.radius = radius;
	memcpy(shape.color, color, sizeof(shape.color));
	shape_draw(server, command, &shape);
}

/* The width of a line of text in pixels (0 without text). */
static int32_t
text_width(
	const struct zwl_glass *glass,
	enum glass_size size,
	const char *text)
{
	int32_t width;
	unsigned index;

	/* The sum of the advances of the glyphs the atlas has. */
	width = 0;
	if (!glass->text)
		return 0;
	for (; *text != '\0'; text++) {
		index = (unsigned char)*text - 32U;
		if (index < GLASS_CLOSE_GLYPH)
			width += glass->glyphs[size][index].advance;
	}

	/* Succeeded. */
	return width;
}

/*
 * Draws a line of text from x on a baseline, cut short (with an ellipsis of
 * dots) where it would pass x + limit.
 */
static void
draw_text(
	struct zwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t x,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	struct zwl_glass *glass;
	int32_t start;
	int32_t width;
	int32_t dots;
	unsigned index;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return;

	/* Room is kept for the ellipsis when the text is too long. */
	dots = 3 * glass->glyphs[size]['.' - 32].advance;
	width = text_width(glass, size, text);
	if (width <= limit)
		dots = 0;

	/* Each glyph the atlas has, while there is room. */
	start = x;
	for (; *text != '\0'; text++) {
		index = (unsigned char)*text - 32U;
		if (index >= GLASS_CLOSE_GLYPH)
			continue;
		if (dots != 0 && x + glass->glyphs[size][index].advance > start + limit - dots)
			break;
		draw_glyph(server, command, size, index, x, baseline, color);
		x += glass->glyphs[size][index].advance;
	}

	/* The ellipsis. */
	if (dots != 0) {
		for (index = 0; index < 3U; index++) {
			draw_glyph(server, command, size, '.' - 32, x, baseline, color);
			x += glass->glyphs[size]['.' - 32].advance;
		}
	}
}

/* Draws one glyph of the atlas with its origin at x on the baseline. */
static void
draw_glyph(
	struct zwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	unsigned index,
	int32_t x,
	int32_t baseline,
	const float *color)
{
	struct glass_shape shape;
	const struct glass_glyph *glyph;
	struct zwl_glass *glass;

	/* A space draws nothing. */
	glass = server->compose->glass;
	glyph = &glass->glyphs[size][index];
	if (glyph->width == 0U || glyph->height == 0U)
		return;

	/* The glyph's pixels, one to one with the atlas. */
	shape_init(&shape, (float)(x + glyph->left), (float)(baseline - glyph->top), (float)glyph->width, (float)glyph->height);
	shape.mode = MODE_TEXT;
	shape.uv[0] = (float)glyph->x / (float)GLASS_ATLAS_WIDTH;
	shape.uv[1] = (float)glyph->y / (float)GLASS_ATLAS_HEIGHT;
	shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_ATLAS_WIDTH;
	shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_ATLAS_HEIGHT;
	memcpy(shape.color, color, sizeof(shape.color));
	shape.set = glass->atlas.set;
	shape_draw(server, command, &shape);
}

/* Draws a window: the shadows, the body with rounded corners, and the title bar. */
static void
draw_window(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	unsigned focused)
{
	const struct zwl_import *image;
	struct glass_shape shape;
	int32_t width;
	int32_t height;
	float soft;

	/* The body's size is its image's. */
	image = zwl_compose_surface_image(surface);
	width = (int32_t)image->width;
	height = (int32_t)image->height;

	/* The body's shadow, deeper for the focused window. */
	soft = 22.0f;
	if (focused)
		soft = 30.0f;
	shape_init(&shape, (float)surface->x, (float)surface->y + 8.0f, (float)width, (float)height);
	shape.quad[0] -= 2.0f * soft;
	shape.quad[1] -= 2.0f * soft;
	shape.quad[2] += 4.0f * soft;
	shape.quad[3] += 4.0f * soft;
	shape.mode = MODE_SHADOW;
	shape.radius = GLASS_RADIUS;
	shape.soft = soft;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.20f;
	shape_draw(server, command, &shape);

	/* A see-through window's body lies on frosted glass. */
	if (server->window_opacity < 1.0f) {
		shape_init(&shape, (float)surface->x, (float)surface->y, (float)width, (float)height);
		shape.mode = MODE_GLASS;
		shape.radius = GLASS_RADIUS;
		shape.color[0] = 1.0f;
		shape.color[1] = 1.0f;
		shape.color[2] = 1.0f;
		shape.color[3] = 0.30f;
		shape.edge = 0.75f;
		shape_draw(server, command, &shape);
	}

	/* The body: the window's image with rounded corners, as opaque as asked. */
	shape_init(&shape, (float)surface->x, (float)surface->y, (float)width, (float)height);
	shape.opacity = server->window_opacity;
	shape.mode = MODE_IMAGE;
	shape.radius = GLASS_RADIUS;
	shape.set = image->set;
	if (image->draw == ZWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;
	shape_draw(server, command, &shape);

	/* The floating title bar. */
	draw_title_bar(server, command, surface, focused);
}

/*
 * Draws a window's title bar: a rounded glass panel above the body with a
 * gap, the application's mark and title on the left and the three buttons
 * on the right.
 */
static void
draw_title_bar(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	unsigned focused)
{
	static const float mark[4] = { 0.29f, 0.55f, 1.0f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float faint[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	struct glass_shape shape;
	const float *ink;
	char letter[2];
	const char *title;
	int32_t width;
	int32_t height;
	int32_t x;
	int32_t y;
	int button;

	/* The bar is as wide as the body, a gap above it. */
	window_size(surface, &width, &height);
	x = surface->x;
	y = surface->y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE;

	/* Its shadow. */
	shape_init(&shape, (float)x, (float)y + 4.0f, (float)width, (float)ZWL_GLASS_TITLE);
	shape.quad[0] -= 36.0f;
	shape.quad[1] -= 36.0f;
	shape.quad[2] += 72.0f;
	shape.quad[3] += 72.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = GLASS_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.12f;
	shape_draw(server, command, &shape);

	/* The glass, whiter for the focused window. */
	shape_init(&shape, (float)x, (float)y, (float)width, (float)ZWL_GLASS_TITLE);
	shape.mode = MODE_GLASS;
	shape.radius = GLASS_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.38f;
	if (focused)
		shape.color[3] = 0.55f;
	shape.edge = 0.85f;
	shape_draw(server, command, &shape);

	/* The application's mark: a blue rounded square with the title's first letter. */
	title = surface->title;
	if (title[0] == '\0')
		title = "Window";
	draw_solid(server, command, (float)(x + 14), (float)(y + 12), 20.0f, 20.0f, 6.0f, mark);
	letter[0] = title[0];
	letter[1] = '\0';
	if (letter[0] >= 'a' && letter[0] <= 'z')
		letter[0] = (char)(letter[0] - 'a' + 'A');
	draw_text(server, command, SIZE_BAR, x + 24 - text_width(server->compose->glass, SIZE_BAR, letter) / 2, y + 27, letter, 20, white);

	/* The title, darker for the focused window, cut short before the buttons. */
	ink = faint;
	if (focused)
		ink = dark;
	draw_text(server, command, SIZE_TITLE, x + 44, y + 28, title, width - 44 - BUTTON_SPACING * BUTTON_COUNT - 12, ink);

	/* The buttons. */
	for (button = 0; button < BUTTON_COUNT; button++)
		draw_button(server, command, surface, button, ink);
}

/*
 * Draws one title bar button: its background when the pointer is over it
 * (red for close), and its sign.
 */
static void
draw_button(
	struct zwl_server *server,
	VkCommandBuffer command,
	struct zwl_object *surface,
	int button,
	const float *ink)
{
	static const float hover[4] = { 1.0f, 1.0f, 1.0f, 0.70f };
	static const float danger[4] = { 0.91f, 0.30f, 0.28f, 0.95f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	struct glass_shape shape;
	struct zwl_glass *glass;
	const float *sign;
	int32_t cx;
	int32_t cy;
	int over;

	/* The background under the pointer. */
	button_centre(surface, button, &cx, &cy);
	sign = ink;
	over = button_at(surface, server->pointer_x, server->pointer_y);
	if (over == button && button == BUTTON_CLOSE) {
		draw_solid(server, command, (float)(cx - BUTTON_WIDTH / 2), (float)(cy - BUTTON_HEIGHT / 2), (float)BUTTON_WIDTH, (float)BUTTON_HEIGHT, 8.0f, danger);
		sign = white;
	} else if (over == button) {
		draw_solid(server, command, (float)(cx - BUTTON_WIDTH / 2), (float)(cy - BUTTON_HEIGHT / 2), (float)BUTTON_WIDTH, (float)BUTTON_HEIGHT, 8.0f, hover);
	}

	/* Minimize: a short line. */
	if (button == BUTTON_MINIMIZE) {
		draw_solid(server, command, (float)(cx - 6), (float)cy - 0.75f, 12.0f, 1.5f, 0.75f, sign);
		return;
	}

	/* Maximize: a small rounded square outline. */
	if (button == BUTTON_MAXIMIZE) {
		shape_init(&shape, (float)(cx - 5), (float)(cy - 5), 10.0f, 10.0f);
		shape.quad[0] -= 1.0f;
		shape.quad[1] -= 1.0f;
		shape.quad[2] += 2.0f;
		shape.quad[3] += 2.0f;
		shape.mode = MODE_RING;
		shape.radius = 2.5f;
		shape.soft = 1.4f;
		memcpy(shape.color, sign, sizeof(shape.color));
		shape_draw(server, command, &shape);
		return;
	}

	/* Close: the multiplication sign, centred. */
	glass = server->compose->glass;
	if (!glass->text)
		return;
	draw_glyph(server, command, SIZE_SIGN, GLASS_CLOSE_GLYPH, cx - glass->glyphs[SIZE_SIGN][GLASS_CLOSE_GLYPH].advance / 2, cy + 7, sign);
}

/*
 * Draws the system bar (a mock-up): a glass strip along the top with the
 * launcher and system menu on the left and the notification area on the
 * right (signal, battery, date and time).
 */
static void
draw_system_bar(
	struct zwl_server *server,
	VkCommandBuffer command)
{
	static const float blue[4] = { 0.25f, 0.52f, 0.98f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float line[4] = { 1.0f, 1.0f, 1.0f, 0.55f };
	struct glass_shape shape;
	struct tm local;
	time_t now;
	char clock[48];
	int32_t right;
	int32_t x;
	int bar;

	/* The strip, with a light line under it. */
	shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)ZWL_GLASS_BAR);
	shape.mode = MODE_GLASS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.55f;
	shape_draw(server, command, &shape);
	draw_solid(server, command, 0.0f, (float)(ZWL_GLASS_BAR - 1), (float)server->width, 1.0f, 0.0f, line);

	/* The launcher: a blue rounded square with four small squares. */
	draw_solid(server, command, 12.0f, 6.0f, 22.0f, 22.0f, 6.0f, blue);
	draw_solid(server, command, 17.0f, 11.0f, 5.0f, 5.0f, 1.5f, white);
	draw_solid(server, command, 24.0f, 11.0f, 5.0f, 5.0f, 1.5f, white);
	draw_solid(server, command, 17.0f, 18.0f, 5.0f, 5.0f, 1.5f, white);
	draw_solid(server, command, 24.0f, 18.0f, 5.0f, 5.0f, 1.5f, white);

	/* The system menu. */
	draw_text(server, command, SIZE_BAR, 44, 22, "zedBSD", 200, dark);

	/* The date and time at the right edge. */
	now = time(NULL);
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&now, &local);
	clock[0] = '\0';
	(void)strftime(clock, sizeof(clock), "%a %b %e  %H:%M", &local);
	right = (int32_t)server->width - 16;
	x = right - text_width(server->compose->glass, SIZE_BAR, clock);
	draw_text(server, command, SIZE_BAR, x, 22, clock, 400, dark);

	/* The battery: an outline, its charge and its terminal. */
	x -= 44;
	shape_init(&shape, (float)x, 11.0f, 22.0f, 12.0f);
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = 3.5f;
	shape.soft = 1.3f;
	memcpy(shape.color, dark, sizeof(shape.color));
	shape_draw(server, command, &shape);
	draw_solid(server, command, (float)(x + 3), 14.0f, 14.0f, 6.0f, 1.5f, dark);
	draw_solid(server, command, (float)(x + 23), 15.0f, 2.0f, 4.0f, 1.0f, dark);

	/* The signal: four bars of rising height. */
	x -= 36;
	for (bar = 0; bar < 4; bar++)
		draw_solid(server, command, (float)(x + bar * 5), (float)(23 - 4 - bar * 3), 3.0f, (float)(4 + bar * 3), 1.0f, dark);
}

/* The size of a window's body: its current image's. */
static void
window_size(
	const struct zwl_object *surface,
	int32_t *width,
	int32_t *height)
{
	uint32_t buffer_width;
	uint32_t buffer_height;

	/* No image, no size. */
	*width = 0;
	*height = 0;
	if (surface->current == NULL)
		return;

	/* The buffer's. */
	zwl_buffer_size(surface->current, &buffer_width, &buffer_height);
	*width = (int32_t)buffer_width;
	*height = (int32_t)buffer_height;
}

/* Tells whether a point is on a window's title bar, its body, or neither (the gap is neither). */
static enum glass_hit
window_hit(
	const struct zwl_object *surface,
	int32_t x,
	int32_t y)
{
	int32_t width;
	int32_t height;
	int32_t top;

	/* Outside the window's columns. */
	window_size(surface, &width, &height);
	if (x < surface->x || x >= surface->x + width)
		return HIT_NONE;

	/* The body. */
	if (y >= surface->y && y < surface->y + height)
		return HIT_BODY;

	/* The title bar. */
	top = surface->y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE;
	if (y >= top && y < top + ZWL_GLASS_TITLE)
		return HIT_TITLE;

	/* Neither. */
	return HIT_NONE;
}

/* Returns the title bar button at a point, or -1. */
static int
button_at(
	const struct zwl_object *surface,
	int32_t x,
	int32_t y)
{
	int32_t cx;
	int32_t cy;
	int button;

	/* Each button's box around its centre. */
	for (button = 0; button < BUTTON_COUNT; button++) {
		button_centre(surface, button, &cx, &cy);
		if (x >= cx - BUTTON_WIDTH / 2 && x < cx + BUTTON_WIDTH / 2 &&
		    y >= cy - BUTTON_HEIGHT / 2 && y < cy + BUTTON_HEIGHT / 2)
			return button;
	}

	/* None. */
	return -1;
}

/* The centre of a title bar button (counted from the right edge). */
static void
button_centre(
	const struct zwl_object *surface,
	int button,
	int32_t *x,
	int32_t *y)
{
	int32_t width;
	int32_t height;

	/* From the bar's right edge, in the middle of its height. */
	window_size(surface, &width, &height);
	*x = surface->x + width - 26 - button * BUTTON_SPACING;
	*y = surface->y - ZWL_GLASS_GAP - ZWL_GLASS_TITLE / 2;
}

/* Finds the topmost window whose title bar or body is at a point. */
static struct zwl_object *
window_at(
	struct zwl_server *server,
	int32_t x,
	int32_t y,
	enum glass_hit *hit)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	struct zwl_object *found;
	enum glass_hit place;

	/* The hit with the highest map order. */
	found = NULL;
	*hit = HIT_NONE;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only mapped windows. */
			if (surface->kind != ZWL_SURFACE ||
			    surface->dead ||
			    !surface->mapped ||
			    surface->role == NULL ||
			    surface->cursor_role)
				continue;

			/* Above what was found so far. */
			place = window_hit(surface, x, y);
			if (place == HIT_NONE)
				continue;
			if (found != NULL && surface->map_order < found->map_order)
				continue;
			found = surface;
			*hit = place;
		}
	}

	/* Succeeded: the window, or NULL. */
	return found;
}

/* Brings a window to the top and gives it the focus. */
static void
window_raise(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct zwl_object *top;

	/* Already on top. */
	top = zwl_top_window(server);
	if (surface == top)
		return;

	/* The highest map order, and the focus follows. */
	server->map_order++;
	surface->map_order = server->map_order;
	server->front_surface = surface;
	zwl_seat_focus(server);
	server->dirty = 1;
}

/*
 * Maximizes a window to the space under the system bar, or gives it back
 * its place and size.  The client is told the size by a configure.
 */
static void
window_maximize(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	int32_t width;
	int32_t height;
	int error;

	/* Back to the place and size it had. */
	if (surface->maximized) {
		surface->maximized = 0;
		surface->x = surface->restore_x;
		surface->y = surface->restore_y;
		surface->window_width = surface->restore_width;
		surface->window_height = surface->restore_height;
	} else {
		/* The place and size to go back to. */
		window_size(surface, &width, &height);
		surface->restore_x = surface->x;
		surface->restore_y = surface->y;
		surface->restore_width = (uint32_t)width;
		surface->restore_height = (uint32_t)height;

		/* The whole space under the system bar and the title bar. */
		surface->maximized = 1;
		surface->x = ZWL_GLASS_MARGIN;
		surface->y = ZWL_GLASS_TOP;
		surface->window_width = server->width - 2U * ZWL_GLASS_MARGIN;
		surface->window_height = server->height - ZWL_GLASS_TOP - ZWL_GLASS_MARGIN;
	}

	/* The client draws the new size. */
	server->dirty = 1;
	error = zwl_window_send_configure(surface);
	if (error != 0)
		printf("ZWL GLASS configure errno=%d\n", error);
}
