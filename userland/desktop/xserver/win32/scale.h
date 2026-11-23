/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scale of the X server's Windows backend: X's pixels on the screen's.
 *
 * The X clients see logical pixels: one X pixel is shown as percent/100
 * screen pixels each way, the percent being one of 100, 125, 150, 175,
 * 200 and so on (steps of 25 from 100 to 400).  A whole scale (100, 200,
 * 300, 400) copies each pixel to a square of pixels (nearest), so text
 * stays sharp; a fractional one filters (bilinear) and blurs a little.
 * The sizes, the places and the pointer go through the same rounding, so
 * that every screen pixel of a window falls in an X pixel of it.
 *
 * Pure: no Windows header, no system call, no state; the tests run on the
 * host.
 */

#ifndef KEILAND_X11_WIN32_SCALE_H
#define KEILAND_X11_WIN32_SCALE_H

#include <stddef.h>
#include <stdint.h>

/* The scales a server may have, in percent. */
#define X11_WIN32_SCALE_MIN	100U
#define X11_WIN32_SCALE_MAX	400U
#define X11_WIN32_SCALE_STEP	25U

/* The screen's DPI at 100 % (Windows' USER_DEFAULT_SCREEN_DPI). */
#define X11_WIN32_DPI_DEFAULT	96U

/*
 * A rectangle of pixels from (x0, y0) to (x1, y1), the far edges outside
 * it.
 */
struct x11_win32_box {
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
};

unsigned x11_win32_scale_of_dpi(unsigned dpi);
int x11_win32_scale_parse(const char *text, unsigned dpi, unsigned *percent);
int x11_win32_scale_is_whole(unsigned percent);
unsigned x11_win32_scale_size(unsigned size, unsigned percent);
int32_t x11_win32_scale_to_screen(int32_t position, unsigned percent);
int32_t x11_win32_scale_to_x(int32_t position, unsigned percent);
void x11_win32_scale_box(const struct x11_win32_box *source, unsigned source_width, unsigned source_height, unsigned width, unsigned height, struct x11_win32_box *result);
void x11_win32_scale_pixels(const uint32_t *source, size_t source_stride, unsigned source_width, unsigned source_height, uint32_t *pixels, size_t stride, unsigned width, unsigned height, int whole, const struct x11_win32_box *box);

#endif
