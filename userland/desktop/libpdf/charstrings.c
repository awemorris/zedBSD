/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The charstring fonts of libpdf's reader (stage 3 of design-pdf.md): what
 * font.c asks of a Type 1 or CFF program once type1.c or cff.c has read
 * it — its glyphs by name, by code of its own encoding and by CID — and
 * the outline of a glyph, run by the program's interpreter into path steps
 * in ems.
 *
 * The outlines are cubic, as PDF's paths are, so they go into the display
 * list without the conversion TrueType's quadratic contours need.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "charstrings.h"

static void emit_point(struct charstrings_path *path, enum pdf_path_verb verb, const double *points, size_t count);
static int compare_names(const struct charstrings_name *left, const struct charstrings_name *right);
static void sort_names(struct charstrings_name *names, struct charstrings_name *scratch, size_t count);

/*
 * Frees a charstring font program.
 */
void
pdf_charstrings_close(
	struct pdf_charstrings *font)
{
	size_t index;

	/* Nothing was read. */
	if (font == NULL)
		return;

	/* Each private dictionary's subroutines, then the arrays and the owned bytes. */
	for (index = 0; index < font->privates_count; index++)
		free(font->privates[index].subrs);
	free(font->privates);
	free(font->private_of);
	free(font->charstrings);
	free(font->names);
	free(font->sorted_names);
	free(font->cids);
	free(font->global_subrs);
	free(font->owned);
	free(font);
}

/*
 * Reports how many glyphs a program has.
 */
size_t
pdf_charstrings_count(
	const struct pdf_charstrings *font)
{
	/* The glyph count, the missing glyph (0) included. */
	return font->glyphs;
}

/*
 * Finds the glyph of a name; ENOENT when the program has none by that
 * name.
 */
int
pdf_charstrings_find(
	const struct pdf_charstrings *font,
	const unsigned char *name,
	size_t length,
	unsigned *glyph)
{
	struct charstrings_name wanted;
	size_t low;
	size_t high;
	size_t middle;
	int order;

	/* Searches the sorted names. */
	wanted.name = name;
	wanted.length = length;
	wanted.glyph = 0;
	low = 0;
	high = font->names_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		order = compare_names(&font->sorted_names[middle], &wanted);
		if (order == 0) {
			*glyph = font->sorted_names[middle].glyph;
			return 0;
		}

		/* Narrows to the half the name is in. */
		if (order < 0) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}

	/* The program has no glyph of the name. */
	return ENOENT;
}

/*
 * Reports a glyph's name (a CID-keyed program's glyphs have none: ENOENT).
 */
int
pdf_charstrings_name(
	const struct pdf_charstrings *font,
	unsigned glyph,
	const unsigned char **name,
	size_t *length)
{
	/* Only a glyph with a name has one. */
	if (font->names == NULL)
		return ENOENT;
	if (glyph >= font->glyphs)
		return ENOENT;
	if (font->names[glyph].size == 0)
		return ENOENT;

	/* Succeeded: the name's bytes, which live with the program. */
	*name = font->names[glyph].data;
	*length = font->names[glyph].size;
	return 0;
}

/*
 * Finds the glyph a code of the program's own encoding draws; ENOENT when
 * the program has no encoding or the code draws nothing.
 */
int
pdf_charstrings_builtin(
	const struct pdf_charstrings *font,
	unsigned code,
	unsigned *glyph)
{
	/* Only a program with its own encoding maps codes. */
	if (!font->has_builtin)
		return ENOENT;
	if (code > 255)
		return ENOENT;
	if (font->builtin[code] == 0)
		return ENOENT;

	/* Succeeded: the code's glyph. */
	*glyph = font->builtin[code];
	return 0;
}

/*
 * Reports whether a program is CID-keyed (its glyphs are found by CID).
 */
int
pdf_charstrings_cid_keyed(
	const struct pdf_charstrings *font)
{
	/* A CID-keyed CFF program's charset maps glyphs to CIDs. */
	return font->cid_keyed;
}

