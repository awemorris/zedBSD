/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of grid placement values: grid lines (grid-row-start and the
 * other three, and the grid-row, grid-column and grid-area shorthands),
 * line names and grid-template-areas, written the way CSSOM serializes
 * them.  The computed declaration writes a style's values with them, and
 * the inline declaration a declared value's canonical form.
 *
 * Shorthands leave out the trailing lines their grammar would give back:
 * an end that is the start's name alone (or auto after anything else).
 * https://drafts.csswg.org/css-grid-2/#placement-shorthands
 * https://drafts.csswg.org/cssom/#serialize-an-identifier
 */

#include "css/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int grid_text_ascii(const char *ascii, struct wb_units *out);
static int grid_text_is_name(const struct css_grid_place *place, int end);
static int grid_text_left_out(const struct css_grid_place *place, int end, const struct css_grid_place *other, int other_end);
static int grid_text_place(const struct css_value *value, struct css_grid_place *place, int end);
static int grid_text_name_is(const uint16_t *units, size_t length, const char *name);
static int grid_text_template(const uint16_t *units, size_t length, struct wb_units *out);
static int grid_text_piece(const uint16_t *units, const struct css_token *tokens, size_t first, size_t last, struct wb_units *out);
static int grid_text_string(const struct css_token *token, struct wb_units *out);
static size_t grid_text_component(const struct css_token *tokens, size_t count, size_t index);

/*
 * Writes an identifier as CSSOM serializes one.
 *
 * NUL becomes U+FFFD; a control character, and a digit that starts the
 * identifier or follows its starting hyphen, are escaped as a code point;
 * a hyphen alone and any other character that is not a name character are
 * escaped with a backslash.
 */
int
css_serialize_ident(
	const struct vm_string *name,
	struct wb_units *out)
{
	char escape[16];
	size_t index;
	uint16_t unit;
	uint16_t first;
	int code_point;
	int plain;
	int error;

