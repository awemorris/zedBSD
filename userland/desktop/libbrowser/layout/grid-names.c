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

/* A line left auto while lines are worked out (a real line can be 0 or negative: before the grid). */
#define GRID_NAMES_AUTO	(-1000000)

static int grid_names_explicit(const struct css_style *style, int columns);
static int grid_names_has(const struct css_style *style, int columns, int line, const struct vm_string *name, const char *suffix);
static int grid_names_template_has(const struct css_line_names *names, int tracks, const struct css_track_repeat *repeat, int line, const struct vm_string *name, const char *suffix);
static int grid_names_range(const struct css_line_names *names, int first, int last, const struct vm_string *name, const char *suffix);
static int grid_names_area_edge(const struct css_style *style, int columns, int line, const struct vm_string *name, const char *suffix);
static int grid_names_equal(const struct vm_string *atom, const struct vm_string *base, const char *suffix);
static int grid_names_edge(const struct vm_string *name, const char *suffix, const struct vm_string *area, const char *edge);
static int grid_names_find(const struct css_style *style, int columns, const struct vm_string *name, const char *suffix, int count);
static int grid_names_count_from(const struct css_style *style, int columns, const struct vm_string *name, int from, int count, int forward);
static int grid_names_axis(const struct css_style *style, int columns, struct css_grid_place *place);
static void grid_names_prepend(struct layout_box *box, int columns, int count);
static void grid_names_settle(struct css_grid_place *place);

/*
 * Resolves the lines of a grid container's children to positive line
 * numbers on both axes: named lines and spans, and line numbers counted
 * from the end.  A line before the explicit grid makes implicit tracks
 * before it: the template gets them in front (sized like implicit tracks)
 * and every line moves on by as many.  The children's styles are the
 * layout's own.
 */
void
layout_grid_names(
	struct layout_box *box)
{
	struct layout_box *child;
	int column_first;
	int row_first;
	int first;

	/* Each child's lines, and the first line any item in the flow reaches. */
	column_first = 1;
	row_first = 1;
	for (child = box->first_child; child != NULL; child = child->next) {
		first = grid_names_axis(&box->style, 1, &child->style.grid_column);
		if (!child->out_of_flow && first < column_first)
			column_first = first;
		first = grid_names_axis(&box->style, 0, &child->style.grid_row);
		if (!child->out_of_flow && first < row_first)
			row_first = first;
	}

	/* Implicit tracks before the explicit grid, and every line moved past them. */
	if (column_first < 1)
		grid_names_prepend(box, 1, 1 - column_first);
	if (row_first < 1)
		grid_names_prepend(box, 0, 1 - row_first);

	/* What is still at or before line 0 is out of the grid: auto. */
	for (child = box->first_child; child != NULL; child = child->next) {
		grid_names_settle(&child->style.grid_column);
		grid_names_settle(&child->style.grid_row);
	}
}

/*
 * Resolves one axis of a place to line numbers (definite lines may be 0 or
 * negative: before the explicit grid; auto is GRID_NAMES_AUTO) and reports
 * the first line the place reaches (1 when it reaches none before the grid).
 */
static int
grid_names_axis(
	const struct css_style *style,
	int columns,
	struct css_grid_place *place)
{
	int explicit_lines;
	int count;
	int first;

	/* Numbered lines counted from the end are counted from the start. */
	explicit_lines = grid_names_explicit(style, columns) + 1;
	if (place->start_name == NULL && place->start_span == 0) {
		if (place->start < 0)
			place->start = explicit_lines + 1 + place->start;
		else if (place->start == 0)
			place->start = GRID_NAMES_AUTO;
	}
	if (place->end_name == NULL && place->end_span == 0) {
		if (place->end < 0)
			place->end = explicit_lines + 1 + place->end;
		else if (place->end == 0)
			place->end = GRID_NAMES_AUTO;
	}