/*
 * Finds the glyph of a CID in a CID-keyed program; ENOENT when it has
 * none.
 */
int
pdf_charstrings_cid(
	const struct pdf_charstrings *font,
	unsigned cid,
	unsigned *glyph)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Searches the CIDs, which are sorted. */
	low = 0;
	high = font->cids_count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (font->cids[middle].cid == cid) {
			*glyph = font->cids[middle].glyph;
			return 0;
		}

		/* Narrows to the half the CID is in. */
		if (font->cids[middle].cid < cid) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}

	/* The program has no glyph for the CID. */
	return ENOENT;
}

/*
 * Draws a glyph's outline: its charstring run into path steps in ems with
 * y upward, each subpath closed.  *advance is the glyph's width in ems.
 *
 * A glyph the program does not have draws nothing (0 with no step); a
 * damaged charstring draws what it drew before the damage.  Only the
 * sink's own failure (ENOMEM) is an error.
 */
int
pdf_charstrings_outline(
	struct pdf_charstrings *font,
	unsigned glyph,
	pdf_charstrings_emit emit,
	void *context,
	double *advance)
{
	struct charstrings_path path;
	const double *matrix;
	double width;

	/* The matrix into ems: a CID-keyed glyph's font DICT's, else the program's. */
	*advance = 0.0;
	matrix = font->matrix;
	if (font->private_of != NULL && glyph < font->glyphs)
		matrix = font->privates[font->private_of[glyph]].matrix;

	/* A glyph the program does not have draws nothing. */
	if (glyph >= font->glyphs)
		return 0;

	/* Starts an empty path at the origin. */
	memset(&path, 0, sizeof(path));
	path.matrix = matrix;
	path.emit = emit;
	path.context = context;

	/* Runs the glyph's charstring by the program's kind; damage ends it where it stands. */
	width = 0.0;
	if (font->kind == CHARSTRINGS_TYPE1) {
		pdf_type1_run(font, glyph, &path, &width);
	} else {
		pdf_cff_run(font, glyph, &path, &width);
	}

	/* The last subpath is closed, whatever ended the charstring. */
	charstrings_close(&path);

	/* Reports the sink's failure. */
	if (path.error != 0)
		return path.error;

	/* Succeeded: the width along the matrix's x axis. */
	*advance = width * matrix[0];
	return 0;
}

/*
 * Finds the glyph a code of StandardEncoding names (the encoding seac's
 * base and accent codes are in); ENOENT when there is none.
 */
int
charstrings_standard_glyph(
	const struct pdf_charstrings *font,
	unsigned code,
	unsigned *glyph)
{
	const char *name;
	unsigned sid;
	int error;

	/* The code's name. */
	if (code > 255)
		return ENOENT;
	sid = pdf_cff_standard_encoding[code];
	if (sid == 0)
		return ENOENT;
	name = pdf_cff_standard_strings[sid];

	/* The glyph of that name. */
	error = pdf_charstrings_find(font, (const unsigned char *)name, strlen(name), glyph);
	if (error != 0)
		return error;

	/* Succeeded: the code's glyph. */
	return 0;
}

/*
 * Sorts a program's glyph names for pdf_charstrings_find(); a glyph
 * without a name is left out.
 */
int
charstrings_sort_names(
	struct pdf_charstrings *font)
{
	struct charstrings_name *scratch;
	size_t glyph;
	size_t count;

	/* Allocates the sorted list. */
	font->names_count = 0;
	if (font->names == NULL)
		return 0;
	font->sorted_names = malloc(sizeof(*font->sorted_names) * (font->glyphs + 1));
	if (font->sorted_names == NULL)
		return ENOMEM;

