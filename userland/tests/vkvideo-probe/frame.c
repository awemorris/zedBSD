/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame hash of vkvideo-probe (see frame.h).
 *
 * A Y tile (Gen12, no swizzling) is 128 bytes by 32 rows, 4 KiB; inside it
 * the bytes go in columns 16 bytes wide, each column's 32 rows one after
 * the other; the tiles of a plane go row of tiles after row of tiles, the
 * pitch's tiles across.
 */

#include "frame.h"

#include "../../base/common/sha256.h"

#include <stddef.h>
#include <stdint.h>

/* The shape of a Y tile. */
#define FRAME_TILE_BYTES	128U
#define FRAME_TILE_ROWS		32U
#define FRAME_COLUMN_BYTES	16U
#define FRAME_TILE_SIZE		4096U

/* The widest picture the probe hashes. */
#define FRAME_MAX_WIDTH		4096U

static void frame_hash_rows(struct command_sha256_context *context, const uint8_t *plane, size_t pitch, int linear, uint32_t x, uint32_t y, uint32_t width, uint32_t rows);

/*
 * Reports the offset in a Y-tiled plane of the byte at column x of row y.
 */
size_t
frame_tile_y_offset(
	size_t pitch,
	uint32_t x,
	uint32_t y)
{
	size_t tile;
	size_t inside;

	/* The tile: its row of tiles, then its place across. */
	tile = (size_t)(y / FRAME_TILE_ROWS) * (pitch / FRAME_TILE_BYTES) + x / FRAME_TILE_BYTES;

	/* Inside it: the 16-byte column, the row in the column, the byte in the row. */
	inside = (size_t)((x % FRAME_TILE_BYTES) / FRAME_COLUMN_BYTES) * (FRAME_COLUMN_BYTES * FRAME_TILE_ROWS) +
	    (size_t)(y % FRAME_TILE_ROWS) * FRAME_COLUMN_BYTES +
	    x % FRAME_COLUMN_BYTES;

	/* Succeeded: the byte's offset. */
	return tile * FRAME_TILE_SIZE + inside;
}

/*
 * Hashes the display window of a decoded NV12 picture as SHA-256 text:
 * the window's Y rows, then its CbCr rows (half as many, from half the
 * window's row; the same bytes across, a Cb and a Cr a pixel pair).
 */
void
frame_hash(
	const struct frame_planes *planes,
	const struct frame_window *window,
	char text[65])
{
	static const char digits[] = "0123456789abcdef";
	struct command_sha256_context context;
	uint8_t digest[32];
	unsigned index;

	/* The Y plane's rows, then the CbCr plane's. */
	command_sha256_init(&context);
	frame_hash_rows(&context, planes->luma, planes->luma_pitch, planes->linear, window->x, window->y, window->width, window->height);
	frame_hash_rows(&context, planes->chroma, planes->chroma_pitch, planes->linear, window->x, window->y / 2U, window->width, window->height / 2U);
	command_sha256_final(&context, digest);

	/* The digest as lowercase hexadecimal. */
	for (index = 0U; index < 32U; index++) {
		text[index * 2U] = digits[digest[index] >> 4];
		text[index * 2U + 1U] = digits[digest[index] & 15U];
	}
	text[64] = '\0';
}

/*
 * Hashes `rows` rows of `width` bytes of a plane from column x of row y: a
 * Y-tiled plane (Intel's decoder), or a linear one (ws236: the Raspberry
 * Pi 4's decoder writes rows of `pitch` bytes).
 */
static void
frame_hash_rows(
	struct command_sha256_context *context,
	const uint8_t *plane,
	size_t pitch,
	int linear,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t rows)
{
	static uint8_t line[FRAME_MAX_WIDTH];
	uint32_t row;
	uint32_t column;

	/* One row at a time, read straight from a linear plane or gathered out of its tiles. */
	for (row = 0U; row < rows; row++) {
		for (column = 0U; column < width && column < FRAME_MAX_WIDTH; column++) {
			if (linear) {
				line[column] = plane[(size_t)(y + row) * pitch + x + column];
			} else {
				line[column] = plane[frame_tile_y_offset(pitch, x + column, y + row)];
			}
		}
		(void)command_sha256_update(context, line, column);
	}
}
