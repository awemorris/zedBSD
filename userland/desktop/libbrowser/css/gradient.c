/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parsing of the gradient functions: linear-gradient(), radial-gradient()
 * and conic-gradient(), and their repeating-* forms
 * (https://drafts.csswg.org/css-images-3/#gradients and
 * https://drafts.csswg.org/css-images-4/#gradients).  A gradient is an
 * optional first argument that sets its geometry (a direction, a shape with a
 * size and a center, or a rotation with a center) and a list of color stops
 * with transition hints between them.
 */

#include "css/internal.h"

#include <errno.h>
#include <string.h>

/* The ratio of degrees to the half turn's radians, to convert rad. */
#define GRADIENT_DEGREES_PER_RADIAN 57.29577951308232f

/* The most words in the first argument of a radial or conic gradient: a shape, a size, "at" and a position. */
#define GRADIENT_WORDS	12

/* One gradient function: what its name stands for. */
struct gradient_name {
	const char *name;
	int kind;
	unsigned repeating;
};

/*
 * The six gradient functions, looked up by name.
 *
 * It is a constant table: the order does not matter, and every name stands for one kind of gradient
 * and for whether its stops repeat.
 */
static const struct gradient_name gradient_names[] = {
	{ "linear-gradient", CSS_GRADIENT_LINEAR, 0 },
	{ "repeating-linear-gradient", CSS_GRADIENT_LINEAR, 1 },
	{ "radial-gradient", CSS_GRADIENT_RADIAL, 0 },
	{ "repeating-radial-gradient", CSS_GRADIENT_RADIAL, 1 },
	{ "conic-gradient", CSS_GRADIENT_CONIC, 0 },
	{ "repeating-conic-gradient", CSS_GRADIENT_CONIC, 1 },
	{ NULL, 0, 0 }
};

static const struct gradient_name *gradient_name_of(const struct css_token *token);
static size_t gradient_component(const struct css_token *tokens, size_t count);
static void gradient_trim(const struct css_token *tokens, size_t *start, size_t *end);
static int gradient_angle(const struct css_token *token, float *degrees);
static int gradient_angle_or_zero(const struct css_token *token, float *degrees);
static int gradient_side(const struct css_token *token, unsigned *side);
static int gradient_direction(const struct css_token *tokens, size_t count, struct css_gradient *made);
static int gradient_words(const struct css_token *tokens, size_t count, const struct css_token **words, size_t *found);
static int gradient_length(const struct css_token *token, struct css_gradient_length *length);
static int gradient_keyword(const struct css_token *token);
static int gradient_extent_keyword(const struct css_token *token, unsigned *extent);
static void gradient_edge(int keyword, struct css_gradient_length *place);
static void gradient_offset(int keyword, const struct css_gradient_length *length, struct css_gradient_length *place);
static int gradient_center(const struct css_token **words, size_t count, struct css_gradient_length *at);
static int gradient_radial(const struct css_token *tokens, size_t count, struct css_gradient *made);
static int gradient_conic(const struct css_token *tokens, size_t count, struct css_gradient *made);
static int gradient_position(const struct css_token *token, int kind, struct css_gradient_stop *stop);
static int gradient_entry(const struct css_token *tokens, size_t count, struct css_gradient *made);

/*
 * Tells whether a token is the function token of one of the gradient functions.
 */
int
css_gradient_function(
	const struct css_token *token)
{
	const struct gradient_name *found;

	/* Only a function token has one of the names. */
	if (token->type != CSS_TOKEN_FUNCTION)
		return 0;
	found = gradient_name_of(token);
	if (found == NULL)
		return 0;

	/* Succeeded: it is one of the six. */
	return 1;
}

/*
 * Parses the tokens of a gradient function: the function token through its
 * closing parenthesis.
 */
int
css_gradient_parse(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	const struct css_gradient **gradient)
{
	const struct gradient_name *name;
	struct css_gradient *made;
	size_t start;
	size_t index;
	size_t end;
	size_t span;
	int depth;
	int first;
	int error;

	/* The function with its closing parenthesis. */
	if (count < 3U || tokens[0].type != CSS_TOKEN_FUNCTION || tokens[count - 1U].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;
	name = gradient_name_of(&tokens[0]);
	if (name == NULL)
		return EINVAL;

	/* The gradient, to the parse's arena. */
	made = wb_arena_alloc(parse->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));
	made->kind = name->kind;
	made->repeating = name->repeating;

	/* A linear gradient points to the bottom unless told otherwise. */
	if (made->kind == CSS_GRADIENT_LINEAR)
		made->angle = 180.0f;

	/* A radial gradient is an ellipse that reaches the farthest corner, and a radial or conic one is centered. */
	if (made->kind == CSS_GRADIENT_RADIAL) {
		made->extent[0] = CSS_GRADIENT_FARTHEST_CORNER;
		made->extent[1] = CSS_GRADIENT_FARTHEST_CORNER;
	}
	if (made->kind != CSS_GRADIENT_LINEAR) {
		made->at[0].percent = 50.0f;
		made->at[1].percent = 50.0f;
	}

	/* Each argument, up to a comma outside any nested function. */
	first = 1;
	start = 1;
	depth = 0;
	end = count - 1U;
	for (index = 1; index <= end; index++) {
		if (index < end) {
			if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
				depth++;
			if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			if (tokens[index].type != CSS_TOKEN_COMMA || depth != 0)
				continue;
		}

		/* An empty argument is not one. */
		span = index - start;
		if (span == 0)
			return EINVAL;

		/* The first argument sets the geometry when it is one that does. */
		if (first) {
			first = 0;
			error = EINVAL;
			if (made->kind == CSS_GRADIENT_LINEAR)
				error = gradient_direction(tokens + start, span, made);
			if (made->kind == CSS_GRADIENT_RADIAL)
				error = gradient_radial(tokens + start, span, made);
			if (made->kind == CSS_GRADIENT_CONIC)
				error = gradient_conic(tokens + start, span, made);
			if (error == 0) {
				start = index + 1U;
				continue;
			}
		}

		/* Any other argument is a color stop or a transition hint. */
		error = gradient_entry(tokens + start, span, made);
		if (error != 0)
			return error;
		start = index + 1U;
	}

	/* A gradient has a color stop, and its list does not end with a hint. */
	if (made->count < 1U)
		return EINVAL;
	if (made->stops[made->count - 1U].hint != 0)
		return EINVAL;

	/* Succeeded: the gradient. */
	*gradient = made;
	return 0;
}

/*
 * Copies a gradient's color stops without its transition hints.
 */
unsigned
css_gradient_color_stops(
	const struct css_gradient *gradient,
	struct css_gradient *stripped)
{
	unsigned index;

	/* The geometry as it is, and no entries yet. */
	*stripped = *gradient;
	stripped->count = 0;

	/* Each entry that is a color stop. */
	for (index = 0; index < gradient->count; index++) {
		if (gradient->stops[index].hint != 0)
			continue;
		stripped->stops[stripped->count] = gradient->stops[index];
		stripped->count++;
	}

	/* Succeeded: the number of color stops. */
	return stripped->count;
}

/* Looks a function token up in the table of gradient functions; NULL when it names none. */
static const struct gradient_name *
gradient_name_of(
	const struct css_token *token)
{
	size_t index;

	/* Compares the token's name with each. */
	for (index = 0; gradient_names[index].name != NULL; index++) {
		if (css_ident_equal(token, gradient_names[index].name))
			return &gradient_names[index];
	}

	/* None of them. */
	return NULL;
}

/* Measures a component: one token, or a function to its closing parenthesis. */
static size_t
gradient_component(
	const struct css_token *tokens,
	size_t count)
{
	size_t index;
	int depth;

	/* A plain token is one. */
	if (tokens[0].type != CSS_TOKEN_FUNCTION && tokens[0].type != CSS_TOKEN_OPEN_PAREN)
		return 1;

	/* To the parenthesis that closes it. */
	depth = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			depth++;
		if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
			depth--;
		if (depth == 0)
			return index + 1U;
	}
	return count;
}