	/* A named start line: NAME-start for a name alone, else the count-th line of the name. */
	if (place->start_name != NULL && place->start_span == 0) {
		count = place->start;
		if (count == 0) {
			count = grid_names_find(style, columns, place->start_name, "-start", 1);
			if (count == GRID_NAMES_AUTO)
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
			if (count == GRID_NAMES_AUTO)
				count = grid_names_find(style, columns, place->end_name, NULL, 1);
		} else {
			count = grid_names_find(style, columns, place->end_name, NULL, count);
		}
		place->end = count;
		place->end_name = NULL;
	}

	/* A span on an auto side keeps it auto. */
	if (place->start_span != 0 && place->start_name == NULL)
		place->start = GRID_NAMES_AUTO;
	if (place->end_span != 0 && place->end_name == NULL)
		place->end = GRID_NAMES_AUTO;

	/* A named start span counts back from a definite end. */
	if (place->start_name != NULL) {
		place->start = GRID_NAMES_AUTO;
		if (place->end != GRID_NAMES_AUTO) {
			place->start = grid_names_count_from(style, columns, place->start_name, place->end, place->start_span, 0);
			place->start_span = 0;
		} else {
			place->start_span = 1;
		}
		place->start_name = NULL;
	}

	/* A named end span counts on from a definite start. */
	if (place->end_name != NULL) {
		place->end = GRID_NAMES_AUTO;
		if (place->start != GRID_NAMES_AUTO) {
			place->end = grid_names_count_from(style, columns, place->end_name, place->start, place->end_span, 1);
			place->end_span = 0;
		} else {
			place->end_span = 1;
		}
		place->end_name = NULL;
	}

	/* The first line reached: the definite lines, and a span back from a definite end. */
	first = 1;
	if (place->start != GRID_NAMES_AUTO && place->start < first)
		first = place->start;
	if (place->end != GRID_NAMES_AUTO && place->end < first)
		first = place->end;
	if (place->start == GRID_NAMES_AUTO && place->end != GRID_NAMES_AUTO && place->start_span > 0 &&
	    place->end - place->start_span < first)
		first = place->end - place->start_span;
	if (place->start == GRID_NAMES_AUTO && place->end != GRID_NAMES_AUTO && place->start_span == 0 &&
	    place->end_span == 0 && place->end - 1 < first)
		first = place->end - 1;

	/* The first line. */
	return first;
}

/*
 * Puts count implicit tracks before an axis's template (the implicit track
 * sizes, from the last backwards) and moves every child's lines past them.
 */
static void
grid_names_prepend(
	struct layout_box *box,
	int columns,
	int count)
{
	struct css_style *style;
	struct layout_box *child;
	struct css_grid_place *place;
	struct css_track *tracks;
	struct css_track_repeat *repeat;
	const struct css_track *pattern;
	int *track_count;
	int pattern_count;
	int index;

	/* The axis. */
	style = &box->style;
	tracks = style->rows;
	track_count = &style->row_count;
	repeat = &style->row_repeat;
	pattern = style->auto_rows;
	pattern_count = style->auto_row_count;
	if (columns) {
		tracks = style->columns;
		track_count = &style->column_count;
		repeat = &style->column_repeat;
		pattern = style->auto_columns;
		pattern_count = style->auto_column_count;
	}

	/* No more tracks than the template keeps. */
	if (count > CSS_TRACKS - *track_count)
		count = CSS_TRACKS - *track_count;
	if (count <= 0)
		return;

	/* The template moves on; the tracks in front cycle the implicit sizes backwards. */
	memmove(tracks + count, tracks, (size_t)*track_count * sizeof(*tracks));
	for (index = 0; index < count; index++) {
		memset(&tracks[index], 0, sizeof(tracks[index]));
		tracks[index].kind = CSS_TRACK_AUTO;
		tracks[index].minimum.unit = CSS_UNIT_AUTO;
		if (pattern_count > 0)
			tracks[index] = pattern[(pattern_count - (count - index) % pattern_count) % pattern_count];
	}
	*track_count += count;
	if (repeat->length > 0)
		repeat->at += count;

	/* Every definite line moves on as far. */
	for (child = box->first_child; child != NULL; child = child->next) {
		place = &child->style.grid_row;
		if (columns)
			place = &child->style.grid_column;
		if (place->start != GRID_NAMES_AUTO)
			place->start += count;
		if (place->end != GRID_NAMES_AUTO)
			place->end += count;
	}
}