	/* Each character, looking back at the first. */
	first = 0;
	if (name->length > 0)
		first = vm_string_at(name, 0);
	for (index = 0; index < name->length; index++) {
		unit = vm_string_at(name, index);

		/* NUL becomes the replacement character. */
		if (unit == 0) {
			unit = 0xfffd;
			error = wb_units_append(out, &unit, 1);
			if (error != 0)
				return error;
			continue;
		}

		/* Controls, and a digit at the start or after a starting hyphen, are code points. */
		code_point = 0;
		if ((unit >= 0x01 && unit <= 0x1f) || unit == 0x7f)
			code_point = 1;
		else if (index == 0 && unit >= '0' && unit <= '9')
			code_point = 1;
		else if (index == 1 && first == '-' && unit >= '0' && unit <= '9')
			code_point = 1;
		if (code_point) {
			snprintf(escape, sizeof(escape), "\\%x ", (unsigned)unit);
			error = grid_text_ascii(escape, out);
			if (error != 0)
				return error;
			continue;
		}

		/* A hyphen alone is escaped. */
		if (index == 0 && unit == '-' && name->length == 1) {
			error = grid_text_ascii("\\-", out);
			if (error != 0)
				return error;
			continue;
		}

		/* Name characters stay as they are. */
		plain = 0;
		if (unit >= 0x80 || unit == '-' || unit == '_')
			plain = 1;
		else if ((unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z'))
			plain = 1;
		if (plain) {
			error = wb_units_append(out, &unit, 1);
			if (error != 0)
				return error;
			continue;
		}

		/* Anything else is escaped. */
		escape[0] = '\\';
		escape[1] = (char)unit;
		escape[2] = '\0';
		error = grid_text_ascii(escape, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the identifier is written. */
	return 0;
}

/*
 * Writes the names of one grid line in brackets.
 */
int
css_line_names_serialize(
	const struct css_line_names *names,
	struct wb_units *out)
{
	int index;
	int error;

	/* The opening bracket. */
	error = grid_text_ascii("[", out);
	if (error != 0)
		return error;

	/* The names, a space apart. */
	for (index = 0; index < names->count; index++) {
		if (index > 0) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
		}
		error = css_serialize_ident(names->names[index], out);
		if (error != 0)
			return error;
	}

	/* The closing bracket. */
	error = grid_text_ascii("]", out);
	if (error != 0)
		return error;

	/* Succeeded: the names are written. */
	return 0;
}

/*
 * Writes one grid line of a place (its end when end is nonzero).
 *
 * auto, a number, a name, a number and a name, or span with its count
 * and its name (a count of one is left out when there is a name).
 */
int
css_grid_line_serialize(
	const struct css_grid_place *place,
	int end,
	struct wb_units *out)
{
	struct vm_string *name;
	char text[32];
	int line;
	int span;
	int error;

	/* The side's parts. */
	line = place->start;
	span = place->start_span;
	name = place->start_name;
	if (end) {
		line = place->end;
		span = place->end_span;
		name = place->end_name;
	}

	/* A span: span, its count unless it is one with a name, then its name. */
	if (span > 0) {
		if (span != 1 || name == NULL) {
			snprintf(text, sizeof(text), "span %d", span);
		} else {
			snprintf(text, sizeof(text), "span");
		}
		error = grid_text_ascii(text, out);
		if (error != 0)
			return error;

		/* The name after the count. */
		if (name != NULL) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
			error = css_serialize_ident(name, out);
			if (error != 0)
				return error;
		}

		/* Succeeded: the span is written. */
		return 0;
	}

	/* auto has neither a number nor a name. */
	if (line == 0 && name == NULL) {
		error = grid_text_ascii("auto", out);
		if (error != 0)
			return error;

		/* Succeeded: auto is written. */
		return 0;
	}

	/* The number, then a space before the name. */
	if (line != 0) {
		snprintf(text, sizeof(text), "%d", line);
		error = grid_text_ascii(text, out);
		if (error != 0)
			return error;
		if (name != NULL) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
		}
	}

	/* The name. */
	if (name != NULL) {
		error = css_serialize_ident(name, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the line is written. */
	return 0;
}

/*
 * Writes grid-row or grid-column: the start, then a slash and the end
 * unless the end is the one the shorthand gives when it is left out.
 */
int
css_grid_lines_serialize(
	const struct css_grid_place *place,
	struct wb_units *out)
{
	int left_out;
	int error;

	/* The start. */
	error = css_grid_line_serialize(place, 0, out);
	if (error != 0)
		return error;

	/* An end the shorthand would give back is left out. */
	left_out = grid_text_left_out(place, 1, place, 0);
	if (left_out)
		return 0;

	/* The slash and the end. */
	error = grid_text_ascii(" / ", out);
	if (error != 0)
		return error;
	error = css_grid_line_serialize(place, 1, out);
	if (error != 0)
		return error;

	/* Succeeded: both lines are written. */
	return 0;
}

/*
 * Writes grid-area: row start / column start / row end / column end,
 * leaving out the trailing ones the shorthand would give back.
 */
int
css_grid_area_serialize(
	const struct css_grid_place *row,
	const struct css_grid_place *column,
	struct wb_units *out)
{
	const struct css_grid_place *places[4];
	int ends[4];
	int parts;
	int part;
	int left_out;
	int error;

	/* The four lines in the shorthand's order. */
	places[0] = row;
	ends[0] = 0;
	places[1] = column;
	ends[1] = 0;
	places[2] = row;
	ends[2] = 1;
	places[3] = column;
	ends[3] = 1;

	/*
	 * How many say something: the column end goes when the column start
	 * gives it back, then the row end when the row start does, then the
	 * column start when the row start does.
	 */
	parts = 4;
	left_out = grid_text_left_out(column, 1, column, 0);
	if (left_out) {
		parts = 3;
		left_out = grid_text_left_out(row, 1, row, 0);
		if (left_out) {
			parts = 2;
			left_out = grid_text_left_out(column, 0, row, 0);
			if (left_out)
				parts = 1;
		}
	}

	/* The lines that say something, a slash apart. */
	for (part = 0; part < parts; part++) {
		if (part > 0) {
			error = grid_text_ascii(" / ", out);
			if (error != 0)
				return error;
		}
		error = css_grid_line_serialize(places[part], ends[part], out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the area is written. */
	return 0;
}

/*
 * Writes grid-template-areas: none, or each row in quotes with its cells
 * a space apart (a full stop for a cell of no area).
 */
int
css_grid_areas_serialize(
	const struct css_grid_areas *areas,
	struct wb_units *out)
{
	struct vm_string *name;
	int row;
	int column;
	int error;

	/* No areas are none. */
	if (areas == NULL) {
		error = grid_text_ascii("none", out);
		if (error != 0)
			return error;

		/* Succeeded: none is written. */
		return 0;
	}

	/* Each row in quotes, after a space from the one before. */
	for (row = 0; row < areas->rows; row++) {
		if (row > 0) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
		}
		error = grid_text_ascii("\"", out);
		if (error != 0)
			return error;

		/* Its cells. */
		for (column = 0; column < areas->columns; column++) {
			if (column > 0) {
				error = grid_text_ascii(" ", out);
				if (error != 0)
					return error;
			}
			name = areas->cells[row * CSS_AREA_TRACKS + column];
			if (name == NULL) {
				error = grid_text_ascii(".", out);
			} else {
				error = vm_string_append_units(name, out);
			}
			if (error != 0)
				return error;
		}
		error = grid_text_ascii("\"", out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the rows are written. */
	return 0;
}

/*
 * Writes the canonical value of a grid placement or grid-template-areas
 * declaration ("name: value").
 *
 * handled is 0, and nothing is written, for another property, for an
 * invalid value and for a CSS-wide keyword.
 */
int
css_grid_declaration_serialize(
	struct vm_heap *heap,
	const uint16_t *units,
	size_t length,
	struct wb_units *out,
	int *handled)
{
	struct css_declaration *declarations;
	struct css_grid_place row;
	struct css_grid_place column;
	struct wb_arena arena;
	size_t count;
	size_t index;
	int property;
	int kind;
	int is_template;
	int error;

	/* The declarations the value makes, in an arena of their own. */
	*handled = 0;
	wb_arena_init(&arena, 0);
	error = css_parse_declarations(heap, &arena, units, length, &declarations, &count);
	if (error == ENOMEM) {
		wb_arena_release(&arena);
		return error;
	}

	/* Only a value that parsed is written here (and not a CSS-wide keyword). */
	if (error != 0 || count == 0) {
		wb_arena_release(&arena);
		return 0;
	}
	kind = declarations[0].value.kind;
	if (kind == CSS_VALUE_INHERIT || kind == CSS_VALUE_INITIAL || kind == CSS_VALUE_UNSET) {
		wb_arena_release(&arena);
		return 0;
	}

	/* grid-template and grid are written from their text. */
	is_template = grid_text_name_is(units, length, "grid-template");
	if (!is_template)
		is_template = grid_text_name_is(units, length, "grid");
	if (is_template) {
		wb_arena_release(&arena);
		error = grid_text_template(units, length, out);
		if (error != 0)
			return error;

		/* Succeeded: the shorthand's canonical text is written. */
		*handled = 1;
		return 0;
	}

	/* grid-template-areas is one value. */
	if (count == 1 && declarations[0].property == CSS_PROP_GRID_TEMPLATE_AREAS) {
		if (declarations[0].value.kind == CSS_VALUE_AREAS) {
			error = css_grid_areas_serialize(declarations[0].value.areas, out);
			*handled = 1;
		}
		wb_arena_release(&arena);
		if (error != 0)
			return error;

		/* Succeeded: the areas are written, or left to the caller. */
		return 0;
	}

	/* The lines the declarations give, all of them grid lines. */
	memset(&row, 0, sizeof(row));
	memset(&column, 0, sizeof(column));
	for (index = 0; index < count; index++) {
		property = declarations[index].property;
		kind = declarations[index].value.kind;
		if (kind != CSS_VALUE_GRID_LINE || property < CSS_PROP_GRID_COLUMN_START || property > CSS_PROP_GRID_ROW_END) {
			wb_arena_release(&arena);
			return 0;
		}
		if (property == CSS_PROP_GRID_COLUMN_START)
			grid_text_place(&declarations[index].value, &column, 0);
		else if (property == CSS_PROP_GRID_COLUMN_END)
			grid_text_place(&declarations[index].value, &column, 1);
		else if (property == CSS_PROP_GRID_ROW_START)
			grid_text_place(&declarations[index].value, &row, 0);
		else
			grid_text_place(&declarations[index].value, &row, 1);
	}

	/* One longhand, a pair for grid-row or grid-column, or the four of grid-area. */
	property = declarations[0].property;
	if (count == 1) {
		error = css_grid_line_serialize(property <= CSS_PROP_GRID_COLUMN_END ? &column : &row,
		    property == CSS_PROP_GRID_COLUMN_END || property == CSS_PROP_GRID_ROW_END, out);
	} else if (count == 2 && property == CSS_PROP_GRID_COLUMN_START) {
		error = css_grid_lines_serialize(&column, out);
	} else if (count == 2) {
		error = css_grid_lines_serialize(&row, out);
	} else {
		error = css_grid_area_serialize(&row, &column, out);
	}
	wb_arena_release(&arena);
	if (error != 0)
		return error;

	/* Succeeded: the placement is written. */
	*handled = 1;
	return 0;
}

/* Tells whether a declaration's text ("name: value") is of the named property (ASCII, any case). */
static int
grid_text_name_is(
	const uint16_t *units,
	size_t length,
	const char *name)
{
	size_t index;
	size_t at;
	uint16_t unit;

	/* Leading whitespace. */
	at = 0;
	while (at < length && (units[at] == ' ' || units[at] == '\t' || units[at] == '\n'))
		at++;

	/* The name's characters. */
	for (index = 0; name[index] != '\0'; index++) {
		if (at >= length)
			return 0;
		unit = units[at];
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit - 'A' + 'a');
		if (unit != (uint16_t)(unsigned char)name[index])
			return 0;
		at++;
	}

	/* Then whitespace and the colon. */
	while (at < length && (units[at] == ' ' || units[at] == '\t' || units[at] == '\n'))
		at++;
	if (at >= length || units[at] != ':')
		return 0;

	/* Succeeded: the declaration is of the property. */
	return 1;
}

/*
 * Writes a grid-template or grid value in canonical form from its text:
 * the components a space apart, adjacent line-name groups merged and empty
 * ones dropped, an auto row size after a string dropped, area strings
 * written as their cells, a unitless zero as 0px, dense after auto-flow,
 * and none / none as none.
 */
static int
grid_text_template(
	const uint16_t *units,
	size_t length,
	struct wb_units *out)
{
	struct css_token *tokens;
	struct wb_arena arena;
	size_t count;
	size_t start;
	size_t index;
	size_t next;
	size_t names[64];
	size_t name_count;
	size_t name;
	size_t slash;
	size_t written;
	int previous_string;
	int has_string;
	int is_word;
	int error;

	/* The value after the colon, as tokens. */
	start = 0;
	while (start < length && units[start] != ':')
		start++;
	start++;
	wb_arena_init(&arena, 0);
	error = css_tokenize(&arena, units + start, length - start, &tokens, &count);
	if (error != 0) {
		wb_arena_release(&arena);
		return error;
	}
	while (count > 0 && (tokens[count - 1U].type == CSS_TOKEN_EOF || tokens[count - 1U].type == CSS_TOKEN_WHITESPACE))
		count--;

	/* Where the slash is, and whether the rows have strings. */
	slash = count;
	has_string = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_DELIM && tokens[index].delim == '/') {
			slash = index;
			break;
		}
		if (tokens[index].type == CSS_TOKEN_STRING)
			has_string = 1;
	}

	/* none / none is none. */
	index = 0;
	next = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index < slash && slash < count && tokens[index].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[index], "none")) {
		next = slash + 1U;
		while (next < count && tokens[next].type == CSS_TOKEN_WHITESPACE)
			next++;
		if (next + 1U == count && tokens[next].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[next], "none")) {
			error = grid_text_ascii("none", out);
			wb_arena_release(&arena);
			return error;
		}
	}

