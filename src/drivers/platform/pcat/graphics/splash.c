/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot splash's spinner.
 *
 * On a quiet boot (kmsg=quiet) the loaders draw the boot splash in the
 * middle of the screen over black bars (tools/build/make-boot-splash.py:
 * the picture 1920x1080, "fit=contain", without its spinner; ws035-p112).  The spinner is drawn here instead, eight blue dots
 * in a ring whose brightness turns one step every SPLASH_STEP_MS of the HAL's
 * monotonic counter (BUG-193: stepping once per log line turned it fast while
 * the boot wrote many lines and slowly later).  It is drawn when the boot
 * writes a line of its log that nobody sees -- the HAL's early console
 * (cons.c) until the kernel's console is up, then the kernel's quiet text
 * console (text.c) -- and only when the time has moved it on, so it stands
 * still while the boot writes nothing.  Before the counter can be read (early
 * in the HAL, or on a HAL without one) each line turns it one step, as before.
 *
 * Where the spinner is comes from the splash's layout, which the build
 * script and this file share: the picture is at its own size, or shrunk
 * to fit the screen, with its middle on the middle; in the picture's coordinates the spinner's centre
 * is at the middle of the width and SPLASH_CENTRE_Y below the middle of the
 * height, the ring's radius SPLASH_RING and a dot's radius SPLASH_DOT, all
 * in ten-thousandths of the picture's height.
 *
 * The picture under the spinner is kept when the spinner starts, and each
 * frame is drawn over it, so the dots are drawn over the splash and never
 * over themselves.  The spinner stops when the console is shown (its clear
 * takes the splash away).  The callers serialize (the HAL's console lock,
 * then the text console's lock).
 */

#include "splash.h"

#include <kern/clock.h>
#include <stdbool.h>
#include <stddef.h>

/* The splash's layout, in ten-thousandths of the picture's height (see make-boot-splash.py). */
#define SPLASH_CENTRE_Y		2720
#define SPLASH_RING		308
#define SPLASH_DOT		64

/* The picture's size (make-boot-splash.py); the loaders never enlarge it. */
#define SPLASH_PICTURE_WIDTH	1920U
#define SPLASH_PICTURE_HEIGHT	1080U

/* The picture height the spinner's size stops growing at, so its square fits the kept picture. */
#define SPLASH_SIZE_HEIGHT_MAX	1200U

/* The largest square the spinner is drawn in, in pixels a side. */
#define SPLASH_PATCH_MAX	96U

/* How long the spinner shows each step, in milliseconds: one turn of the ring a second. */
#define SPLASH_STEP_MS		125U

/* How many dots, and the fixed point of positions (sixteenths of a pixel). */
#define SPLASH_DOTS		8U
#define SPLASH_SUBPIXEL		16

/*
 * The spinner being shown: the framebuffer, where the spinner's square is
 * on it, the spinner's centre, ring and dot radii (in sixteenths of a
 * pixel, from the square's corner), and the frame shown.  active is zero
 * before the spinner starts and after it stops, and then nothing is drawn.
 * timed is nonzero once the monotonic counter was read: the frame is then
 * the time since time_start in steps of step_counts, and frame_base is the
 * frame shown at that moment, so the turn goes on from where the per-line
 * steps left it.
 */
struct splash_spinner {
	volatile uint32_t *pixels;
	unsigned stride;
	int rgbx;
	int active;
	unsigned patch_x;
	unsigned patch_y;
	unsigned patch_side;
	int centre_x;
	int centre_y;
	int ring;
	int dot;
	unsigned frame;
	int timed;
	uint64_t time_start;
	uint64_t step_counts;
	unsigned frame_base;
};

/*
 * The spinner.  It lives for the whole boot; it is filled once by
 * drv_pcat_splash_start and changed only by the calls below.
 */
static struct splash_spinner splash;

/*
 * The splash's pixels under the spinner's square, as they were when the
 * spinner started, row by row (patch_side a row).  Each frame starts from
 * them.
 */
static uint32_t splash_under[SPLASH_PATCH_MAX * SPLASH_PATCH_MAX];

/* The dots' directions from the centre, clockwise from the top, in 1/1024. */
static const int splash_direction[SPLASH_DOTS][2] = {
	{ 0, -1024 },
	{ 724, -724 },
	{ 1024, 0 },
	{ 724, 724 },
	{ 0, 1024 },
	{ -724, 724 },
	{ -1024, 0 },
	{ -724, -724 }
};

/* The dots' colours by how far behind the leading dot they are (red, green, blue), as in the splash. */
static const uint8_t splash_colour[SPLASH_DOTS][3] = {
	{ 64, 138, 250 },
	{ 94, 164, 253 },
	{ 128, 183, 253 },
	{ 164, 204, 252 },
	{ 192, 218, 248 },
	{ 210, 226, 246 },
	{ 214, 228, 246 },
	{ 214, 228, 246 }
};

static void splash_draw(void);
static int splash_timed_frame(unsigned *frame);
static void splash_draw_dot(unsigned index, const uint8_t *colour);
static uint32_t splash_blend(uint32_t under, const uint8_t *colour, unsigned coverage);

/*
 * Starts the spinner on a framebuffer that shows the splash: works out
 * where it is, keeps the picture under it and draws the first frame.
 */
void
drv_pcat_splash_start(
	volatile uint32_t *pixels,
	unsigned width,
	unsigned height,
	unsigned stride,
	int rgbx)
{
	unsigned picture_height;
	unsigned size_height;
	unsigned reach;
	unsigned side;
	unsigned centre_x;
	unsigned centre_y;
	unsigned x;
	unsigned y;

	/* No framebuffer, no spinner. */
	splash.active = 0;
	if (pixels == 0 || width == 0U || height == 0U || stride < width)
		return;

	/*
	 * The picture's height on the screen: its own when the screen holds
	 * it, else shrunk to the screen's height when the screen is the wider
	 * in proportion, else to the width's (as the loaders draw it).
	 */
	picture_height = SPLASH_PICTURE_HEIGHT;
	if (width < SPLASH_PICTURE_WIDTH || height < SPLASH_PICTURE_HEIGHT) {
		if ((uint64_t)width * SPLASH_PICTURE_HEIGHT >= (uint64_t)height * SPLASH_PICTURE_WIDTH)
			picture_height = height;
		else
			picture_height = SPLASH_PICTURE_HEIGHT * width / SPLASH_PICTURE_WIDTH;
	}

	/* The spinner's size follows the picture up to a height, so that its square fits. */
	size_height = picture_height;
	if (size_height > SPLASH_SIZE_HEIGHT_MAX)
		size_height = SPLASH_SIZE_HEIGHT_MAX;
	splash.ring = (int)(size_height * SPLASH_RING * SPLASH_SUBPIXEL / 10000U);
	splash.dot = (int)(size_height * SPLASH_DOT * SPLASH_SUBPIXEL / 10000U);
	if (splash.dot < 2 * SPLASH_SUBPIXEL)
		splash.dot = 2 * SPLASH_SUBPIXEL;

	/* Its square: the ring and a dot on each side, and two pixels of margin. */
	reach = (unsigned)((splash.ring + splash.dot) / SPLASH_SUBPIXEL) + 2U;
	side = 2U * reach;
	centre_x = width / 2U;
	centre_y = height / 2U + picture_height * SPLASH_CENTRE_Y / 10000U;

	/* A square that does not fit the kept picture or the screen is not drawn. */
	if (side > SPLASH_PATCH_MAX || centre_x < reach || centre_y < reach)
		return;
	if (centre_x + reach > width || centre_y + reach > height)
		return;

	/* Where it is. */
	splash.pixels = pixels;
	splash.stride = stride;
	splash.rgbx = rgbx;
	splash.patch_x = centre_x - reach;
	splash.patch_y = centre_y - reach;
	splash.patch_side = side;
	splash.centre_x = (int)reach * SPLASH_SUBPIXEL;
	splash.centre_y = (int)reach * SPLASH_SUBPIXEL;
	splash.frame = 0U;
	splash.timed = 0;

	/* The picture under it, kept. */
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++)
			splash_under[y * side + x] = pixels[(splash.patch_y + y) * stride + splash.patch_x + x];
	}

	/* Succeeded: the first frame is shown. */
	splash.active = 1;
	splash_draw();
}

