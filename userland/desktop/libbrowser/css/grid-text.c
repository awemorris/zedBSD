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

/*
 * A shorthand's canonical value cut at its top-level slashes: its text,
 * its tokens, and the tokens [first, last) of each part (count 0 when it
 * cannot be cut).
 */
struct grid_text_split {
	struct wb_units canonical;
	struct wb_arena arena;
	struct css_token *tokens;
	size_t token_count;
	size_t first[4];
	size_t last[4];
	int count;
};

static int grid_text_ascii(const char *ascii, struct wb_units *out);
static int grid_text_is_name(const struct css_grid_place *place, int end);
static int grid_text_left_out(const struct css_grid_place *place, int end, const struct css_grid_place *other, int other_end);
static int grid_text_place(const struct css_value *value, struct css_grid_place *place, int end);
static int grid_text_name_is(const uint16_t *units, size_t length, const char *name);
static int grid_text_template(const uint16_t *units, size_t length, struct wb_units *out);
static int grid_text_piece(const uint16_t *units, const struct css_token *tokens, size_t first, size_t last, struct wb_units *out);
static int grid_text_string(const struct css_token *token, struct wb_units *out);
static size_t grid_text_component(const struct css_token *tokens, size_t count, size_t index);
static int grid_text_split(struct vm_heap *heap, const struct vm_string *shorthand, const struct vm_string *value, struct grid_text_split *split);
static void grid_text_split_release(struct grid_text_split *split);
static int grid_text_split_is_name(const struct grid_text_split *split, int part);
static int grid_text_split_write(const struct grid_text_split *split, size_t first, size_t last, struct wb_units *out);
static int grid_text_line_part(struct vm_heap *heap, const struct grid_text_split *split, int row, int part, const char *longhand, struct wb_units *out, int *found);
static int grid_text_template_part(const struct grid_text_split *split, int grid, int part, struct wb_units *out, int *found);
static int grid_text_flow_sizes(const struct grid_text_split *split, int side, struct wb_units *out);
static int grid_text_template_side(const struct grid_text_split *split, int part, struct wb_units *out, int *found);

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

/*
 * The grid shorthands and their longhands, in the order of the parts the
 * shorthands give them.
 */
static const char *const grid_text_shorthands[5][8] = {
	{ "grid-row", "grid-row-start", "grid-row-end", NULL },
	{ "grid-column", "grid-column-start", "grid-column-end", NULL },
	{ "grid-area", "grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end", NULL },
	{ "grid-template", "grid-template-rows", "grid-template-columns", "grid-template-areas", NULL },
	{ "grid", "grid-template-rows", "grid-template-columns", "grid-template-areas",
	  "grid-auto-rows", "grid-auto-columns", "grid-auto-flow", NULL },
};

/*
 * The longhands of a grid shorthand (a list that ends with NULL), or NULL
 * when the name is not a grid shorthand.
 */
const char *const *
css_grid_shorthand_longhands(
	const struct vm_string *name)
{
	size_t row;

	/* The shorthand's row. */
	for (row = 0; row < 5U; row++) {
		if (vm_string_equal_ascii(name, grid_text_shorthands[row][0]))
			return &grid_text_shorthands[row][1];
	}

	/* Not a grid shorthand. */
	return NULL;
}

/*
 * Writes the value a grid shorthand's value gives one of its longhands
 * (found is 0 when the name is not one of them or the value cannot be
 * split).  The shorthand's canonical text is cut at its slashes, and each
 * part follows the shorthand's grammar: an omitted grid line is the name
 * of the line it mirrors or auto, the strings of grid-template's rows make
 * the areas (their rows auto unless sized), and auto-flow on one side of
 * grid gives that side's auto tracks and the flow.
 * https://drafts.csswg.org/css-grid-2/#grid-template
 * https://drafts.csswg.org/css-grid-2/#grid-shorthand
 */
int
css_grid_shorthand_part(
	struct vm_heap *heap,
	const struct vm_string *shorthand,
	const struct vm_string *value,
	const char *longhand,
	struct wb_units *out,
	int *found)
{
	struct grid_text_split split;
	const char *const *longhands;
	size_t row;
	int part;
	int error;

	/* The shorthand and the longhand's part. */
	*found = 0;
	longhands = css_grid_shorthand_longhands(shorthand);
	if (longhands == NULL)
		return 0;
	for (part = 0; longhands[part] != NULL; part++) {
		if (strcmp(longhands[part], longhand) == 0)
			break;
	}
	if (longhands[part] == NULL)
		return 0;
	for (row = 0; row < 5U; row++) {
		if (&grid_text_shorthands[row][1] == longhands)
			break;
	}

	/* The canonical value, cut at its slashes. */
	error = grid_text_split(heap, shorthand, value, &split);
	if (error != 0 || split.count == 0) {
		grid_text_split_release(&split);
		return error;
	}

	/* The part by the shorthand's grammar. */
	if (row <= 2U)
		error = grid_text_line_part(heap, &split, (int)row, part, longhand, out, found);
	else
		error = grid_text_template_part(&split, row == 4U, part, out, found);
	grid_text_split_release(&split);
	return error;
}

