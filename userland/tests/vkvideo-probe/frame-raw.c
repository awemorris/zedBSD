/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "frame-raw.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The widest picture the probe writes. */
#define FRAME_RAW_MAX_WIDTH	4096U

static int frame_raw_rows(FILE *file, const uint8_t *plane, size_t pitch, int linear, uint32_t x, uint32_t y, uint32_t width, uint32_t rows);

/*
 * Appends the display window of a decoded NV12 picture to the file: the
 * window's Y rows, then its CbCr rows (half as many, the same bytes
 * across), the bytes frame_hash hashes.  Returns 0, or -1 when the file
 * could not be written.
 */
int
frame_raw(
	FILE *file,
	const struct frame_planes *planes,
	const struct frame_window *window)
{
	int error;

	/* The Y plane's rows, then the CbCr plane's. */
	error = frame_raw_rows(file, planes->luma, planes->luma_pitch, planes->linear, window->x, window->y, window->width, window->height);
	if (error == 0)
		error = frame_raw_rows(file, planes->chroma, planes->chroma_pitch, planes->linear, window->x, window->y / 2U,
				       window->width, window->height / 2U);

	/* Succeeded, or the write failed. */
	return error;
}

/*
 * Writes `rows` rows of `width` bytes of a plane from column x of row y,
 * read straight from a linear plane or gathered out of its Y tiles.
 */
static int
frame_raw_rows(
	FILE *file,
	const uint8_t *plane,
	size_t pitch,
	int linear,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t rows)
{
	static uint8_t line[FRAME_RAW_MAX_WIDTH];
	uint32_t row;
	uint32_t column;

	/* One row at a time. */
	for (row = 0U; row < rows; row++) {
		for (column = 0U; column < width && column < FRAME_RAW_MAX_WIDTH; column++) {
			if (linear)
				line[column] = plane[(size_t)(y + row) * pitch + x + column];
			else
				line[column] = plane[frame_tile_y_offset(pitch, x + column, y + row)];
		}
		if (fwrite(line, 1U, column, file) != column)
			return -1;
	}

	/* Succeeded: the rows are written. */
	return 0;
}
