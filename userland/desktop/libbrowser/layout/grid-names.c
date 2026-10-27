/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Named grid lines and areas: turns the named lines and named spans of a
 * grid container's children into line numbers before the grid places them.
 *
 * A line's names are the ones its template gives it, and for each named
 * area of grid-template-areas, NAME-start on the area's first line and
 * NAME-end on its last on both axes.  A name alone on a start (or an end)
 * first looks for a NAME-start (or NAME-end) line; then it is the first
 * line of that name.  When there are fewer lines of a name than counted,
 * every implicit line after the explicit grid is taken to have it.  A
 * named span counts lines of its name from the other, definite line, and
 * one track when that line is not definite either.  Lines before the
 * explicit grid are not made: a count that runs off the start resolves to
 * the first line.
 * https://drafts.csswg.org/css-grid-2/#line-placement
 */

#include "layout/layout.h"

#include <string.h>

static int grid_names_explicit(const struct css_style *style, int columns);
static int grid_names_has(const struct css_style *style, int columns, int line, const struct vm_string *name, const char *suffix);
static int grid_names_area_edge(const struct css_style *style, int columns, int line, const struct vm_string *name, const char *suffix);
static int grid_names_equal(const struct vm_string *atom, const struct vm_string *base, const char *suffix);
static int grid_names_edge(const struct vm_string *name, const char *suffix, const struct vm_string *area, const char *edge);
static int grid_names_find(const struct css_style *style, int columns, const struct vm_string *name, const char *suffix, int count);
static int grid_names_count_from(const struct css_style *style, int columns, const struct vm_string *name, int from, int count, int forward);
static void grid_names_axis(const struct css_style *style, int columns, struct css_grid_place *place);

/*
 * Resolves the named lines and spans of a grid container's children to
 * line numbers on both axes (their boxes' styles are the layout's own).
 */
void
layout_grid_names(
	struct layout_box *box)
{
	struct layout_box *child;

	/* Each child, in the flow or out of it. */
	for (child = box->first_child; child != NULL; child = child->next) {
		grid_names_axis(&box->style, 1, &child->style.grid_column);
		grid_names_axis(&box->style, 0, &child->style.grid_row);
	}
}

/* Resolves one axis of a place: its definite named lines first, then its named spans. */
static void
grid_names_axis(
	const struct css_style *style,
	int columns,
	struct css_grid_place *place)
{
	int count;

	/* A named start line: NAME-start for a name alone, else the count-th line of the name. */
	if (place->start_name != NULL && place->start_span == 0) {
		count = place->start;
		if (count == 0) {
			count = grid_names_find(style, columns, place->start_name, "-start", 1);
			if (count == 0)
				count = grid_names_find(style, columns, place->start_name, NULL, 1);
		} else {
			count = grid_names_find(style, columns, place->start_name, NULL, count);
		}
		place->start = count;
		place->start_name = NULL;
	}

	/* A named end line: NAME-end for a name alone, else the count-th line of the name. */
	if (place->end_name != NULL && place->end_span == 0) {
		count = place->end;
		if (count == 0) {
			count = grid_names_find(style, columns, place->end_name, "-end", 1);
			if (count == 0)
				count = grid_names_find(style, columns, place->end_name, NULL, 1);
		} else {
			count = grid_names_find(style, columns, place->end_name, NULL, count);
		}
		place->end = count;
		place->end_name = NULL;
	}

	/* A named start span counts back from a definite end. */
	if (place->start_name != NULL) {
		if (place->end > 0 && place->end_span == 0) {
			place->start = grid_names_count_from(style, columns, place->start_name, place->end, place->start_span, 0);
			place->start_span = 0;
		} else {
			place->start_span = 1;
		}
		place->start_name = NULL;
	}

	/* A named end span counts on from a definite start. */
	if (place->end_name != NULL) {
		if (place->start > 0 && place->start_span == 0) {
			place->end = grid_names_count_from(style, columns, place->end_name, place->start, place->end_span, 1);
			place->end_span = 0;
		} else {
			place->end_span = 1;
		}
		place->end_name = NULL;
	}
}

/* The number of explicit tracks on an axis: the template's, or more when the named areas need them. */
static int
grid_names_explicit(
	const struct css_style *style,
	int columns)
{
	int count;

	/* The template's tracks. */
	count = style->row_count;
	if (columns)
		count = style->column_count;

	/* The areas' tracks, when there are more. */
	if (style->areas != NULL) {
		if (columns && style->areas->columns > count)
			count = style->areas->columns;
		if (!columns && style->areas->rows > count)
			count = style->areas->rows;
	}

	/* The explicit tracks. */
	return count;
}

/*
 * Finds the line of a name (with a suffix, NULL for none) that a count
 * picks: the count-th from the first line, or from the last when it is
 * negative.  Implicit lines after the explicit grid all have the name
 * when the explicit ones run out (a suffixed name looked up for an area
 * does not; 0 then means none).
 */
static int
grid_names_find(
	const struct css_style *style,
	int columns,
	const struct vm_string *name,
	const char *suffix,
	int count)
{
	int last;
	int line;
	int found;
	int has;

	/* The explicit lines are 1 to last. */
	last = grid_names_explicit(style, columns) + 1;

	/* Counting from the first line. */
	if (count > 0) {
		found = 0;
		for (line = 1; line <= last; line++) {
			has = grid_names_has(style, columns, line, name, suffix);
			if (!has)
				continue;
			found++;
			if (found == count)
				return line;
		}

		/* An area's edge that is not there; otherwise the implicit lines carry on the count. */
		if (suffix != NULL)
			return 0;
		return last + (count - found);
	}

	/* Counting from the last line. */
	found = 0;
	for (line = last; line >= 1; line--) {
		has = grid_names_has(style, columns, line, name, suffix);
		if (!has)
			continue;
		found--;
		if (found == count)
			return line;
	}

	/* Lines before the grid are not made: the first line. */
	return 1;
}