	/* The components in order. */
	written = 0;
	name_count = 0;
	previous_string = 0;
	index = 0;
	while (index <= count) {
		/* Whitespace separates components. */
		if (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* A group of names joins the ones just before it. */
		if (index < count && tokens[index].type == CSS_TOKEN_OPEN_SQUARE) {
			for (index++; index < count && tokens[index].type != CSS_TOKEN_CLOSE_SQUARE; index++) {
				if (tokens[index].type == CSS_TOKEN_IDENT && name_count < 64U) {
					names[name_count] = index;
					name_count++;
				}
			}
			index++;
			continue;
		}

		/* Names gathered before this component are written first, merged. */
		if (name_count > 0) {
			if (written > 0) {
				error = grid_text_ascii(" ", out);
				if (error != 0)
					break;
			}
			error = grid_text_ascii("[", out);
			for (name = 0; name < name_count && error == 0; name++) {
				if (name > 0)
					error = grid_text_ascii(" ", out);
				if (error == 0)
					error = wb_units_append(out, units + start + tokens[names[name]].source_start,
					    tokens[names[name]].source_end - tokens[names[name]].source_start);
			}
			if (error == 0)
				error = grid_text_ascii("]", out);
			if (error != 0)
				break;
			written++;
			name_count = 0;
			previous_string = 0;
		}
		if (index >= count)
			break;

		/* An auto size after a row's string is the default and goes. */
		is_word = tokens[index].type == CSS_TOKEN_IDENT;
		if (is_word && has_string && index < slash && previous_string && css_ident_equal(&tokens[index], "auto")) {
			previous_string = 0;
			index++;
			continue;
		}

		/* dense before auto-flow is written after it. */
		next = index + 1U;
		while (next < count && tokens[next].type == CSS_TOKEN_WHITESPACE)
			next++;
		if (is_word && next < count && css_ident_equal(&tokens[index], "dense") &&
		    tokens[next].type == CSS_TOKEN_IDENT && css_ident_equal(&tokens[next], "auto-flow")) {
			if (written > 0) {
				error = grid_text_ascii(" ", out);
				if (error != 0)
					break;
			}
			error = grid_text_ascii("auto-flow dense", out);
			if (error != 0)
				break;
			written++;
			index = next + 1U;
			continue;
		}

		/* Any other component as written (a string as its cells). */
		next = grid_text_component(tokens, count, index);
		if (written > 0) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				break;
		}
		error = grid_text_piece(units + start, tokens, index, next, out);
		if (error != 0)
			break;
		written++;
		previous_string = tokens[index].type == CSS_TOKEN_STRING;
		index = next;
	}