/* Narrows a span of tokens to the part between its leading and trailing whitespace: it starts at start and ends before end. */
static void
gradient_trim(
	const struct css_token *tokens,
	size_t *start,
	size_t *end)
{
	size_t first;
	size_t last;

	/* The first token that is not whitespace, and the end after the last. */
	first = 0;
	last = *end;
	while (first < last && tokens[first].type == CSS_TOKEN_WHITESPACE)
		first++;
	while (last > first && tokens[last - 1U].type == CSS_TOKEN_WHITESPACE)
		last--;

	/* Succeeded: the part. */
	*start = first;
	*end = last;
}

/* Reads an angle (a number with deg, grad, rad or turn) in degrees. */
static int
gradient_angle(
	const struct css_token *token,
	float *degrees)
{
	/* Only a dimension. */
	if (token->type != CSS_TOKEN_DIMENSION)
		return EINVAL;

	/* Each unit. */
	if (css_ident_equal(token, "deg")) {
		*degrees = (float)token->number;
		return 0;
	}
	if (css_ident_equal(token, "grad")) {
		*degrees = (float)token->number * 0.9f;
		return 0;
	}
	if (css_ident_equal(token, "rad")) {
		*degrees = (float)token->number * GRADIENT_DEGREES_PER_RADIAN;
		return 0;
	}
	if (css_ident_equal(token, "turn")) {
		*degrees = (float)token->number * 360.0f;
		return 0;
	}
	return EINVAL;
}

