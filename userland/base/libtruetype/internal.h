/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the TrueType reader shares between its files.
 *
 * Nothing here is public.  The face holds pointers into the caller's font
 * data, which is why that memory must outlive it.
 */

#ifndef LIBTRUETYPE_INTERNAL_H
#define LIBTRUETYPE_INTERNAL_H

#include <truetype.h>

#include <stddef.h>
#include <stdint.h>

/* Builds the four-byte tag a table directory entry is named by. */
#define TRUETYPE_TAG(a, b, c, d)					\
	(((uint32_t)(unsigned char)(a) << 24) |				\
	 ((uint32_t)(unsigned char)(b) << 16) |				\
	 ((uint32_t)(unsigned char)(c) << 8) |				\
	 (uint32_t)(unsigned char)(d))

/*
 * A size beyond this is refused rather than allocated for.  A glyph at
 * 1024 pixels per em needs a megabyte of coverage; a caller asking for more
 * has lost track of its units.
 */
#define TRUETYPE_PIXELS_MAX	1024U

/* How deep a composite glyph may nest before it is called a loop. */
#define TRUETYPE_COMPOSITE_DEPTH 8U

/* How many points and contours one glyph may have. */
#define TRUETYPE_POINTS_MAX	4096U
#define TRUETYPE_CONTOURS_MAX	256U

/*
 * Anti-aliasing samples each pixel row this many times.  The coverage of a
 * pixel is how many of its sample rows the outline covers, times how much of
 * the row it covers horizontally.
 */
#define TRUETYPE_SAMPLES	4U

/* One point of an outline, in font units scaled to the chosen size. */
struct truetype_point {
	float x;
	float y;
	unsigned on_curve;
};

/* One glyph's outline, gathered before it is drawn. */
struct truetype_outline {
	struct truetype_point points[TRUETYPE_POINTS_MAX];
	unsigned ends[TRUETYPE_CONTOURS_MAX];
	unsigned point_count;
	unsigned contour_count;
};

struct truetype_face {
	const uint8_t *data;
	size_t size;
	const uint8_t *directory;
	unsigned table_count;

	/* head */
	uint16_t units_per_em;
	unsigned long_loca;

	/* maxp */
	uint16_t glyph_count;

	/* hhea */
	int16_t ascent;
	int16_t descent;
	int16_t line_gap;
	uint16_t hmetric_count;

	/* The tables the outlines and the character map are read from. */
	const uint8_t *hmtx;
	uint32_t hmtx_size;
	const uint8_t *loca;
	uint32_t loca_size;
	const uint8_t *glyf;
	uint32_t glyf_size;
	const uint8_t *cmap;
	uint32_t cmap_size;

	/* The chosen cmap subtable, and which format it is. */
	const uint8_t *cmap_subtable;
	uint32_t cmap_subtable_size;
	unsigned cmap_format;

	unsigned pixels;
};

uint16_t truetype_u16(const uint8_t *bytes);
int16_t truetype_s16(const uint8_t *bytes);
uint32_t truetype_u32(const uint8_t *bytes);

int truetype_cmap_select(struct truetype_face *face);

int truetype_outline_load(struct truetype_face *face, unsigned glyph,
			  struct truetype_outline *outline, unsigned depth);
int truetype_advance(const struct truetype_face *face, unsigned glyph,
		     int *advance);
void truetype_outline_bounds(const struct truetype_outline *outline,
			     float *minimum_x, float *minimum_y,
			     float *maximum_x, float *maximum_y);
void truetype_rasterize(const struct truetype_outline *outline,
			const struct truetype_glyph *metrics,
			uint8_t *bitmap, size_t stride);

/*
 * Turns a measurement in font units into whole pixels.
 *
 * up rounds toward positive infinity and down toward negative, so that a
 * glyph's box never loses the fraction of a pixel it reaches into.
 */
int truetype_scale_up(const struct truetype_face *face, int value);
int truetype_scale_down(const struct truetype_face *face, int value);
float truetype_scale(const struct truetype_face *face);

#endif