/* Cuts a shorthand's canonical value into its tokens and the parts between its top-level slashes. */
static int
grid_text_split(
	struct vm_heap *heap,
	const struct vm_string *shorthand,
	const struct vm_string *value,
	struct grid_text_split *split)
{
	struct wb_units text;
	size_t index;
	int handled;
	int depth;
	int error;

	/* The declaration's canonical value (or the value as it is). */
	memset(split, 0, sizeof(*split));
	wb_units_init(&split->canonical);
	wb_arena_init(&split->arena, 0);
	wb_units_init(&text);
	error = vm_string_append_units(shorthand, &text);
	if (error == 0)
		error = grid_text_ascii(": ", &text);
	if (error == 0)
		error = vm_string_append_units(value, &text);
	handled = 0;
	if (error == 0)
		error = css_grid_declaration_serialize(heap, text.data, text.length, &split->canonical, &handled);
	wb_units_release(&text);
	if (error == 0 && !handled) {
		split->canonical.length = 0;
		error = vm_string_append_units(value, &split->canonical);
	}
	if (error != 0)
		return error;

	/* Its tokens, without the trailing whitespace. */
	error = css_tokenize(&split->arena, split->canonical.data, split->canonical.length, &split->tokens, &split->token_count);
	if (error != 0)
		return error;
	while (split->token_count > 0 && (split->tokens[split->token_count - 1U].type == CSS_TOKEN_EOF ||
	    split->tokens[split->token_count - 1U].type == CSS_TOKEN_WHITESPACE))
		split->token_count--;

	/* The parts between the slashes outside parentheses. */
	depth = 0;
	split->first[0] = 0;
	split->count = 1;
	for (index = 0; index < split->token_count; index++) {
		if (split->tokens[index].type == CSS_TOKEN_FUNCTION || split->tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		else if (split->tokens[index].type == CSS_TOKEN_CLOSE_PAREN && depth > 0)
			depth--;
		if (depth != 0 || split->tokens[index].type != CSS_TOKEN_DELIM || split->tokens[index].delim != '/')
			continue;
		if (split->count == 4) {
			split->count = 0;
			return 0;
		}
		split->last[split->count - 1] = index;
		split->first[split->count] = index + 1U;
		split->count++;
	}
	split->last[split->count - 1] = split->token_count;

	/* Each part without its surrounding whitespace (an empty part splits nothing). */
	for (index = 0; index < (size_t)split->count; index++) {
		while (split->first[index] < split->last[index] && split->tokens[split->first[index]].type == CSS_TOKEN_WHITESPACE)
			split->first[index]++;
		while (split->last[index] > split->first[index] && split->tokens[split->last[index] - 1U].type == CSS_TOKEN_WHITESPACE)
			split->last[index]--;
		if (split->first[index] == split->last[index])
			split->count = 0;
	}

	/* Succeeded: the value is cut. */
	return 0;
}

/* Releases a cut value. */
static void
grid_text_split_release(
	struct grid_text_split *split)
{
	/* The tokens and the text. */
	wb_arena_release(&split->arena);
	wb_units_release(&split->canonical);
}

/* Tells whether a part is one custom identifier (the name an omitted line mirrors). */
static int
grid_text_split_is_name(
	const struct grid_text_split *split,
	int part)
{
	const struct css_token *token;

	/* One identifier that is not a keyword of the grammar. */
	if (split->last[part] != split->first[part] + 1U)
		return 0;
	token = &split->tokens[split->first[part]];
	if (token->type != CSS_TOKEN_IDENT)
		return 0;
	if (css_ident_equal(token, "auto") || css_ident_equal(token, "span"))
		return 0;

	/* Succeeded: the part is a name. */
	return 1;
}

/* Writes the tokens first to last of the canonical text as they are. */
static int
grid_text_split_write(
	const struct grid_text_split *split,
	size_t first,
	size_t last,
	struct wb_units *out)
{
	/* Nothing for no tokens. */
	if (last <= first)
		return 0;