/* Reads an angle, or the unitless zero that the grammar allows in its place. */
static int
gradient_angle_or_zero(
	const struct css_token *token,
	float *degrees)
{
	/* The unitless zero. */
	if (token->type == CSS_TOKEN_NUMBER && token->number == 0) {
		*degrees = 0.0f;
		return 0;
	}

	/* An angle with its unit. */
	return gradient_angle(token, degrees);
}

/* Reads a side keyword. */
static int
gradient_side(
	const struct css_token *token,
	unsigned *side)
{
	/* Only an identifier. */
	if (token->type != CSS_TOKEN_IDENT)
		return EINVAL;
	if (css_ident_equal(token, "top")) {
		*side = CSS_GRADIENT_TOP;
		return 0;
	}
	if (css_ident_equal(token, "right")) {
		*side = CSS_GRADIENT_RIGHT;
		return 0;
	}
	if (css_ident_equal(token, "bottom")) {
		*side = CSS_GRADIENT_BOTTOM;
		return 0;
	}
	if (css_ident_equal(token, "left")) {
		*side = CSS_GRADIENT_LEFT;
		return 0;
	}
	return EINVAL;
}

/* Reads the first argument of a linear gradient as a direction: an angle, or "to" and one or two sides; EINVAL when it is not one. */
static int
gradient_direction(
	const struct css_token *tokens,
	size_t count,
	struct css_gradient *made)
{
	unsigned first;
	unsigned second;
	size_t index;
	float degrees;
	int error;

	/* Whitespace around it is not part of it. */
	gradient_trim(tokens, &index, &count);
	if (index >= count)
		return EINVAL;

	/* An angle alone, or the unitless zero. */
	if (count - index == 1U) {
		error = gradient_angle_or_zero(&tokens[index], &degrees);
		if (error != 0)
			return error;
		made->angle = degrees;
		made->corner = 0;
		return 0;
	}

	/* "to" and a side, or two sides of different axes. */
	if (tokens[index].type != CSS_TOKEN_IDENT || !css_ident_equal(&tokens[index], "to"))
		return EINVAL;
	index++;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index >= count)
		return EINVAL;
	error = gradient_side(&tokens[index], &first);
	if (error != 0)
		return error;
	index++;
	second = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	if (index < count) {
		error = gradient_side(&tokens[index], &second);
		if (error != 0)
			return error;
		index++;
		if (((first | second) & (CSS_GRADIENT_TOP | CSS_GRADIENT_BOTTOM)) == (first | second))
			return EINVAL;
		if (((first | second) & (CSS_GRADIENT_LEFT | CSS_GRADIENT_RIGHT)) == (first | second))
			return EINVAL;
		while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
			index++;
		if (index < count)
			return EINVAL;
	}

	/* The sides it names. */
	made->corner = first | second;
	made->angle = 0.0f;
	return 0;
}

/* Collects the words of an argument (its tokens separated by whitespace); EINVAL for a function or too many. */
static int
gradient_words(
	const struct css_token *tokens,
	size_t count,
	const struct css_token **words,
	size_t *found)
{
	size_t index;
	size_t start;

	/* Whitespace around the argument is not part of it. */
	gradient_trim(tokens, &start, &count);
	*found = 0;

	/* Each token that is not whitespace is a word of its own. */
	for (index = start; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type == CSS_TOKEN_FUNCTION || tokens[index].type == CSS_TOKEN_OPEN_PAREN)
			return EINVAL;
		if (*found >= GRADIENT_WORDS)
			return EINVAL;
		words[*found] = &tokens[index];
		(*found)++;
	}

	/* Succeeded: the words. */
	return 0;
}

