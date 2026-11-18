/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot logo.
 *
 * zedbsd.cfg names the logo with logo=PATH, a path relative to the boot
 * volume like kernel=.  The file is a binary PPM (P6, maximum value 255, at
 * most 4096 pixels a side).  The loader fills the screen with the colour of
 * the logo's top-left pixel and draws the logo in the middle, so a logo
 * with a plain background reads as one picture on any screen size.  The
 * kernel does not know logo=; it ignores it as an unknown name.
 *
 * A picture whose header has the comment "fit=contain" (the boot
 * splash, 1920x1080, tools/build/make-boot-splash.py) is drawn
 * whole in the middle of a black screen: at its own size when the screen
 * holds it, else shrunk bilinearly, its proportions kept, to the largest
 * size the screen holds.  The sides it does not reach stay black bars.
 */

#include "logo.h"

/* The largest logo side the loader draws. */
#define LOGO_SIDE_MAX	4096U

/* The header comment that asks for a whole picture over black bars. */
#define LOGO_CONTAIN_WORD	"fit=contain"

static int logo_header_number(const uint8_t *data, size_t size, size_t *at, uint32_t *number, int *contain);
static void logo_skip_space(const uint8_t *data, size_t size, size_t *at, int *contain);
static int logo_comment_contain(const uint8_t *data, size_t size, size_t at);
static void logo_draw_contain(const uint8_t *image, uint32_t width, uint32_t height, const struct zbl6_framebuffer *framebuffer, volatile uint32_t *pixels);
static uint32_t logo_sample(const uint8_t *image, uint32_t width, uint32_t height, uint64_t fx, uint64_t fy, const struct zbl6_framebuffer *framebuffer);
static uint32_t logo_pixel(const struct zbl6_framebuffer *framebuffer, const uint8_t *rgb);

/*
 * Draws a binary PPM in the middle of the framebuffer over its top-left
 * pixel's colour.
 *
 * Returns 1 when the logo was drawn, 0 when the file is not a PPM the
 * loader takes (the screen is then left as it was).
 */
int
zbl_uefi_logo_draw(
	const uint8_t *data,
	size_t size,
	const struct zbl6_framebuffer *framebuffer)
{
	volatile uint32_t *pixels;
	const uint8_t *image;
	uint32_t width;
	uint32_t height;
	uint32_t maximum;
	uint32_t background;
	uint32_t origin_x;
	uint32_t origin_y;
	uint32_t shown_width;
	uint32_t shown_height;
	uint32_t skip_x;
	uint32_t skip_y;
	uint32_t x;
	uint32_t y;
	size_t at;
	int contain;
	int error;

	/* The magic: P6. */
	if (data == NULL ||
	    framebuffer == NULL ||
	    size < 2U ||
	    data[0] != 'P' ||
	    data[1] != '6')
		return 0;

	/* The width, the height and the maximum value, which must be 255 (a comment may ask for bars). */
	at = 2U;
	contain = 0;
	error = logo_header_number(data, size, &at, &width, &contain);
	if (error != 0)
		return 0;

	/* The height. */
	error = logo_header_number(data, size, &at, &height, &contain);
	if (error != 0)
		return 0;

	/* The maximum value, which must be 255. */
	error = logo_header_number(data, size, &at, &maximum, &contain);
	if (error != 0 || maximum != 255U)
		return 0;

	/* Refuses an empty logo and one larger than the loader draws. */
	if (width == 0U ||
	    height == 0U ||
	    width > LOGO_SIDE_MAX ||
	    height > LOGO_SIDE_MAX)
		return 0;

	/* One whitespace byte, then the pixels, all of them. */
	at++;
	if (at > size || (size_t)width * height * 3U > size - at)
		return 0;

	/* The pixels start there. */
	image = data + at;

	/* A framebuffer the loader can write whole pixels to. */
	if (framebuffer->physical_base == 0U ||
	    framebuffer->width == 0U ||
	    framebuffer->height == 0U)
		return 0;

	/* Views the framebuffer as pixels. */
	pixels = (volatile uint32_t *)(uintptr_t)framebuffer->physical_base;

	/* A picture over black bars is drawn whole, its stores made visible before the kernel. */
	if (contain) {
		logo_draw_contain(image, width, height, framebuffer, pixels);
		__asm__ volatile("sfence" : : : "memory");
		return 1;
	}

	/* The screen in the logo's background colour. */
	background = logo_pixel(framebuffer, image);
	for (y = 0; y < framebuffer->height; y++) {
		for (x = 0; x < framebuffer->width; x++)
			pixels[(uint64_t)y * framebuffer->stride + x] = background;
	}

	/* The logo in the middle, cut to the screen when it is larger. */
	shown_width = width;
	shown_height = height;
	skip_x = 0U;
	skip_y = 0U;
	if (shown_width > framebuffer->width) {
		skip_x = (shown_width - framebuffer->width) / 2U;
		shown_width = framebuffer->width;
	}

	/* The same for its height. */
	if (shown_height > framebuffer->height) {
		skip_y = (shown_height - framebuffer->height) / 2U;
		shown_height = framebuffer->height;
	}

	/* Its pixels, from its top-left corner in the middle of the screen. */
	origin_x = (framebuffer->width - shown_width) / 2U;
	origin_y = (framebuffer->height - shown_height) / 2U;
	for (y = 0; y < shown_height; y++) {
		for (x = 0; x < shown_width; x++) {
			pixels[(uint64_t)(origin_y + y) * framebuffer->stride + origin_x + x] =
			    logo_pixel(framebuffer, image + ((size_t)(y + skip_y) * width + x + skip_x) * 3U);
		}
	}

	/* Makes the framebuffer stores visible before the kernel takes over. */
	__asm__ volatile("sfence" : : : "memory");

	/* Succeeded: the logo is on the screen. */
	return 1;
}

