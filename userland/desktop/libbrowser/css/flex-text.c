/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of the flex and flex-flow shorthands: the canonical form
 * CSSOM serializes a declared value in, and the values it gives its
 * longhands.  flex is written as its three parts (none is 0 0 auto, an
 * omitted basis 0%, an omitted factor 1); flex-flow leaves out a nowrap,
 * and a row when a wrap is written.
 * https://drafts.csswg.org/css-flexbox-1/#flex-property
 * https://drafts.csswg.org/css-flexbox-1/#flex-flow-property
 */

#include "css/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The flex shorthands and their longhands, in the order of their parts. */
static const char *const flex_text_shorthands[2][5] = {
	{ "flex", "flex-grow", "flex-shrink", "flex-basis", NULL },
	{ "flex-flow", "flex-direction", "flex-wrap", NULL },
};

static int flex_text_flex(const uint16_t *units, const struct css_token *tokens, size_t count, struct wb_units parts[3], int *found);
static int flex_text_flow(const struct css_token *tokens, size_t count, struct wb_units parts[3], int *found);
static int flex_text_is_number(const struct css_token *tokens, size_t first, size_t last);
static int flex_text_number(const uint16_t *units, const struct css_token *tokens, size_t first, size_t last, struct wb_units *out);
static int flex_text_ascii(const char *ascii, struct wb_units *out);

/*
 * The longhands of a flex shorthand (a list that ends with NULL), or NULL
 * when the name is not one.
 */
const char *const *
css_flex_shorthand_longhands(
	const struct vm_string *name)
{
	size_t row;

	/* The shorthand's row. */
	for (row = 0; row < 2U; row++) {
		if (vm_string_equal_ascii(name, flex_text_shorthands[row][0]))
			return &flex_text_shorthands[row][1];
	}

	/* Not a flex shorthand. */
	return NULL;
}

/*
 * Writes a flex or flex-flow value's canonical text and the values it
 * gives its longhands (found is 0 when the value is not one of the forms).
 */
int
css_flex_shorthand_parts(
	const struct vm_string *shorthand,
	const struct vm_string *value,
	struct wb_units *canonical,
	struct wb_units parts[3],
	int *found)
{
	struct css_token *tokens;
	struct wb_units text;
	struct wb_arena arena;
	size_t count;
	size_t index;
	int flow;
	int error;

	/* The value's tokens. */
	*found = 0;
	if (css_flex_shorthand_longhands(shorthand) == NULL)
		return 0;
	flow = vm_string_equal_ascii(shorthand, "flex-flow");
	wb_units_init(&text);
	wb_arena_init(&arena, 0);
	error = vm_string_append_units(value, &text);
	if (error == 0)
		error = css_tokenize(&arena, text.data, text.length, &tokens, &count);
	if (error != 0)
		goto cleanup;
	while (count > 0 && (tokens[count - 1U].type == CSS_TOKEN_EOF || tokens[count - 1U].type == CSS_TOKEN_WHITESPACE))
		count--;

	/* The parts. */
	if (flow)
		error = flex_text_flow(tokens, count, parts, found);
	else
		error = flex_text_flex(text.data, tokens, count, parts, found);
	if (error != 0 || !*found)
		goto cleanup;

	/* The canonical text: flex's three parts, or flex-flow's that are not left out. */
	if (!flow) {
		for (index = 0; index < 3U && error == 0; index++) {
			if (index > 0)
				error = flex_text_ascii(" ", canonical);
			if (error == 0)
				error = wb_units_append(canonical, parts[index].data, parts[index].length);
		}
	} else {
		if (parts[1].length == 6 && parts[1].data[0] == 'n') {
			error = wb_units_append(canonical, parts[0].data, parts[0].length);
		} else if (parts[0].length == 3 && parts[0].data[0] == 'r') {
			error = wb_units_append(canonical, parts[1].data, parts[1].length);
		} else {
			error = wb_units_append(canonical, parts[0].data, parts[0].length);
			if (error == 0)
				error = flex_text_ascii(" ", canonical);
			if (error == 0)
				error = wb_units_append(canonical, parts[1].data, parts[1].length);
		}
	}

cleanup:
	wb_arena_release(&arena);
	wb_units_release(&text);
	return error;
}

/* Finds flex's grow, shrink and basis. */
static int
flex_text_flex(
	const uint16_t *units,
	const struct css_token *tokens,
	size_t count,
	struct wb_units parts[3],
	int *found)
{
	size_t starts[3];
	size_t ends[3];
	size_t components;
	size_t index;
	size_t next;
	int numbers;
	int last_number;
	int basis;
	int zero;
	int error;

	/* The components (a function and its arguments is one). */
	components = 0;
	index = 0;
	while (index < count) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}
		next = index + 1U;
		if (tokens[index].type == CSS_TOKEN_FUNCTION) {
			int depth;

			depth = 1;
			while (next < count && depth > 0) {
				if (tokens[next].type == CSS_TOKEN_FUNCTION || tokens[next].type == CSS_TOKEN_OPEN_PAREN)
					depth++;
				else if (tokens[next].type == CSS_TOKEN_CLOSE_PAREN)
					depth--;
				next++;
			}
		}
		if (components == 3)
			return 0;
		starts[components] = index;
		ends[components] = next;
		components++;
		index = next;
	}
	if (components == 0)
		return 0;

	/* none and auto. */
	if (components == 1 && tokens[starts[0]].type == CSS_TOKEN_IDENT) {
		if (css_ident_equal(&tokens[starts[0]], "none")) {
			error = flex_text_ascii("0", &parts[0]);
			if (error == 0)
				error = flex_text_ascii("0", &parts[1]);
		} else if (css_ident_equal(&tokens[starts[0]], "auto")) {
			error = flex_text_ascii("1", &parts[0]);
			if (error == 0)
				error = flex_text_ascii("1", &parts[1]);
		} else {
			goto parts;
		}
		if (error == 0)
			error = flex_text_ascii("auto", &parts[2]);
		*found = error == 0;
		return error;
	}