	/* The source text they cover. */
	return wb_units_append(out, split->canonical.data + split->tokens[first].source_start,
	    split->tokens[last - 1U].source_end - split->tokens[first].source_start);
}

/*
 * Writes a grid line of grid-row, grid-column (row 0 or 1: start, end) or
 * grid-area (row 2: row start, column start, row end, column end).
 */
static int
grid_text_line_part(
	struct vm_heap *heap,
	const struct grid_text_split *split,
	int row,
	int part,
	const char *longhand,
	struct wb_units *out,
	int *found)
{
	struct wb_units text;
	int source;
	int handled;
	int error;

	/* The part that gives the line, or the one an omitted line mirrors. */
	source = part;
	if (part >= split->count) {
		source = -1;
		if (row <= 1 || part == 1 || part == 2)
			source = 0;
		else if (part == 3)
			source = split->count >= 2 ? 1 : 0;
		if (source >= 0 && !grid_text_split_is_name(split, source))
			source = -1;
	}

	/* An omitted line that mirrors nothing is auto. */
	*found = 1;
	if (source < 0)
		return grid_text_ascii("auto", out);

	/* The line in its own canonical form. */
	wb_units_init(&text);
	error = grid_text_ascii(longhand, &text);
	if (error == 0)
		error = grid_text_ascii(": ", &text);
	if (error == 0)
		error = grid_text_split_write(split, split->first[source], split->last[source], &text);
	handled = 0;
	if (error == 0)
		error = css_grid_declaration_serialize(heap, text.data, text.length, out, &handled);
	if (error == 0 && !handled)
		error = grid_text_split_write(split, split->first[source], split->last[source], out);
	wb_units_release(&text);
	return error;
}

/*
 * Writes a part of grid-template (rows, columns, areas) or grid (those,
 * then auto rows, auto columns and the flow).
 */
static int
grid_text_template_part(
	const struct grid_text_split *split,
	int grid,
	int part,
	struct wb_units *out,
	int *found)
{
	size_t index;
	int flow_side;
	int dense;

	/* Which side of grid has auto-flow, if either. */
	flow_side = -1;
	dense = 0;
	for (index = 0; grid && index < split->token_count; index++) {
		if (split->tokens[index].type != CSS_TOKEN_IDENT)
			continue;
		if (css_ident_equal(&split->tokens[index], "auto-flow"))
			flow_side = split->count >= 2 && index >= split->first[1] ? 1 : 0;
		if (css_ident_equal(&split->tokens[index], "dense"))
			dense = 1;
	}
	*found = 1;

	/* The auto tracks and the flow. */
	if (part == 3 || part == 4) {
		if (flow_side == part - 3)
			return grid_text_flow_sizes(split, flow_side, out);
		return grid_text_ascii("auto", out);
	}
	if (part == 5) {
		if (flow_side == 1)
			return grid_text_ascii(dense ? "column dense" : "column", out);
		return grid_text_ascii(dense ? "dense" : "row", out);
	}

	/* The template beside auto-flow: the other side's tracks, none on auto-flow's. */
	if (flow_side >= 0) {
		if (part == 2 || part == flow_side || split->count != 2)
			return grid_text_ascii("none", out);
		return grid_text_split_write(split, split->first[part], split->last[part], out);
	}

	/* The template's own forms. */
	return grid_text_template_side(split, part, out, found);
}

/* Writes the auto track sizes on auto-flow's side of grid (auto when there are none). */
static int
grid_text_flow_sizes(
	const struct grid_text_split *split,
	int side,
	struct wb_units *out)
{
	size_t index;
	size_t next;
	int written;
	int error;

	/* The components other than auto-flow and dense. */
	written = 0;
	index = split->first[side];
	while (index < split->last[side]) {
		next = grid_text_component(split->tokens, split->last[side], index);
		if (split->tokens[index].type == CSS_TOKEN_WHITESPACE ||
		    (split->tokens[index].type == CSS_TOKEN_IDENT &&
		    (css_ident_equal(&split->tokens[index], "auto-flow") || css_ident_equal(&split->tokens[index], "dense")))) {
			index = next;
			continue;
		}
		if (written > 0) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
		}
		error = grid_text_split_write(split, index, next, out);
		if (error != 0)
			return error;
		written++;
		index = next;
	}

	/* None is auto. */
	if (written == 0)
		return grid_text_ascii("auto", out);

	/* Succeeded: the sizes are written. */
	return 0;
}

/*
 * Writes the rows (0), columns (1) or areas (2) of a grid-template value:
 * none, rows / columns, or the areas form with its strings.
 */
