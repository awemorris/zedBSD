/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_UAPI_GRAPHICS_H
#define BOOTS_UAPI_GRAPHICS_H

#include <stdint.h>
#include <sys/ioctl.h>

#define BOOTS_GRAPHICS_IOC_GROUP 'g'

#define BOOTS_GRAPHICS_CAP_FILL         0x00000001U
#define BOOTS_GRAPHICS_CAP_LINE         0x00000002U
#define BOOTS_GRAPHICS_CAP_PATTERN      0x00000004U
#define BOOTS_GRAPHICS_CAP_BLIT_INDEX8  0x00000008U
#define BOOTS_GRAPHICS_CAP_BLIT_RGB24   0x00000010U
#define BOOTS_GRAPHICS_CAP_BLIT_MONO1   0x00000020U
#define BOOTS_GRAPHICS_CAP_FLUSH        0x00000040U
#define BOOTS_GRAPHICS_CAP_GLYPH        0x00000080U

#define BOOTS_GRAPHICS_FORMAT_INDEX8 1U
#define BOOTS_GRAPHICS_FORMAT_RGB24  2U
#define BOOTS_GRAPHICS_FORMAT_MONO1  3U
#define BOOTS_GRAPHICS_GLYPH_MSB1    1U

struct boots_graphics_caps {
	uint32_t capabilities;
	uint32_t maximum_width;
	uint32_t maximum_height;
	uint32_t reserved;
};

struct boots_graphics_mode {
	uint32_t preferred_bits_per_pixel;
	uint32_t width;
	uint32_t height;
	uint32_t bits_per_pixel;
	uint32_t stride;
	uint32_t capabilities;
	uint32_t reserved[2];
};

struct boots_graphics_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct boots_graphics_fill {
	struct boots_graphics_rect rect;
	uint32_t color;
	uint32_t reserved;
};

struct boots_graphics_line {
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t color;
	uint32_t reserved;
};

struct boots_graphics_pattern_fill {
	struct boots_graphics_rect rect;
	uint32_t color;
	uint32_t reserved;
	uint64_t pattern;
};

struct boots_graphics_blit {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride;
	uint32_t pixels;
	uint32_t palette;
	uint32_t palette_count;
	uint32_t foreground;
	uint32_t background;
	uint32_t reserved;
	uint64_t pattern;
};

struct boots_graphics_flush {
	uint32_t rectangles;
	uint32_t rectangle_count;
};

struct boots_graphics_glyph {
	uint32_t codepoint;
	uint32_t bitmap;
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

#define BOOTS_GRAPHICS_GET_CAPS \
	_IOR(BOOTS_GRAPHICS_IOC_GROUP, 1, struct boots_graphics_caps)
#define BOOTS_GRAPHICS_ENTER \
	_IOWR(BOOTS_GRAPHICS_IOC_GROUP, 2, struct boots_graphics_mode)
#define BOOTS_GRAPHICS_GET_MODE \
	_IOR(BOOTS_GRAPHICS_IOC_GROUP, 3, struct boots_graphics_mode)
#define BOOTS_GRAPHICS_FILL_RECT \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 4, struct boots_graphics_fill)
#define BOOTS_GRAPHICS_DRAW_LINE \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 5, struct boots_graphics_line)
#define BOOTS_GRAPHICS_PATTERN_FILL \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 6, struct boots_graphics_pattern_fill)
#define BOOTS_GRAPHICS_BLIT \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 7, struct boots_graphics_blit)
#define BOOTS_GRAPHICS_BLIT_PATTERN \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 8, struct boots_graphics_blit)
#define BOOTS_GRAPHICS_FLUSH \
	_IOW(BOOTS_GRAPHICS_IOC_GROUP, 9, struct boots_graphics_flush)
#define BOOTS_GRAPHICS_GET_GLYPH \
	_IOWR(BOOTS_GRAPHICS_IOC_GROUP, 10, struct boots_graphics_glyph)

#endif