/* Leaves a place in the grid layout's terms: lines from 1, 0 for auto (a line still before the grid is auto). */
static void
grid_names_settle(
	struct css_grid_place *place)
{
	/* The start. */
	if (place->start == GRID_NAMES_AUTO || place->start < 1)
		place->start = 0;

	/* The end. */
	if (place->end == GRID_NAMES_AUTO || place->end < 1)
		place->end = 0;
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
 * when the explicit ones run out, and so do those before it counting
 * back (a suffixed name looked up for an area does not; GRID_NAMES_AUTO
 * then means none).
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
			return GRID_NAMES_AUTO;
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

	/* The implicit lines before the grid carry on the count. */
	if (suffix != NULL)
		return GRID_NAMES_AUTO;
	return 1 + (count - found);
}

/*
 * Counts count lines of a name from a definite line, on (forward) or
 * back; the implicit lines before and after the grid all have the name.
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

	/* The implicit lines before the grid carry on the count. */
	if (from < 1)
		return from - count;
	return 1 - (count - found);
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
	const struct css_track_repeat *repeat;
	int tracks;
	int same;

	/* The template's names of the line (in the template as written, before the repeats were counted). */
	names = style->row_names;
	tracks = style->row_count;
	repeat = &style->row_repeat;
	if (columns) {
		names = style->column_names;
		tracks = style->column_count;
		repeat = &style->column_repeat;
	}
	if (names != NULL) {
		same = grid_names_template_has(names, tracks, repeat, line - 1, name, suffix);
		if (same)
			return 1;
	}

	/* The implicit NAME-start and NAME-end of the named areas. */
	same = grid_names_area_edge(style, columns, line, name, suffix);
	if (same)
		return 1;

	/* The line does not have the name. */
	return 0;
}

/*
 * Tells whether a line of a template (index from 0, after layout put an
 * automatic repeat's tracks in count times) has a name: a line inside the
 * repeat has the names of its line in the repeat as written, and a line
 * between two repetitions the repeat's own last-line and first-line names.
 */
static int
grid_names_template_has(
	const struct css_line_names *names,
	int tracks,
	const struct css_track_repeat *repeat,
	int line,
	const struct vm_string *name,
	const char *suffix)
{
	int original;
	int written;
	int offset;
	int end;
	int same;

	/* The line in the template as written. */
	original = line;
	if (repeat->length > 0 && repeat->count > 1) {
		end = repeat->at + repeat->length * repeat->count;
		if (line > repeat->at && line < end) {
			offset = (line - repeat->at) % repeat->length;
			if (offset == 0) {
				/* Between two repetitions: the last line's own names, then the first line's. */
				same = grid_names_range(&names[repeat->at + repeat->length], 0, repeat->name_end, name, suffix);
				if (same)
					return 1;
				same = grid_names_range(&names[repeat->at], repeat->name_prefix, names[repeat->at].count, name, suffix);
				return same;
			}
			original = repeat->at + offset;
		} else if (line >= end) {
			original = line - repeat->length * (repeat->count - 1);
		}
	}

	/* Lines past the template as written have no names. */
	written = tracks;
	if (repeat->length > 0 && repeat->count > 1)
		written = tracks - repeat->length * (repeat->count - 1);
	if (original < 0 || original > written || original > CSS_TRACKS)
		return 0;

	/* Succeeded: whether the written line has the name. */
	same = grid_names_range(&names[original], 0, names[original].count, name, suffix);
	return same;
}

/* Tells whether one of a line's names, from first up to last, is the name looked for (with its suffix). */
static int
grid_names_range(
	const struct css_line_names *names,
	int first,
	int last,
	const struct vm_string *name,
	const char *suffix)
{
	int index;
	int same;

	/* Each name in the range. */
	if (last > names->count)
		last = names->count;
	for (index = first; index < last; index++) {
		same = grid_names_equal(names->names[index], name, suffix);
		if (same)
			return 1;
	}

	/* None of them. */
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