/* Reads a length of a gradient's geometry: zero, a percentage, or a length in pixels or ems. */
static int
gradient_length(
	const struct css_token *token,
	struct css_gradient_length *length)
{
	/* A fresh length. */
	memset(length, 0, sizeof(*length));

	/* Zero. */
	if (token->type == CSS_TOKEN_NUMBER && token->number == 0)
		return 0;

	/* A percentage. */
	if (token->type == CSS_TOKEN_PERCENTAGE) {
		length->percent = (float)token->number;
		return 0;
	}

	/* A length in pixels or ems. */
	if (token->type != CSS_TOKEN_DIMENSION)
		return EINVAL;
	if (css_ident_equal(token, "px")) {
		length->pixels = (float)token->number;
		return 0;
	}
	if (css_ident_equal(token, "em")) {
		length->ems = (float)token->number;
		return 0;
	}
	return EINVAL;
}

/* Reads a position keyword as its letter (l, r, t, b, or c for center); 0 when the token is not one. */
static int
gradient_keyword(
	const struct css_token *token)
{
	/* Only an identifier. */
	if (token->type != CSS_TOKEN_IDENT)
		return 0;
	if (css_ident_equal(token, "left"))
		return 'l';
	if (css_ident_equal(token, "right"))
		return 'r';
	if (css_ident_equal(token, "top"))
		return 't';
	if (css_ident_equal(token, "bottom"))
		return 'b';
	if (css_ident_equal(token, "center"))
		return 'c';
	return 0;
}

/* Reads a radial gradient's size keyword. */
static int
gradient_extent_keyword(
	const struct css_token *token,
	unsigned *extent)
{
	/* Only an identifier. */
	if (token->type != CSS_TOKEN_IDENT)
		return EINVAL;
	if (css_ident_equal(token, "closest-side")) {
		*extent = CSS_GRADIENT_CLOSEST_SIDE;
		return 0;
	}
	if (css_ident_equal(token, "closest-corner")) {
		*extent = CSS_GRADIENT_CLOSEST_CORNER;
		return 0;
	}
	if (css_ident_equal(token, "farthest-side")) {
		*extent = CSS_GRADIENT_FARTHEST_SIDE;
		return 0;
	}
	if (css_ident_equal(token, "farthest-corner")) {
		*extent = CSS_GRADIENT_FARTHEST_CORNER;
		return 0;
	}
	return EINVAL;
}

/* Places a position keyword with no offset: its edge, or the middle for center. */
static void
gradient_edge(
	int keyword,
	struct css_gradient_length *place)
{
	/* The start edges are at 0%, the end edges at 100%, the center at 50%. */
	memset(place, 0, sizeof(*place));
	if (keyword == 'r' || keyword == 'b')
		place->percent = 100.0f;
	if (keyword == 'c')
		place->percent = 50.0f;
}

/*
 * Places an edge keyword with an offset: from the start edge (left or top) the
 * offset is the length itself, from the end edge (right or bottom) it is the
 * length back from 100%.
 */
static void
gradient_offset(
	int keyword,
	const struct css_gradient_length *length,
	struct css_gradient_length *place)
{
	/* The start edges. */
	if (keyword == 'l' || keyword == 't') {
		*place = *length;
		return;
	}

	/* The end edges. */
	place->percent = 100.0f - length->percent;
	place->pixels = -length->pixels;
	place->ems = -length->ems;
}

/*
 * Reads a <position> (CSS Values 3 section 7.3: one, two or four words) into
 * the place on the horizontal and the vertical axis; EINVAL for any other.
 */
static int
gradient_center(
	const struct css_token **words,
	size_t count,
	struct css_gradient_length *at)
{
	struct css_gradient_length length_a;
	struct css_gradient_length length_b;
	int keyword_a;
	int keyword_b;
	int horizontal_a;
	int vertical_a;
	int horizontal_b;
	int vertical_b;
	int error;

