/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's glyphs from a TrueType font (WS069 p002).
 *
 * The core font Xzed offers (zed-unicode) has 8x16 cells, 16x16 for a wide
 * character, with the text drawn above the baseline the client gives.  A
 * monospaced TrueType font is drawn at 13 pixels onto a baseline at row 12
 * of the cell and turned into one bit a pixel (coverage of a half or more).
 * The printable ASCII glyphs are kept once drawn.
 */

#include "glyphs.h"

#include <errno.h>

#ifdef XZED_WAYLAND

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <truetype.h>
#include <unistd.h>

/* The cell, the size the font is drawn at, and where its baseline is in the cell. */
#define GLYPHS_CELL_WIDTH	8U
#define GLYPHS_CELL_HEIGHT	16U
#define GLYPHS_PIXELS		13U
#define GLYPHS_BASELINE		12

/* The largest font file read, and the largest glyph drawn. */
#define GLYPHS_FILE_MAX		(32U * 1024U * 1024U)
#define GLYPHS_DRAWN_MAX	48U

/* The printable ASCII glyphs kept. */
#define GLYPHS_CACHED		128U

/*
 * The font: the file in memory and the face over it; NULL before
 * xzed_glyphs_open and after xzed_glyphs_close.
 */
static void *glyphs_data;
static struct truetype_face *glyphs_face;

/*
 * The ASCII glyphs drawn so far, and which of them are drawn.
 */
static struct xzed_glyph glyphs_cache[GLYPHS_CACHED];
static uint8_t glyphs_cached[GLYPHS_CACHED];

static int glyphs_read(const char *path, size_t *size);
static int glyphs_draw(uint32_t codepoint, struct xzed_glyph *glyph);

/*
 * Reads a monospaced TrueType font.  Returns 0, or an errno value.
 */
int
xzed_glyphs_open(
	const char *path)
{
	size_t size;
	int error;

	/* The file. */
	error = glyphs_read(path, &size);
	if (error != 0)
		return error;

	/* The face over it, at the cell's size. */
	error = truetype_open(glyphs_data, size, 0U, &glyphs_face);
	if (error != 0) {
		xzed_glyphs_close();
		return error;
	}

	/* The cell's size. */
	error = truetype_set_pixel_size(glyphs_face, GLYPHS_PIXELS);
	if (error != 0) {
		xzed_glyphs_close();
		return error;
	}

	/* Succeeded: glyphs can be drawn. */
	return 0;
}

/*
 * Draws a character's glyph (or gives the kept one).  Returns 0, or -1
 * without a font.
 */
int
xzed_glyph(
	uint32_t codepoint,
	struct xzed_glyph *glyph)
{
	int error;

	/* Without a font there are no glyphs. */
	if (glyphs_face == NULL)
		return -1;

	/* A kept ASCII glyph. */
	if (codepoint < GLYPHS_CACHED && glyphs_cached[codepoint]) {
		*glyph = glyphs_cache[codepoint];
		return 0;
	}

	/* A new one, kept when it is ASCII. */
	error = glyphs_draw(codepoint, glyph);
	if (error != 0)
		return error;
	if (codepoint < GLYPHS_CACHED) {
		glyphs_cache[codepoint] = *glyph;
		glyphs_cached[codepoint] = 1;
	}

	/* Succeeded: the glyph. */
	return 0;
}

/*
 * Releases the font.
 */
void
xzed_glyphs_close(void)
{
	/* The face reads the file, so it goes first. */
	if (glyphs_face != NULL)
		truetype_close(glyphs_face);
	free(glyphs_data);
	glyphs_face = NULL;
	glyphs_data = NULL;
	memset(glyphs_cached, 0, sizeof(glyphs_cached));
}

/* Reads the whole font file into memory the face keeps using. */
static int
glyphs_read(
	const char *path,
	size_t *size)
{
	struct stat status;
	ssize_t count;
	size_t done;
	int descriptor;
	int error;

	/* The file and its size. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	error = fstat(descriptor, &status);
	if (error != 0 || status.st_size <= 0 || (unsigned long)status.st_size > GLYPHS_FILE_MAX) {
		close(descriptor);
		return EINVAL;
	}

	/* The memory for it. */
	*size = (size_t)status.st_size;
	glyphs_data = malloc(*size);
	if (glyphs_data == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Reads it to the end. */
	done = 0U;
	while (done < *size) {
		count = read(descriptor, (char *)glyphs_data + done, *size - done);
		if (count <= 0) {
			close(descriptor);
			return EIO;
		}

		/* What was read counts towards the whole. */
		done += (size_t)count;
	}

	/* Succeeded: the font is in memory. */
	close(descriptor);
	return 0;
}

/* Draws a glyph onto the cell's baseline as one bit a pixel. */
static int
glyphs_draw(
	uint32_t codepoint,
	struct xzed_glyph *glyph)
{
	static uint8_t coverage[GLYPHS_DRAWN_MAX * GLYPHS_DRAWN_MAX];
	struct truetype_glyph metrics;
	unsigned index;
	unsigned x;
	unsigned y;
	int cell_x;
	int cell_y;
	int error;

	/* An empty cell of the core font's size. */
	memset(glyph, 0, sizeof(*glyph));
	glyph->width = GLYPHS_CELL_WIDTH;
	glyph->height = GLYPHS_CELL_HEIGHT;
	glyph->stride = 1U;
	glyph->advance = GLYPHS_CELL_WIDTH;

	/* The glyph's coverage; one that cannot be drawn is left empty. */
	index = truetype_glyph_index(glyphs_face, codepoint);
	error = truetype_render_glyph(glyphs_face, index, &metrics, coverage, GLYPHS_DRAWN_MAX, sizeof(coverage));
	if (error != 0)
		return 0;

	/* Each covered pixel that lands in the cell sets its bit (top counts up from the baseline). */
	for (y = 0U; y < metrics.height; y++) {
		cell_y = GLYPHS_BASELINE - metrics.top + (int)y;
		if (cell_y < 0 || cell_y >= (int)GLYPHS_CELL_HEIGHT)
			continue;
		for (x = 0U; x < metrics.width; x++) {
			cell_x = metrics.left + (int)x;
			if (cell_x < 0 || cell_x >= (int)GLYPHS_CELL_WIDTH)
				continue;
			if (coverage[y * GLYPHS_DRAWN_MAX + x] >= 128U)
				glyph->bitmap[cell_y] = (uint8_t)(glyph->bitmap[cell_y] | (0x80U >> cell_x));
		}
	}

	/* Succeeded: the glyph's cell. */
	return 0;
}

#else

/*
 * Without the Wayland backend there is no TrueType library: no font opens.
 */
int
xzed_glyphs_open(
	const char *path)
{
	/* Not available. */
	(void)path;
	return ENOTSUP;
}

/*
 * No glyphs without a font.
 */
int
xzed_glyph(
	uint32_t codepoint,
	struct xzed_glyph *glyph)
{
	/* None. */
	(void)codepoint;
	(void)glyph;
	return -1;
}

/*
 * Nothing to release.
 */
void
xzed_glyphs_close(void)
{
	/* Nothing. */
	return;
}

#endif
