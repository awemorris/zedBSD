/*
 * /dev/graphics
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_GRAPHICS_H
#define ZEDBSD_UAPI_GRAPHICS_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/types.h>

#define ZEDBSD_GRAPHICS_IOC_GROUP 'g'

#define ZEDBSD_GRAPHICS_CAP_FILL         0x00000001U
#define ZEDBSD_GRAPHICS_CAP_LINE         0x00000002U
#define ZEDBSD_GRAPHICS_CAP_PATTERN      0x00000004U
#define ZEDBSD_GRAPHICS_CAP_BLIT_INDEX8  0x00000008U
#define ZEDBSD_GRAPHICS_CAP_BLIT_RGB24   0x00000010U
#define ZEDBSD_GRAPHICS_CAP_BLIT_MONO1   0x00000020U
#define ZEDBSD_GRAPHICS_CAP_FLUSH        0x00000040U
#define ZEDBSD_GRAPHICS_CAP_GLYPH        0x00000080U

#define ZEDBSD_GRAPHICS_FORMAT_INDEX8 1U
#define ZEDBSD_GRAPHICS_FORMAT_RGB24  2U
#define ZEDBSD_GRAPHICS_FORMAT_MONO1  3U
#define ZEDBSD_GRAPHICS_GLYPH_MSB1    1U

struct zedbsd_graphics_caps {
	uint32_t capabilities;
	uint32_t maximum_width;
	uint32_t maximum_height;
	uint32_t reserved;
};

struct zedbsd_graphics_mode {
	uint32_t preferred_width;
	uint32_t preferred_height;
	uint32_t preferred_bits_per_pixel;
	uint32_t width;
	uint32_t height;
	uint32_t bits_per_pixel;
	uint32_t stride;
	uint32_t capabilities;
};

struct zedbsd_graphics_mode_info {
	uint32_t width;
	uint32_t height;
	uint32_t bits_per_pixel;
	uint32_t stride;
};

struct zedbsd_graphics_mode_list {
	uapi_ptr_t modes;
	uint32_t capacity;
	uint32_t count;
	uint32_t reserved;
};

struct zedbsd_graphics_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct zedbsd_graphics_fill {
	struct zedbsd_graphics_rect rect;
	uint32_t color;
	uint32_t reserved;
};

struct zedbsd_graphics_line {
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t color;
	uint32_t reserved;
};

struct zedbsd_graphics_pattern_fill {
	struct zedbsd_graphics_rect rect;
	uint32_t color;
	uint32_t reserved;
	uint64_t pattern;
};

struct zedbsd_graphics_blit {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride;
	uapi_ptr_t pixels;
	uapi_ptr_t palette;
	uint32_t palette_count;
	uint32_t foreground;
	uint32_t background;
	uint32_t reserved;
	uint64_t pattern;
};

struct zedbsd_graphics_flush {
	uapi_ptr_t rectangles;
	uint32_t rectangle_count;
};

struct zedbsd_graphics_glyph {
	uint32_t codepoint;
	uapi_ptr_t bitmap;
	uint32_t bitmap_capacity;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	int32_t bearing_x;
	int32_t bearing_y;
	uint32_t advance;
	uint32_t format;
	uint32_t bitmap_size;
	uint32_t reserved;
};

#define ZEDBSD_GRAPHICS_GET_CAPS \
	_IOR(ZEDBSD_GRAPHICS_IOC_GROUP, 1, struct zedbsd_graphics_caps)
#define ZEDBSD_GRAPHICS_ENTER \
	_IOWR(ZEDBSD_GRAPHICS_IOC_GROUP, 2, struct zedbsd_graphics_mode)
#define ZEDBSD_GRAPHICS_GET_MODE \
	_IOR(ZEDBSD_GRAPHICS_IOC_GROUP, 3, struct zedbsd_graphics_mode)
#define ZEDBSD_GRAPHICS_FILL_RECT \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 4, struct zedbsd_graphics_fill)
#define ZEDBSD_GRAPHICS_DRAW_LINE \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 5, struct zedbsd_graphics_line)
#define ZEDBSD_GRAPHICS_PATTERN_FILL \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 6, struct zedbsd_graphics_pattern_fill)
#define ZEDBSD_GRAPHICS_BLIT \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 7, struct zedbsd_graphics_blit)
#define ZEDBSD_GRAPHICS_BLIT_PATTERN \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 8, struct zedbsd_graphics_blit)
#define ZEDBSD_GRAPHICS_FLUSH \
	_IOW(ZEDBSD_GRAPHICS_IOC_GROUP, 9, struct zedbsd_graphics_flush)
#define ZEDBSD_GRAPHICS_GET_GLYPH \
	_IOWR(ZEDBSD_GRAPHICS_IOC_GROUP, 10, struct zedbsd_graphics_glyph)
#define ZEDBSD_GRAPHICS_GET_MODES \
	_IOWR(ZEDBSD_GRAPHICS_IOC_GROUP, 11, struct zedbsd_graphics_mode_list)

#endif