	/* The center on both axes unless the words say otherwise. */
	memset(at, 0, 2U * sizeof(*at));
	at[0].percent = 50.0f;
	at[1].percent = 50.0f;

	/* One word: a keyword for one axis (the other stays centered), or a length for the horizontal axis. */
	if (count == 1U) {
		keyword_a = gradient_keyword(words[0]);
		if (keyword_a == 'l' || keyword_a == 'r') {
			gradient_edge(keyword_a, &at[0]);
			return 0;
		}
		if (keyword_a == 't' || keyword_a == 'b') {
			gradient_edge(keyword_a, &at[1]);
			return 0;
		}
		if (keyword_a == 'c')
			return 0;
		error = gradient_length(words[0], &at[0]);
		return error;
	}

	/* Two words: two keywords in either order, or the horizontal word and then the vertical one. */
	if (count == 2U) {
		keyword_a = gradient_keyword(words[0]);
		keyword_b = gradient_keyword(words[1]);

		/* Which axis each keyword can be on: left and right are horizontal, top and bottom are vertical, center is either. */
		horizontal_a = keyword_a == 'l' || keyword_a == 'r' || keyword_a == 'c';
		vertical_a = keyword_a == 't' || keyword_a == 'b' || keyword_a == 'c';
		horizontal_b = keyword_b == 'l' || keyword_b == 'r' || keyword_b == 'c';
		vertical_b = keyword_b == 't' || keyword_b == 'b' || keyword_b == 'c';

		/* Two keywords: the first is the horizontal one when the two fit that way, otherwise they swap. */
		if (keyword_a != 0 && keyword_b != 0) {
			if (horizontal_a && vertical_b) {
				gradient_edge(keyword_a, &at[0]);
				gradient_edge(keyword_b, &at[1]);
				return 0;
			}
			if (vertical_a && horizontal_b) {
				gradient_edge(keyword_b, &at[0]);
				gradient_edge(keyword_a, &at[1]);
				return 0;
			}
			return EINVAL;
		}

		/* The horizontal word: left, center, right or a length. */
		if (keyword_a != 0) {
			if (!horizontal_a)
				return EINVAL;
			gradient_edge(keyword_a, &at[0]);
		} else {
			error = gradient_length(words[0], &at[0]);
			if (error != 0)
				return error;
		}

		/* The vertical word: top, center, bottom or a length. */
		if (keyword_b != 0) {
			if (!vertical_b)
				return EINVAL;
			gradient_edge(keyword_b, &at[1]);
		} else {
			error = gradient_length(words[1], &at[1]);
			if (error != 0)
				return error;
		}
		return 0;
	}

	/* Four words: an edge and its offset for each axis, in either order. */
	if (count != 4U)
		return EINVAL;
	keyword_a = gradient_keyword(words[0]);
	keyword_b = gradient_keyword(words[2]);
	error = gradient_length(words[1], &length_a);
	if (error != 0)
		return error;
	error = gradient_length(words[3], &length_b);
	if (error != 0)
		return error;
	if ((keyword_a == 'l' || keyword_a == 'r') && (keyword_b == 't' || keyword_b == 'b')) {
		gradient_offset(keyword_a, &length_a, &at[0]);
		gradient_offset(keyword_b, &length_b, &at[1]);
		return 0;
	}
	if ((keyword_a == 't' || keyword_a == 'b') && (keyword_b == 'l' || keyword_b == 'r')) {
		gradient_offset(keyword_b, &length_b, &at[0]);
		gradient_offset(keyword_a, &length_a, &at[1]);
		return 0;
	}
	return EINVAL;
}

/*
 * Reads the first argument of a radial gradient as its geometry: a shape and a
 * size in either order, then "at" and a position; EINVAL when it is not one.
 */
static int
gradient_radial(
	const struct css_token *tokens,
	size_t count,
	struct css_gradient *made)
{
	const struct css_token *words[GRADIENT_WORDS];
	struct css_gradient_length lengths[2];
	struct css_gradient_length place[2];
	unsigned extents[2];
	size_t found;
	size_t head;
	size_t index;
	unsigned extent;
	unsigned extent_count;
	unsigned length_count;
	int shape;
	int error;

	/* The words of the argument. */
	error = gradient_words(tokens, count, words, &found);
	if (error != 0)
		return error;
	if (found == 0)
		return EINVAL;