	/* Lists each glyph that has a name. */
	count = 0;
	for (glyph = 0; glyph < font->glyphs; glyph++) {
		if (font->names[glyph].size == 0)
			continue;
		font->sorted_names[count].name = font->names[glyph].data;
		font->sorted_names[count].length = font->names[glyph].size;
		font->sorted_names[count].glyph = (unsigned)glyph;
		count++;
	}

	/* Allocates the merge sort's other half. */
	scratch = malloc(sizeof(*scratch) * (count + 1));
	if (scratch == NULL)
		return ENOMEM;

	/* Sorts by name, the lower glyph first among equal names. */
	sort_names(font->sorted_names, scratch, count);
	free(scratch);

	/* Succeeded: the names can be searched. */
	font->names_count = count;
	return 0;
}

/*
 * Starts a subpath at a point of glyph space, closing the one before.
 */
void
charstrings_move(
	struct charstrings_path *path,
	double x,
	double y)
{
	double point[2];

	/* The subpath before ends. */
	charstrings_close(path);

	/* The new one starts; nothing is emitted until it draws. */
	point[0] = x;
	point[1] = y;
	path->x = point[0];
	path->y = point[1];
	path->start_x = point[0];
	path->start_y = point[1];
}

/*
 * Draws a line from the current point, starting a subpath at the current
 * point when none is open.
 */
void
charstrings_line(
	struct charstrings_path *path,
	double x,
	double y)
{
	double point[2];

	/* A drawing step opens the subpath where the pen is. */
	if (!path->open) {
		point[0] = path->x;
		point[1] = path->y;
		emit_point(path, PDF_PATH_MOVE, point, 1);
		path->start_x = path->x;
		path->start_y = path->y;
		path->open = 1;
	}

	/* The line. */
	point[0] = x;
	point[1] = y;
	emit_point(path, PDF_PATH_LINE, point, 1);
	path->x = x;
	path->y = y;
}

/*
 * Draws a cubic curve from the current point, starting a subpath at the
 * current point when none is open.
 */
void
charstrings_curve(
	struct charstrings_path *path,
	double x1,
	double y1,
	double x2,
	double y2,
	double x3,
	double y3)
{
	double points[6];

	/* A drawing step opens the subpath where the pen is. */
	if (!path->open) {
		points[0] = path->x;
		points[1] = path->y;
		emit_point(path, PDF_PATH_MOVE, points, 1);
		path->start_x = path->x;
		path->start_y = path->y;
		path->open = 1;
	}

	/* The curve's two control points and end. */
	points[0] = x1;
	points[1] = y1;
	points[2] = x2;
	points[3] = y2;
	points[4] = x3;
	points[5] = y3;
	emit_point(path, PDF_PATH_CUBIC, points, 3);
	path->x = x3;
	path->y = y3;
}

/*
 * Closes the open subpath; the current point returns to its start.
 */
void
charstrings_close(
	struct charstrings_path *path)
{
	double point[2];

	/* Nothing is open. */
	if (!path->open)
		return;

	/* Closes it; a charstring's closepath leaves the pen at the subpath's start. */
	point[0] = path->start_x;
	point[1] = path->start_y;
	emit_point(path, PDF_PATH_CLOSE, point, 0);
	path->open = 0;
}

/*
 * Sends one path step to the sink with its points moved from glyph space
 * into ems; a failed sink stops every later step.
 */
static void
emit_point(
	struct charstrings_path *path,
	enum pdf_path_verb verb,
	const double *points,
	size_t count)
{
	const double *matrix;
	double moved[6];
	size_t index;

	/* After a failure nothing more is sent. */
	if (path->error != 0)
		return;

	/* Each point through the matrix. */
	matrix = path->matrix;
	for (index = 0; index < count; index++) {
		moved[index * 2] = matrix[0] * points[index * 2] + matrix[2] * points[index * 2 + 1] + matrix[4];
		moved[index * 2 + 1] = matrix[1] * points[index * 2] + matrix[3] * points[index * 2 + 1] + matrix[5];
	}

	/* Sends the step. */
	path->error = path->emit(path->context, verb, moved, count);
}