/*
 * Moves the spinner to another mapping of the same framebuffer (the
 * kernel's own, once the text console takes over from the HAL's).
 */
void
drv_pcat_splash_retarget(
	volatile uint32_t *pixels)
{
	/* Only a spinner that is shown, onto a mapping there is. */
	if (!splash.active || pixels == 0)
		return;

	/* The same framebuffer seen through another address. */
	splash.pixels = pixels;
}

/*
 * Shows the spinner's frame for the time now: the boot has written another
 * line.
 */
void
drv_pcat_splash_step(
	void)
{
	unsigned frame;
	int timed;

	/* Nothing when the spinner is not shown. */
	if (!splash.active)
		return;

	/* The frame the time gives; without the counter, the next frame. */
	timed = splash_timed_frame(&frame);
	if (!timed)
		frame = splash.frame + 1U;

	/* Draws only a frame that differs from the one shown. */
	if (frame == splash.frame)
		return;
	splash.frame = frame;
	splash_draw();
}

/*
 * Stops the spinner for good (the console is shown over the splash).
 */
void
drv_pcat_splash_stop(
	void)
{
	/* Nothing is drawn any more. */
	splash.active = 0;
}

/*
 * Finds the frame for the time now from the kernel's monotonic counter.
 * Reports 0 when the counter cannot be read yet, and the caller steps once
 * per line instead.
 */
