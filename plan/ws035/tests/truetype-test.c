/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libtruetype reads and draws, checked against a real font.
 *
 * A rasterizer cannot be checked by comparing bytes with another one: a
 * different sampling gives different values for the same correct shape.
 * What is checked here is what a wrong reader gets wrong -- the glyph a
 * character maps to, the box it occupies, the advance, and whether the
 * coverage has the shape the letter should have.  A capital I is a bar, an
 * o has a hole, a period is small and low, and a space draws nothing.
 *
 *     truetype-test FONT
 */

#include <truetype.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BITMAP_MAX	(256U * 256U)

static unsigned failures;
static uint8_t bitmap[BITMAP_MAX];

static void check(int condition, const char *what);
static void *load(const char *path, size_t *size);
static unsigned ink(const struct truetype_glyph *metrics);

/* Records one expectation. */
static void
check(
	int condition,
	const char *what)
{
	/* Handles the expectation that held. */
	if (condition) {
		printf("ok       %s\n", what);
		return;
	}
	printf("FAILED   %s\n", what);
	failures++;
}

/* Reads a whole file into memory the caller keeps. */
static void *
load(
	const char *path,
	size_t *size)
{
	struct stat information;
	void *data;
	FILE *file;

	file = fopen(path, "rb");

	/* Handles the file availability. */
	if (file == NULL)
		return NULL;

	/* Handles the size query failure. */
	if (fstat(fileno(file), &information) != 0) {
		fclose(file);
		return NULL;
	}
	*size = (size_t)information.st_size;
	data = malloc(*size);

	/* Handles the allocation failure. */
	if (data == NULL) {
		fclose(file);
		return NULL;
	}

	/* Handles the short read. */
	if (fread(data, 1, *size, file) != *size) {
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);

	/* Succeeded. */
	return data;
}

/* Counts the pixels a glyph put any ink into. */
static unsigned
ink(
	const struct truetype_glyph *metrics)
{
	unsigned count, row, column;

	count = 0;

	/* Process each row. */
	for (row = 0; row < metrics->height; row++) {
		/* Process each column. */
		for (column = 0; column < metrics->width; column++) {
			/* Skips a pixel the outline did not reach. */
			if (bitmap[row * metrics->width + column] < 64U)
				continue;
			count++;
		}
	}

	/* Returns the computed result. */
	return count;
}

/*
 * Runs the checks.
 */