	/* The words before "at" are the shape and the size. */
	head = found;
	for (index = 0; index < found; index++) {
		if (words[index]->type == CSS_TOKEN_IDENT && css_ident_equal(words[index], "at")) {
			head = index;
			break;
		}
	}

	/* Each of them is a shape, a size keyword, or a length. */
	shape = 0;
	extent_count = 0;
	length_count = 0;
	extents[0] = 0;
	extents[1] = 0;
	memset(lengths, 0, sizeof(lengths));
	for (index = 0; index < head; index++) {
		if (words[index]->type == CSS_TOKEN_IDENT && css_ident_equal(words[index], "circle")) {
			if (shape != 0)
				return EINVAL;
			shape = 1;
			continue;
		}
		if (words[index]->type == CSS_TOKEN_IDENT && css_ident_equal(words[index], "ellipse")) {
			if (shape != 0)
				return EINVAL;
			shape = 2;
			continue;
		}
		error = gradient_extent_keyword(words[index], &extent);
		if (error == 0) {
			if (extent_count >= 2U)
				return EINVAL;
			extents[extent_count] = extent;
			extent_count++;
			continue;
		}
		if (length_count >= 2U)
			return EINVAL;
		error = gradient_length(words[index], &lengths[length_count]);
		if (error != 0)
			return error;
		length_count++;
	}

	/* A size is keywords or lengths, not both, and a radius is not negative. */
	if (extent_count > 0 && length_count > 0)
		return EINVAL;
	for (index = 0; index < length_count; index++) {
		if (lengths[index].percent < 0.0f || lengths[index].pixels < 0.0f || lengths[index].ems < 0.0f)
			return EINVAL;
	}

	/* A circle has one size; an ellipse has one or two keywords, or two lengths. */
	if (shape == 1 && (extent_count > 1U || length_count > 1U))
		return EINVAL;
	if (shape == 2 && length_count == 1U)
		return EINVAL;

	/* The shape when it is not named: a circle for one length, otherwise an ellipse. */
	if (shape == 0) {
		shape = 2;
		if (length_count == 1U)
			shape = 1;
	}

	/* The center after "at": a position and nothing more. */
	memset(place, 0, sizeof(place));
	place[0].percent = 50.0f;
	place[1].percent = 50.0f;
	if (head < found) {
		error = gradient_center(words + head + 1U, found - head - 1U, place);
		if (error != 0)
			return error;
	}

	/* Succeeded: the geometry. */
	made->circle = shape == 1 ? 1U : 0U;
	if (extent_count > 0) {
		made->extent[0] = extents[0];
		made->extent[1] = extent_count > 1U ? extents[1] : extents[0];
	}
	if (length_count > 0) {
		made->extent[0] = CSS_GRADIENT_EXTENT_NONE;
		made->extent[1] = CSS_GRADIENT_EXTENT_NONE;
		made->radius[0] = lengths[0];
		if (length_count > 1U)
			made->radius[1] = lengths[1];
	}
	if (made->circle != 0)
		made->extent[1] = CSS_GRADIENT_EXTENT_NONE;
	made->at[0] = place[0];
	made->at[1] = place[1];
	return 0;
}

/*
 * Reads the first argument of a conic gradient as its geometry: "from" and an
 * angle, then "at" and a position, each optional; EINVAL when it is not one.
 */
static int
gradient_conic(
	const struct css_token *tokens,
	size_t count,
	struct css_gradient *made)
{
	const struct css_token *words[GRADIENT_WORDS];
	struct css_gradient_length place[2];
	size_t found;
	size_t index;
	float degrees;
	int error;

	/* The words of the argument. */
	error = gradient_words(tokens, count, words, &found);
	if (error != 0)
		return error;
	if (found == 0)
		return EINVAL;

	/* "from" and the rotation. */
	index = 0;
	degrees = 0.0f;
	if (words[0]->type == CSS_TOKEN_IDENT && css_ident_equal(words[0], "from")) {
		if (found < 2U)
			return EINVAL;
		error = gradient_angle_or_zero(words[1], &degrees);
		if (error != 0)
			return error;
		index = 2;
	}

	/* "at" and the center. */
	memset(place, 0, sizeof(place));
	place[0].percent = 50.0f;
	place[1].percent = 50.0f;
	if (index < found) {
		if (words[index]->type != CSS_TOKEN_IDENT || !css_ident_equal(words[index], "at"))
			return EINVAL;
		error = gradient_center(words + index + 1U, found - index - 1U, place);
		if (error != 0)
			return error;
	}