	/* The tokens are no longer needed. */
	wb_arena_release(&arena);
	if (error != 0)
		return error;

	/* Succeeded: the value is written. */
	return 0;
}

/* The index after one component: a function and its arguments, or one token. */
static size_t
grid_text_component(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	int depth;

	/* One token unless it opens a function. */
	if (tokens[index].type != CSS_TOKEN_FUNCTION)
		return index + 1U;

	/* A function runs to its closing parenthesis. */
	depth = 0;
	for (; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN) {
			depth--;
			if (depth == 0)
				return index + 1U;
		}
	}

	/* An unclosed function runs to the end. */
	return count;
}

/* Writes one component: a string as its cells, a unitless zero as 0px, anything else as written. */
static int
grid_text_piece(
	const uint16_t *units,
	const struct css_token *tokens,
	size_t first,
	size_t last,
	struct wb_units *out)
{
	int error;

	/* A string. */
	if (tokens[first].type == CSS_TOKEN_STRING) {
		error = grid_text_string(&tokens[first], out);
		return error;
	}

	/* A unitless zero. */
	if (last == first + 1U && tokens[first].type == CSS_TOKEN_NUMBER && tokens[first].number == 0) {
		error = grid_text_ascii("0px", out);
		return error;
	}

	/* The source text. */
	error = wb_units_append(out, units + tokens[first].source_start, tokens[last - 1U].source_end - tokens[first].source_start);
	if (error != 0)
		return error;