static int
splash_timed_frame(
	unsigned *frame)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t steps;
	bool available;

	/* Reads the counter, which may not run yet this early. */
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available)
		return 0;

	/* The first reading starts the clock at the frame shown; the frequency stays for the boot. */
	if (!splash.timed) {
		splash.step_counts = frequency * SPLASH_STEP_MS / 1000U;
		if (splash.step_counts == 0U)
			splash.step_counts = 1U;
		splash.time_start = counter;
		splash.frame_base = splash.frame;
		splash.timed = 1;
	}

	/* Reports the frame shown when the clock started plus the whole steps since. */
	steps = (counter - splash.time_start) / splash.step_counts;
	*frame = splash.frame_base + (unsigned)steps;
	return 1;
}

/* Draws the frame: the kept picture, then each dot in its colour for the frame. */
static void
splash_draw(
	void)
{
	unsigned leading;
	unsigned behind;
	unsigned index;
	unsigned x;
	unsigned y;

	/* The picture under the square. */
	for (y = 0; y < splash.patch_side; y++) {
		for (x = 0; x < splash.patch_side; x++)
			splash.pixels[(splash.patch_y + y) * splash.stride + splash.patch_x + x] = splash_under[y * splash.patch_side + x];
	}

	/* The leading dot turns clockwise; the others fade behind it. */
	leading = splash.frame % SPLASH_DOTS;
	for (index = 0; index < SPLASH_DOTS; index++) {
		behind = (leading + SPLASH_DOTS - index) % SPLASH_DOTS;
		splash_draw_dot(index, splash_colour[behind]);
	}
}

/* Draws one dot in a colour, its edge smoothed by sixteen samples a pixel. */
static void
splash_draw_dot(
	unsigned index,
	const uint8_t *colour)
{
	unsigned coverage;
	unsigned first_x;
	unsigned first_y;
	unsigned last_x;
	unsigned last_y;
	unsigned x;
	unsigned y;
	unsigned sample;
	int dot_x;
	int dot_y;
	int dx;
	int dy;
	int radius_squared;
	size_t at;

	/* The dot's centre in the square, and the pixels it can touch. */
	dot_x = splash.centre_x + splash.ring * splash_direction[index][0] / 1024;
	dot_y = splash.centre_y + splash.ring * splash_direction[index][1] / 1024;
	radius_squared = splash.dot * splash.dot;
	first_x = (unsigned)((dot_x - splash.dot) / SPLASH_SUBPIXEL);
	first_y = (unsigned)((dot_y - splash.dot) / SPLASH_SUBPIXEL);
	last_x = (unsigned)((dot_x + splash.dot) / SPLASH_SUBPIXEL) + 1U;
	last_y = (unsigned)((dot_y + splash.dot) / SPLASH_SUBPIXEL) + 1U;
	if (last_x >= splash.patch_side)
		last_x = splash.patch_side - 1U;
	if (last_y >= splash.patch_side)
		last_y = splash.patch_side - 1U;

	/* Each pixel: how many of its sixteen samples are inside the dot. */
	for (y = first_y; y <= last_y; y++) {
		for (x = first_x; x <= last_x; x++) {
			coverage = 0U;
			for (sample = 0; sample < 16U; sample++) {
				dx = (int)x * SPLASH_SUBPIXEL + 2 + 4 * (int)(sample % 4U) - dot_x;
				dy = (int)y * SPLASH_SUBPIXEL + 2 + 4 * (int)(sample / 4U) - dot_y;
				if (dx * dx + dy * dy <= radius_squared)
					coverage++;
			}

			/* A pixel the dot misses keeps the picture. */
			if (coverage == 0U)
				continue;

			/* The dot's colour over the picture, as much as it covers. */
			at = (size_t)(splash.patch_y + y) * splash.stride + splash.patch_x + x;
			splash.pixels[at] = splash_blend(splash_under[y * splash.patch_side + x], colour, coverage);
		}
	}
}

/* Mixes a colour over a framebuffer pixel by a coverage of sixteenths, in the framebuffer's layout. */
static uint32_t
splash_blend(
	uint32_t under,
	const uint8_t *colour,
	unsigned coverage)
{
	unsigned red;
	unsigned green;
	unsigned blue;

	/* The pixel's channels (RGBX keeps red in the low byte, BGRX blue). */
	green = (under >> 8) & 255U;
	if (splash.rgbx) {
		red = under & 255U;
		blue = (under >> 16) & 255U;
	} else {
		blue = under & 255U;
		red = (under >> 16) & 255U;
	}

	/* Each channel moved towards the colour. */
	red = (red * (16U - coverage) + colour[0] * coverage) / 16U;
	green = (green * (16U - coverage) + colour[1] * coverage) / 16U;
	blue = (blue * (16U - coverage) + colour[2] * coverage) / 16U;

	/* Packed again in the same layout. */
	if (splash.rgbx)
		return red | (green << 8) | (blue << 16);

	/* BGRX. */
	return blue | (green << 8) | (red << 16);
}