	/* Succeeded: the geometry. */
	made->angle = degrees;
	made->at[0] = place[0];
	made->at[1] = place[1];
	return 0;
}

/*
 * Reads the position of a stop or a hint: for a conic gradient an angle, a
 * percentage or zero; for the others a length or a percentage.
 */
static int
gradient_position(
	const struct css_token *token,
	int kind,
	struct css_gradient_stop *stop)
{
	float degrees;
	int error;

	/* A conic gradient's positions are angles or percentages of the turn. */
	if (kind == CSS_GRADIENT_CONIC) {
		if (token->type == CSS_TOKEN_PERCENTAGE) {
			stop->position = (float)token->number;
			stop->unit = CSS_GRADIENT_AT_PERCENT;
			return 0;
		}
		error = gradient_angle_or_zero(token, &degrees);
		if (error != 0)
			return error;
		stop->position = degrees;
		stop->unit = CSS_GRADIENT_AT_DEGREES;
		return 0;
	}

	/* Zero. */
	if (token->type == CSS_TOKEN_NUMBER && token->number == 0) {
		stop->position = 0.0f;
		stop->unit = CSS_GRADIENT_AT_PX;
		return 0;
	}

	/* A percentage. */
	if (token->type == CSS_TOKEN_PERCENTAGE) {
		stop->position = (float)token->number;
		stop->unit = CSS_GRADIENT_AT_PERCENT;
		return 0;
	}

	/* A length in pixels or ems. */
	if (token->type != CSS_TOKEN_DIMENSION)
		return EINVAL;
	if (css_ident_equal(token, "px")) {
		stop->position = (float)token->number;
		stop->unit = CSS_GRADIENT_AT_PX;
		return 0;
	}
	if (css_ident_equal(token, "em")) {
		stop->position = (float)token->number;
		stop->unit = CSS_GRADIENT_AT_EM;
		return 0;
	}
	return EINVAL;
}

/*
 * Reads an argument that is a color stop (a color, then up to two positions;
 * two make two entries of one color) or a transition hint (one position that
 * follows a color stop).
 */
static int
gradient_entry(
	const struct css_token *tokens,
	size_t count,
	struct css_gradient *made)
{
	struct css_gradient_stop stop;
	struct css_gradient_stop second;
	size_t index;
	size_t span;
	unsigned positions;
	unsigned added;
	int error;

	/* Whitespace around it is not part of it. */
	gradient_trim(tokens, &index, &count);
	if (index >= count)
		return EINVAL;

	/* A color, a token or a function, starts a color stop; anything else must be a hint. */
	memset(&stop, 0, sizeof(stop));
	memset(&second, 0, sizeof(second));
	span = gradient_component(tokens + index, count - index);
	error = css_parse_color(tokens + index, span, &stop.color);
	if (error != 0) {
		/* A hint is one position, and it comes after a color stop. */
		if (span != 1U || index + span != count)
			return EINVAL;
		if (made->count == 0 || made->stops[made->count - 1U].hint != 0)
			return EINVAL;
		error = gradient_position(&tokens[index], made->kind, &stop);
		if (error != 0)
			return EINVAL;
		if (made->count + 1U > CSS_GRADIENT_STOPS)
			return EINVAL;
		stop.hint = 1;
		made->stops[made->count] = stop;
		made->count++;
		return 0;
	}
	index += span;

	/* Up to two positions. */
	positions = 0;
	while (index < count) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}
		if (positions == 0)
			error = gradient_position(&tokens[index], made->kind, &stop);
		else if (positions == 1U)
			error = gradient_position(&tokens[index], made->kind, &second);
		else
			return EINVAL;
		if (error != 0)
			return error;
		positions++;
		index++;
	}

	/* The entries, kept in order, and no more than the list holds. */
	added = positions == 2U ? 2U : 1U;
	if (made->count + added > CSS_GRADIENT_STOPS)
		return EINVAL;
	made->stops[made->count] = stop;
	made->count++;
	if (positions == 2U) {
		second.color = stop.color;
		made->stops[made->count] = second;
		made->count++;
	}

	/* Succeeded: the stop is added. */
	return 0;
}