/* Orders two names by their bytes, then by length, then by glyph. */
static int
compare_names(
	const struct charstrings_name *left,
	const struct charstrings_name *right)
{
	size_t shorter;
	int order;

	/* The bytes both have. */
	shorter = left->length;
	if (right->length < shorter)
		shorter = right->length;
	order = memcmp(left->name, right->name, shorter);
	if (order != 0)
		return order;

	/* The shorter name first. */
	if (left->length < right->length)
		return -1;
	if (left->length > right->length)
		return 1;

	/* The same name. */
	return 0;
}

/* Sorts names by merging runs of doubling width between the list and the scratch. */
static void
sort_names(
	struct charstrings_name *names,
	struct charstrings_name *scratch,
	size_t count)
{
	struct charstrings_name *source;
	struct charstrings_name *target;
	struct charstrings_name *swap;
	size_t width;
	size_t start;
	size_t middle;
	size_t end;
	size_t left;
	size_t right;
	size_t out;
	int order;
	int take_left;

	/* Merges runs of width, doubling it, between the two arrays. */
	source = names;
	target = scratch;
	for (width = 1; width < count; width *= 2) {
		for (start = 0; start < count; start += 2 * width) {
			/* The two runs [start, middle) and [middle, end). */
			middle = start + width;
			if (middle > count)
				middle = count;
			end = start + 2 * width;
			if (end > count)
				end = count;

			/* Takes the lower head each time, the left one on a tie (the lower glyph). */
			left = start;
			right = middle;
			for (out = start; out < end; out++) {
				take_left = 0;
				if (right >= end) {
					/* The right run is used up. */
					take_left = 1;
				} else if (left < middle) {
					/* Both runs have a head. */
					order = compare_names(&source[left], &source[right]);
					if (order <= 0)
						take_left = 1;
				}

				/* Moves the chosen head. */
				if (take_left) {
					target[out] = source[left];
					left++;
				} else {
					target[out] = source[right];
					right++;
				}
			}
		}

		/* The merged runs are the next pass's source. */
		swap = source;
		source = target;
		target = swap;
	}

	/* The sorted names end where the last pass wrote them. */
	if (source != names)
		memcpy(names, source, count * sizeof(*names));
}

/*
 * Finds the CFF table of an OpenType font (a /FontFile3 of subtype
 * OpenType whose outlines are CFF); ENOENT when it has none (its outlines
 * are TrueType's).
 */
int
pdf_opentype_cff(
	const unsigned char *data,
	size_t size,
	const unsigned char **cff,
	size_t *cff_size)
{
	const unsigned char *record;
	unsigned long offset;
	unsigned long length;
	size_t tables;
	size_t index;
	int differs;

	/* The table directory: a version, the table count, and 16 bytes a table. */
	if (size < 12)
		return PDF_EFORMAT;
	tables = ((size_t)data[4] << 8) | data[5];
	if (tables > (size - 12) / 16)
		return PDF_EFORMAT;

	/* Looks for the tag "CFF ". */
	for (index = 0; index < tables; index++) {
		record = data + 12 + index * 16;
		differs = memcmp(record, "CFF ", 4);
		if (differs != 0)
			continue;

		/* The table must lie in the font. */
		offset = ((unsigned long)record[8] << 24) | ((unsigned long)record[9] << 16) | ((unsigned long)record[10] << 8) | record[11];
		length = ((unsigned long)record[12] << 24) | ((unsigned long)record[13] << 16) | ((unsigned long)record[14] << 8) | record[15];
		if (offset > size)
			return PDF_EFORMAT;
		if (length > size - offset)
			return PDF_EFORMAT;

		/* Succeeded: the table's bytes. */
		*cff = data + offset;
		*cff_size = length;
		return 0;
	}

	/* The font's outlines are not CFF. */
	return ENOENT;
}