	/* Succeeded: the component is written. */
	return 0;
}

/* Writes an area string in quotes: its cell tokens a space apart, a run of full stops as one. */
static int
grid_text_string(
	const struct css_token *token,
	struct wb_units *out)
{
	static const uint16_t space = ' ';
	static const uint16_t stop = '.';
	size_t at;
	size_t begin;
	uint16_t unit;
	int cells;
	int error;

	/* The opening quote. */
	error = grid_text_ascii("\"", out);
	if (error != 0)
		return error;

	/* Each cell token. */
	cells = 0;
	at = 0;
	while (at < token->length) {
		unit = token->text[at];
		if (unit == ' ' || unit == '\t' || unit == '\n' || unit == '\r' || unit == '\f') {
			at++;
			continue;
		}
		if (cells > 0) {
			error = wb_units_append(out, &space, 1);
			if (error != 0)
				return error;
		}
		begin = at;
		if (unit == '.') {
			while (at < token->length && token->text[at] == '.')
				at++;
			error = wb_units_append(out, &stop, 1);
		} else {
			while (at < token->length && token->text[at] != ' ' && token->text[at] != '\t' &&
			    token->text[at] != '\n' && token->text[at] != '\r' && token->text[at] != '\f' && token->text[at] != '.')
				at++;
			error = wb_units_append(out, token->text + begin, at - begin);
		}
		if (error != 0)
			return error;
		cells++;
	}