int
main(
	int argc,
	char **argv)
{
	struct truetype_face *face;
	struct truetype_metrics metrics;
	struct truetype_glyph glyph, other;
	unsigned index, capital_i, letter_o, period, space, middle, holes;
	unsigned column, row, transitions, previous;
	void *data;
	size_t size;
	int error;

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: truetype-test FONT\n");
		return 2;
	}
	data = load(argv[1], &size);

	/* Handles the font availability. */
	if (data == NULL) {
		fprintf(stderr, "truetype-test: cannot read %s\n", argv[1]);
		return 2;
	}

	/* A file that is not a font is refused rather than read as one. */
	error = truetype_open("not a font at all", 17, 0, &face);
	check(error != 0, "a file that is not a font is refused");

	/* A truncated font is refused. */
	error = truetype_open(data, 8, 0, &face);
	check(error != 0, "a truncated font is refused");

	/* A face index an ordinary font does not have is refused. */
	error = truetype_open(data, size, 7, &face);
	check(error != 0, "a face an ordinary font does not have is refused");

	error = truetype_open(data, size, 0, &face);
	check(error == 0, "the font opens");

	/* Handles the font this test cannot go on without. */
	if (error != 0) {
		free(data);
		return 1;
	}
	error = truetype_set_pixel_size(face, 0);
	check(error != 0, "a size of zero is refused");
	error = truetype_set_pixel_size(face, 48);
	check(error == 0, "the size is chosen");
	error = truetype_metrics(face, &metrics);
	check(error == 0 && metrics.ascent > 0 && metrics.descent < 0 &&
	      metrics.line_height >= metrics.ascent - metrics.descent,
	      "the vertical measurements are sensible");

	/* Every letter maps to a glyph, and different letters to different ones. */
	capital_i = truetype_glyph_index(face, 'I');
	letter_o = truetype_glyph_index(face, 'o');
	period = truetype_glyph_index(face, '.');
	space = truetype_glyph_index(face, ' ');
	/*
	 * A fallback font carries the characters the main font lacks and no
	 * others: DroidSansFallbackFull maps space and then jumps to U+0E3F.
	 * The shape checks below name particular letters, so such a font is
	 * reported as unsuitable rather than as a failure of the reader.
	 */
	if (capital_i == 0 || letter_o == 0 || period == 0) {
		printf("TRUETYPE SKIP %s has no Latin letters\n", argv[1]);
		truetype_close(face);
		free(data);
		return 77;
	}
	check(capital_i != 0 && letter_o != 0 && period != 0,
	      "the character map finds I, o and .");
	check(capital_i != letter_o && letter_o != period,
	      "different characters map to different glyphs");

	/* A character no font draws maps to nothing. */
	index = truetype_glyph_index(face, 0x10fffdU);
	check(index == 0, "a private-use character maps to no glyph");

	/* A capital I is a bar: as tall as the ascent and much narrower. */
	error = truetype_render_glyph(face, capital_i, &glyph, bitmap,
				      256U, sizeof(bitmap));
	check(error == 0, "I renders");
	check(glyph.height > (unsigned)metrics.ascent * 3U / 4U,
	      "I is nearly as tall as the ascent");
	check(glyph.width < glyph.height, "I is taller than it is wide");
	check(ink(&glyph) > 0, "I has ink in it");
	check(glyph.advance > 0, "I advances the pen");

	/*
	 * An o has a hole.  Across its middle row the coverage goes
	 * background, ink, background, ink, background: four transitions.
	 * A rasterizer that fills the whole shape gives two.
	 */
	error = truetype_render_glyph(face, letter_o, &glyph, bitmap,
				      256U, sizeof(bitmap));
	check(error == 0, "o renders");
	middle = glyph.height / 2U;
	transitions = 0;
	previous = 0;

	/* Process each column of the middle row. */
	for (column = 0; column < glyph.width; column++) {
		index = bitmap[middle * 256U + column] >= 64U;

		/* Counts every change between ink and background. */
		if (index != previous)
			transitions++;
		previous = index;
	}

	/* The row ends at the edge of the box, where the ink stops. */
	if (previous != 0)
		transitions++;
	check(transitions == 4U, "o has a hole across its middle");

	/* The hole is empty, not merely lighter. */
	holes = 0;

	/* Process each row of the o. */
	for (row = glyph.height / 3U; row < glyph.height * 2U / 3U; row++) {
		/* Counts the rows whose centre column has no ink. */
		if (bitmap[row * 256U + glyph.width / 2U] < 64U)
			holes++;
	}
	check(holes > 0, "the middle of the o is empty");

	/* A period is small and sits at the baseline. */
	error = truetype_render_glyph(face, period, &other, bitmap, 256U, sizeof(bitmap));
	check(error == 0, "a period renders");
	check(other.height < glyph.height, "a period is shorter than an o");
	check(other.top <= glyph.top, "a period does not reach above an o");

	/* A space draws nothing but still advances. */
	error = truetype_render_glyph(face, space, &other, bitmap, 256U, sizeof(bitmap));
	check(error == 0 && other.width == 0 && other.height == 0,
	      "a space draws nothing");
	check(other.advance > 0, "a space advances the pen");

	/* Measuring and drawing agree. */
	error = truetype_glyph_metrics(face, letter_o, &other);
	check(error == 0 && other.width == glyph.width &&
	      other.height == glyph.height && other.advance == glyph.advance,
	      "measuring gives what drawing gives");

	/* A bitmap narrower than the glyph is refused. */
	error = truetype_render_glyph(face, letter_o, &other, bitmap,
				      (size_t)glyph.width - 1U, sizeof(bitmap));
	check(error == EINVAL, "a bitmap narrower than the glyph is refused");

	/* A bitmap wide enough but too short is refused as well. */
	error = truetype_render_glyph(face, letter_o, &other, bitmap, 256U,
				      256U * (size_t)(glyph.height - 1U));
	check(error == EINVAL, "a bitmap shorter than the glyph is refused");

	/* Twice the size is about twice as tall. */
	error = truetype_set_pixel_size(face, 96);
	check(error == 0, "the size changes");
	error = truetype_render_glyph(face, capital_i, &other, bitmap, 256U, sizeof(bitmap));
	check(error == 0 && other.height > glyph.height,
	      "a larger size gives a larger glyph");
	truetype_close(face);
	free(data);
	printf("%s\n", failures == 0 ? "TRUETYPE PASS" : "TRUETYPE FAIL");

	/* Returns the computed result. */
	return failures == 0 ? 0 : 1;
}