static int
grid_text_template_side(
	const struct grid_text_split *split,
	int part,
	struct wb_units *out,
	int *found)
{
	size_t index;
	size_t next;
	int strings;
	int pending;
	int written;
	int error;

	/* Whether the rows have strings. */
	strings = 0;
	for (index = split->first[0]; index < split->last[0]; index++) {
		if (split->tokens[index].type == CSS_TOKEN_STRING)
			strings = 1;
	}

	/* none (one part) is none everywhere. */
	if (split->count == 1 && !strings) {
		if (split->last[0] != split->first[0] + 1U || split->tokens[split->first[0]].type != CSS_TOKEN_IDENT ||
		    !css_ident_equal(&split->tokens[split->first[0]], "none")) {
			*found = 0;
			return 0;
		}
		return grid_text_ascii("none", out);
	}

	/* rows / columns. */
	if (!strings) {
		if (split->count != 2 || part == 2)
			return grid_text_ascii("none", out);
		return grid_text_split_write(split, split->first[part], split->last[part], out);
	}

	/* The areas form's columns. */
	if (part == 1) {
		if (split->count < 2)
			return grid_text_ascii("none", out);
		return grid_text_split_write(split, split->first[1], split->last[1], out);
	}

	/* Its rows (an unsized string's row is auto) or its strings. */
	written = 0;
	pending = 0;
	index = split->first[0];
	while (index <= split->last[0]) {
		if (index < split->last[0] && split->tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}

		/* A string's row that no size follows is auto. */
		if (part == 0 && pending && (index == split->last[0] || split->tokens[index].type == CSS_TOKEN_STRING ||
		    split->tokens[index].type == CSS_TOKEN_OPEN_SQUARE)) {
			error = grid_text_ascii(written > 0 ? " auto" : "auto", out);
			if (error != 0)
				return error;
			written++;
			pending = 0;
		}
		if (index == split->last[0])
			break;

		/* The next component: line names, a string or a size. */
		next = grid_text_component(split->tokens, split->last[0], index);
		if (split->tokens[index].type == CSS_TOKEN_OPEN_SQUARE) {
			while (next < split->last[0] && split->tokens[next - 1U].type != CSS_TOKEN_CLOSE_SQUARE)
				next++;
		}
		if (split->tokens[index].type == CSS_TOKEN_STRING) {
			pending = 1;
			if (part == 0) {
				index = next;
				continue;
			}
		} else {
			pending = 0;
			if (part == 2) {
				index = next;
				continue;
			}
		}
		if (written > 0) {
			error = grid_text_ascii(" ", out);
			if (error != 0)
				return error;
		}
		if (split->tokens[index].type == CSS_TOKEN_STRING)
			error = grid_text_string(&split->tokens[index], out);
		else
			error = grid_text_split_write(split, index, next, out);
		if (error != 0)
			return error;
		written++;
		index = next;
	}

	/* Succeeded: the part is written. */
	return 0;
}

/* Adds a line's names (atoms) to another's, once each and no more than a line keeps. */
static void
grid_text_merge_names(
	struct css_line_names *line,
	const struct css_line_names *names)
{
	int index;
	int have;

	/* Each name not there yet. */
	for (index = 0; index < names->count; index++) {
		for (have = 0; have < line->count; have++) {
			if (line->names[have] == names->names[index])
				break;
		}
		if (have == line->count && line->count < CSS_LINE_NAMES) {
			line->names[line->count] = names->names[index];
			line->count++;
		}
	}
}

/*
 * Lays a subgrid's declared line-name lists over the lines it spans: the
 * lists in order, a repeat() as many times as it says (an auto-fill one as
 * many as fills what the others leave), anything past the last line dropped.
 * The lines' names are added to the lines array, which has count entries.
 */
void
css_subgrid_lines(
	const struct css_subgrid_names *declared,
	int count,
	struct css_line_names *lines)
{
	const struct css_subgrid_part *part;
	int fixed;
	int fill_groups;
	int repeats;
	int line;
	int index;
	int round;
	int group;

	/* Nothing is declared. */
	if (declared == NULL)
		return;

	/* How many lines the lists that are not an auto-fill repeat take, and how many lists the auto-fill one repeats. */
	fixed = 0;
	fill_groups = 0;
	for (index = 0; index < declared->count; index++) {
		part = &declared->parts[index];
		if (part->repeat < 0)
			fill_groups = part->groups;
		else if (part->repeat == 0)
			fixed++;
		else
			fixed += part->repeat * part->groups;
	}

	/* An auto-fill repeat is as many times as fills the lines (none when they are full). */
	repeats = 0;
	if (fill_groups > 0 && count > fixed)
		repeats = (count - fixed) / fill_groups;

	/* The lists, line after line. */
	line = 0;
	for (index = 0; index < declared->count; index++) {
		part = &declared->parts[index];
		if (part->repeat == 0) {
			if (line < count)
				grid_text_merge_names(&lines[line], &part->names[0]);
			line++;
			continue;
		}
		round = part->repeat < 0 ? repeats : part->repeat;
		for (; round > 0 && line < count; round--) {
			for (group = 0; group < part->groups && line < count; group++) {
				grid_text_merge_names(&lines[line], &part->names[group]);
				line++;
			}
		}
	}
}