	/* The closing quote. */
	error = grid_text_ascii("\"", out);
	if (error != 0)
		return error;

	/* Succeeded: the string is written. */
	return 0;
}

/* Puts a declared grid line into one side of a place. */
static int
grid_text_place(
	const struct css_value *value,
	struct css_grid_place *place,
	int end)
{
	/* The start's line, span and name. */
	if (!end) {
		place->start = (int)value->number;
		place->start_span = value->keyword;
		place->start_name = value->ident;
		return 0;
	}

	/* The end's. */
	place->end = (int)value->number;
	place->end_span = value->keyword;
	place->end_name = value->ident;
	return 0;
}

/* Tells whether one grid line of a place is a name alone. */
static int
grid_text_is_name(
	const struct css_grid_place *place,
	int end)
{
	/* The end: a name, no number, no span. */
	if (end) {
		if (place->end_name == NULL || place->end != 0 || place->end_span != 0)
			return 0;
		return 1;
	}

	/* The start. */
	if (place->start_name == NULL || place->start != 0 || place->start_span != 0)
		return 0;
	return 1;
}

/*
 * Tells whether a grid line is the one a shorthand gives when it is left
 * out after another: that other's name alone, or else auto.
 */
static int
grid_text_left_out(
	const struct css_grid_place *place,
	int end,
	const struct css_grid_place *other,
	int other_end)
{
	struct vm_string *name;
	struct vm_string *own;
	int is_name;

	/* The line's own name. */
	own = place->start_name;
	if (end)
		own = place->end_name;

	/* After a name alone, the same name alone. */
	is_name = grid_text_is_name(other, other_end);
	if (is_name) {
		name = other->start_name;
		if (other_end)
			name = other->end_name;
		is_name = grid_text_is_name(place, end);
		if (!is_name)
			return 0;
		if (own != name)
			return 0;
		return 1;
	}

	/* After anything else, auto. */
	if (own != NULL)
		return 0;
	if (end && (place->end != 0 || place->end_span != 0))
		return 0;
	if (!end && (place->start != 0 || place->start_span != 0))
		return 0;
	return 1;
}

/* Appends ASCII characters. */
static int
grid_text_ascii(
	const char *ascii,
	struct wb_units *out)
{
	uint16_t unit;
	size_t index;
	int error;

	/* Each character as a UTF-16 unit. */
	for (index = 0; ascii[index] != '\0'; index++) {
		unit = (uint16_t)(unsigned char)ascii[index];
		error = wb_units_append(out, &unit, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the characters are appended. */
	return 0;
}