parts:
	/* The factors in order, side by side; anything else (or a zero after them) is the basis. */
	numbers = 0;
	last_number = -2;
	basis = 0;
	for (index = 0; index < components; index++) {
		zero = ends[index] == starts[index] + 1U && tokens[starts[index]].type == CSS_TOKEN_NUMBER &&
		    tokens[starts[index]].number == 0;
		if (flex_text_is_number(tokens, starts[index], ends[index]) &&
		    (!zero || (numbers < 2 && (numbers == 0 || (int)index == last_number + 1)))) {
			if (numbers == 2 || (numbers == 1 && (int)index != last_number + 1))
				return 0;
			error = flex_text_number(units, tokens, starts[index], ends[index], &parts[numbers]);
			if (error != 0)
				return error;
			last_number = (int)index;
			numbers++;
			continue;
		}
		if (basis)
			return 0;
		basis = 1;
		if (zero)
			error = flex_text_ascii("0px", &parts[2]);
		else
			error = wb_units_append(&parts[2], units + tokens[starts[index]].source_start,
			    tokens[ends[index] - 1U].source_end - tokens[starts[index]].source_start);
		if (error != 0)
			return error;
	}

	/* The parts left out. */
	error = 0;
	if (numbers == 0)
		error = flex_text_ascii("1", &parts[0]);
	if (error == 0 && numbers < 2)
		error = flex_text_ascii("1", &parts[1]);
	if (error == 0 && !basis)
		error = flex_text_ascii("0%", &parts[2]);
	*found = error == 0;
	return error;
}

/* Finds flex-flow's direction and wrap. */
static int
flex_text_flow(
	const struct css_token *tokens,
	size_t count,
	struct wb_units parts[3],
	int *found)
{
	static const char *const directions[4] = { "row", "row-reverse", "column", "column-reverse" };
	static const char *const wraps[3] = { "nowrap", "wrap", "wrap-reverse" };
	const char *direction;
	const char *wrap;
	size_t index;
	size_t name;
	int error;

	/* Each keyword once, in either order. */
	direction = NULL;
	wrap = NULL;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type != CSS_TOKEN_IDENT)
			return 0;
		for (name = 0; name < 4U; name++) {
			if (css_ident_equal(&tokens[index], directions[name]))
				break;
		}
		if (name < 4U) {
			if (direction != NULL)
				return 0;
			direction = directions[name];
			continue;
		}
		for (name = 0; name < 3U; name++) {
			if (css_ident_equal(&tokens[index], wraps[name]))
				break;
		}
		if (name == 3U || wrap != NULL)
			return 0;
		wrap = wraps[name];
	}
	if (direction == NULL && wrap == NULL)
		return 0;

	/* The defaults for what is left out. */
	error = flex_text_ascii(direction != NULL ? direction : "row", &parts[0]);
	if (error == 0)
		error = flex_text_ascii(wrap != NULL ? wrap : "nowrap", &parts[1]);
	*found = error == 0;
	return error;
}

/* Tells whether a component is a number: a number token, or a calculation without units. */
static int
flex_text_is_number(
	const struct css_token *tokens,
	size_t first,
	size_t last)
{
	size_t index;

	/* A number token. */
	if (last == first + 1U)
		return tokens[first].type == CSS_TOKEN_NUMBER;

	/* A math function whose arguments have no dimension or percentage. */
	if (tokens[first].type != CSS_TOKEN_FUNCTION)
		return 0;
	for (index = first + 1U; index < last; index++) {
		if (tokens[index].type == CSS_TOKEN_DIMENSION || tokens[index].type == CSS_TOKEN_PERCENTAGE)
			return 0;
	}

	/* Succeeded: the component is a number. */
	return 1;
}

/* Writes a number component: a number token in its shortest form, a function as written. */
static int
flex_text_number(
	const uint16_t *units,
	const struct css_token *tokens,
	size_t first,
	size_t last,
	struct wb_units *out)
{
	char text[64];

	/* A function as written. */
	if (last != first + 1U)
		return wb_units_append(out, units + tokens[first].source_start,
		    tokens[last - 1U].source_end - tokens[first].source_start);

	/* A number token. */
	snprintf(text, sizeof(text), "%.6g", tokens[first].number);
	return flex_text_ascii(text, out);
}

/* Appends ASCII characters. */
static int
flex_text_ascii(
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