/*
 * Counts count lines of a name from a definite line, on (forward) or
 * back; implicit lines after the grid all have the name, and a count
 * back that runs out stops at the first line.
 */
static int
grid_names_count_from(
	const struct css_style *style,
	int columns,
	const struct vm_string *name,
	int from,
	int count,
	int forward)
{
	int last;
	int line;
	int found;
	int has;

	/* The explicit lines are 1 to last. */
	last = grid_names_explicit(style, columns) + 1;
	found = 0;

	/* Forward: the explicit lines after the start, then the implicit ones. */
	if (forward) {
		for (line = from + 1; line <= last; line++) {
			has = grid_names_has(style, columns, line, name, NULL);
			if (!has)
				continue;
			found++;
			if (found == count)
				return line;
		}
		if (from + 1 > last)
			return from + (count - found);
		return last + (count - found);
	}

	/* Back from the end. */
	for (line = from - 1; line >= 1; line--) {
		has = grid_names_has(style, columns, line, name, NULL);
		if (!has)
			continue;
		found++;
		if (found == count)
			return line;
	}

	/* Lines before the grid are not made: the first line. */
	return 1;
}

/* Tells whether an explicit line has a name (with a suffix, NULL for none). */
static int
grid_names_has(
	const struct css_style *style,
	int columns,
	int line,
	const struct vm_string *name,
	const char *suffix)
{
	const struct css_line_names *names;
	int tracks;
	int index;
	int same;

	/* The template's names of the line. */
	names = style->row_names;
	tracks = style->row_count;
	if (columns) {
		names = style->column_names;
		tracks = style->column_count;
	}
	if (names != NULL && line - 1 <= tracks) {
		for (index = 0; index < names[line - 1].count; index++) {
			same = grid_names_equal(names[line - 1].names[index], name, suffix);
			if (same)
				return 1;
		}
	}

	/* The implicit NAME-start and NAME-end of the named areas. */
	same = grid_names_area_edge(style, columns, line, name, suffix);
	if (same)
		return 1;

	/* The line does not have the name. */
	return 0;
}

/*
 * Tells whether a line is a named area's NAME-start or NAME-end line and
 * that is the name looked for (with its suffix).
 */
static int
grid_names_area_edge(
	const struct css_style *style,
	int columns,
	int line,
	const struct vm_string *name,
	const char *suffix)
{
	const struct css_grid_areas *areas;
	struct vm_string *cell;
	int row;
	int column;
	int first;
	int after;
	int same;

	/* No areas, no implicit names. */
	areas = style->areas;
	if (areas == NULL)
		return 0;

	/* Each area by its cells: the first line before its first cell, the last after its last. */
	for (row = 0; row < areas->rows; row++) {
		for (column = 0; column < areas->columns; column++) {
			cell = areas->cells[row * CSS_AREA_TRACKS + column];
			if (cell == NULL)
				continue;

			/* The cell's lines on this axis. */
			first = row + 1;
			if (columns)
				first = column + 1;
			after = first + 1;

			/* NAME-start before the cell when no cell of the area comes before it. */
			if (line == first) {
				if (columns && column > 0 && areas->cells[row * CSS_AREA_TRACKS + column - 1] == cell)
					continue;
				if (!columns && row > 0 && areas->cells[(row - 1) * CSS_AREA_TRACKS + column] == cell)
					continue;
				same = grid_names_edge(name, suffix, cell, "-start");
				if (same)
					return 1;
			}

			/* NAME-end after the cell when no cell of the area comes after it. */
			if (line == after) {
				if (columns && column + 1 < areas->columns && areas->cells[row * CSS_AREA_TRACKS + column + 1] == cell)
					continue;
				if (!columns && row + 1 < areas->rows && areas->cells[(row + 1) * CSS_AREA_TRACKS + column] == cell)
					continue;
				same = grid_names_edge(name, suffix, cell, "-end");
				if (same)
					return 1;
			}
		}
	}

	/* No area has the line on its edge with that name. */
	return 0;
}

/*
 * Tells whether an area's implicit line name (the area's name and an edge,
 * -start or -end) is the name looked for (the name and its suffix).
 */
static int
grid_names_edge(
	const struct vm_string *name,
	const char *suffix,
	const struct vm_string *area,
	const char *edge)
{
	int same;

	/* A suffixed name: the area's own name with the same edge. */
	if (suffix != NULL) {
		if (name != area)
			return 0;
		same = strcmp(suffix, edge);
		if (same != 0)
			return 0;
		return 1;
	}

	/* A plain name: the area's name with the edge written out. */
	same = grid_names_equal(name, area, edge);
	if (!same)
		return 0;

	/* Succeeded: the line has the name. */
	return 1;
}

/* Tells whether an atom is base followed by suffix (NULL for none). */
static int
grid_names_equal(
	const struct vm_string *atom,
	const struct vm_string *base,
	const char *suffix)
{
	size_t extra;
	size_t index;

	/* The same atom when there is no suffix. */
	if (suffix == NULL)
		return atom == base;

	/* The base's characters, then the suffix's. */
	extra = strlen(suffix);
	if (atom->length != base->length + extra)
		return 0;
	for (index = 0; index < base->length; index++) {
		if (vm_string_at(atom, index) != vm_string_at(base, index))
			return 0;
	}
	for (index = 0; index < extra; index++) {
		if (vm_string_at(atom, base->length + index) != (uint16_t)(unsigned char)suffix[index])
			return 0;
	}

	/* Succeeded: the atom is the suffixed base. */
	return 1;
}