/*
 * Draws a whole picture in the middle of a black framebuffer, at its own
 * size when the screen holds it, else shrunk to the largest size the
 * screen holds with its proportions kept.
 */
static void
logo_draw_contain(
	const uint8_t *image,
	uint32_t width,
	uint32_t height,
	const struct zbl6_framebuffer *framebuffer,
	volatile uint32_t *pixels)
{
	uint64_t scaled_width;
	uint64_t scaled_height;
	uint64_t origin_x;
	uint64_t origin_y;
	uint64_t fx;
	uint64_t fy;
	uint32_t x;
	uint32_t y;

	/*
	 * The picture's size on the screen: its own when it fits, else as
	 * tall as the screen when the screen is the wider of the two in
	 * proportion, else as wide.
	 */
	scaled_width = width;
	scaled_height = height;
	if (width > framebuffer->width || height > framebuffer->height) {
		if ((uint64_t)framebuffer->width * height >= (uint64_t)framebuffer->height * width) {
			scaled_height = framebuffer->height;
			scaled_width = (uint64_t)width * framebuffer->height / height;
		} else {
			scaled_width = framebuffer->width;
			scaled_height = (uint64_t)height * framebuffer->width / width;
		}
	}

	/* Where its top-left corner is: the bars on each side are as wide as each other. */
	origin_x = (framebuffer->width - scaled_width) / 2U;
	origin_y = (framebuffer->height - scaled_height) / 2U;

	/* Each screen pixel is black outside the picture, else samples it where its centre falls (16.16). */
	for (y = 0; y < framebuffer->height; y++) {
		/* A row above or below the picture is a bar. */
		if (y < origin_y || y - origin_y >= scaled_height) {
			for (x = 0; x < framebuffer->width; x++)
				pixels[(uint64_t)y * framebuffer->stride + x] = 0U;
			continue;
		}

		/* A row of the picture between the bars on the left and right. */
		fy = (((uint64_t)(y - origin_y) * 2U + 1U) * height << 16) / (scaled_height * 2U);
		for (x = 0; x < framebuffer->width; x++) {
			/* A pixel left or right of the picture is a bar. */
			if (x < origin_x || x - origin_x >= scaled_width) {
				pixels[(uint64_t)y * framebuffer->stride + x] = 0U;
				continue;
			}

			/* A pixel of the picture, sampled where its centre falls. */
			fx = (((uint64_t)(x - origin_x) * 2U + 1U) * width << 16) / (scaled_width * 2U);
			pixels[(uint64_t)y * framebuffer->stride + x] = logo_sample(image, width, height, fx, fy, framebuffer);
		}
	}
}

/*
 * Samples a picture bilinearly at a place in 16.16 fixed point (pixel
 * centres at half units), in the framebuffer's pixel layout.
 */
static uint32_t
logo_sample(
	const uint8_t *image,
	uint32_t width,
	uint32_t height,
	uint64_t fx,
	uint64_t fy,
	const struct zbl6_framebuffer *framebuffer)
{
	const uint8_t *row0;
	const uint8_t *row1;
	uint8_t rgb[3];
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t wx;
	uint32_t wy;
	uint32_t top;
	uint32_t bottom;
	uint32_t pixel;
	unsigned channel;

	/* From the half-unit centres to the pixels' corners; the first half pixel keeps the edge. */
	if (fx > 32768U)
		fx -= 32768U;
	else
		fx = 0U;

	/* The same for the rows. */
	if (fy > 32768U)
		fy -= 32768U;
	else
		fy = 0U;

	/* The pixel at or left of the place, its right neighbour, and how far towards it (0..255). */
	x0 = (uint32_t)(fx >> 16);
	wx = (uint32_t)(fx & 0xffffU) >> 8;
	x1 = x0 + 1U;
	if (x0 >= width - 1U) {
		x0 = width - 1U;
		x1 = x0;
		wx = 0U;
	}

	/* The row at or above the place, the one below it, and how far towards it. */
	y0 = (uint32_t)(fy >> 16);
	wy = (uint32_t)(fy & 0xffffU) >> 8;
	y1 = y0 + 1U;
	if (y0 >= height - 1U) {
		y0 = height - 1U;
		y1 = y0;
		wy = 0U;
	}

	/* The two rows sampled. */
	row0 = image + (size_t)y0 * width * 3U;
	row1 = image + (size_t)y1 * width * 3U;

	/* Each channel, mixed along the rows and then between them. */
	for (channel = 0; channel < 3U; channel++) {
		top = row0[x0 * 3U + channel] * (256U - wx) + row0[x1 * 3U + channel] * wx;
		bottom = row1[x0 * 3U + channel] * (256U - wx) + row1[x1 * 3U + channel] * wx;
		rgb[channel] = (uint8_t)((top * (256U - wy) + bottom * wy + 32768U) >> 16);
	}

	/* Packs the mixed pixel in the framebuffer's layout. */
	pixel = logo_pixel(framebuffer, rgb);

	/* Reports the mixed pixel. */
	return pixel;
}

/* Reads one decimal number of a PPM header after whitespace and comments. */
static int
logo_header_number(
	const uint8_t *data,
	size_t size,
	size_t *at,
	uint32_t *number,
	int *contain)
{
	uint32_t value;
	unsigned digits;

	/* The whitespace and comments before it. */
	logo_skip_space(data, size, at, contain);

	/* Its digits, bounded. */
	value = 0U;
	digits = 0U;
	while (*at < size && data[*at] >= '0' && data[*at] <= '9') {
		if (digits >= 6U)
			return -1;

		/* Takes the digit and moves past it. */
		value = value * 10U + (uint32_t)(data[*at] - '0');
		digits++;
		(*at)++;
	}

	/* A number has at least one digit. */
	if (digits == 0U)
		return -1;

	/* Succeeded: the number. */
	*number = value;
	return 0;
}

/* Steps over whitespace and # comments in a PPM header, noting a "fit=contain" comment. */
static void
logo_skip_space(
	const uint8_t *data,
	size_t size,
	size_t *at,
	int *contain)
{
	int asked;

	/* Blanks, and comments to their line's end. */
	while (*at < size) {
		/* A comment may ask for black bars; it runs to the line's end. */
		if (data[*at] == '#') {
			asked = logo_comment_contain(data, size, *at + 1U);
			if (asked)
				*contain = 1;

			/* Steps to the end of the comment's line. */
			while (*at < size && data[*at] != '\n')
				(*at)++;

			continue;
		}

		/* Anything but a blank is where the next field starts. */
		if (data[*at] != ' ' &&
		    data[*at] != '\t' &&
		    data[*at] != '\r' &&
		    data[*at] != '\n')
			return;

		/* Steps over the blank. */
		(*at)++;
	}
}

/* Reports whether a comment's text (from after its #) is "fit=contain", blanks around it allowed. */
static int
logo_comment_contain(
	const uint8_t *data,
	size_t size,
	size_t at)
{
	/* The comment that asks for black bars. */
	static const char word[] = LOGO_CONTAIN_WORD;
	size_t index;

	/* Blanks before the word. */
	while (at < size && (data[at] == ' ' || data[at] == '\t'))
		at++;

	/* The word, letter by letter. */
	for (index = 0; index + 1U < sizeof(word); index++) {
		if (at + index >= size || data[at + index] != (uint8_t)word[index])
			return 0;
	}

	/* Nothing but blanks after it on the line. */
	at += index;
	while (at < size &&
	       (data[at] == ' ' ||
		data[at] == '\t' ||
		data[at] == '\r'))
		at++;

	/* Refuses a comment with more than the word on its line. */
	if (at < size && data[at] != '\n')
		return 0;

	/* Succeeded: the picture asks for black bars. */
	return 1;
}

/* Packs an RGB triple the way the framebuffer lays its pixels out. */
static uint32_t
logo_pixel(
	const struct zbl6_framebuffer *framebuffer,
	const uint8_t *rgb)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* The three channels. */
	red = rgb[0];
	green = rgb[1];
	blue = rgb[2];

	/* RGBX keeps red in the low byte. */
	if (framebuffer->format == ZBL6_FRAMEBUFFER_RGBX8888)
		return red | (green << 8) | (blue << 16);

	/* BGRX keeps blue in the low byte. */
	return blue | (green << 8) | (red << 16);
}
