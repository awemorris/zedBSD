/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parsing of linear-gradient() and repeating-linear-gradient() (https://drafts.csswg.org/css-images-3/#linear-gradients):
 * an optional direction (an angle, or "to" and a side or a corner) and a
 * list of color stops, each a color with up to two positions.
 */

#include "css/internal.h"

#include <errno.h>
#include <string.h>

/* The ratio of degrees to the half turn's radians, to convert rad. */
#define GRADIENT_DEGREES_PER_RADIAN 57.29577951308232f

static size_t gradient_component(const struct css_token *tokens, size_t count);
static int gradient_angle(const struct css_token *token, float *degrees);
static int gradient_direction(const struct css_token *tokens, size_t count, struct css_gradient *made);
static int gradient_side(const struct css_token *token, unsigned *side);
static int gradient_position(const struct css_token *token, struct css_gradient_stop *stop);
static int gradient_stop(const struct css_token *tokens, size_t count, struct css_gradient *made);

/*
 * Parses the tokens of a linear-gradient() function: the function token
 * through its closing parenthesis.
 */
int
css_gradient_parse(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	const struct css_gradient **gradient)
{
	struct css_gradient *made;
	size_t start;
	size_t index;
	size_t end;
	size_t span;
	int depth;
	int first;
	int repeating;
	int error;

	/* The function with its closing parenthesis. */
	if (count < 3U || tokens[0].type != CSS_TOKEN_FUNCTION || tokens[count - 1U].type != CSS_TOKEN_CLOSE_PAREN)
		return EINVAL;
	repeating = css_ident_equal(&tokens[0], "repeating-linear-gradient");
	if (!repeating && !css_ident_equal(&tokens[0], "linear-gradient"))
		return EINVAL;

	/* The gradient, to the parse's arena. */
	made = wb_arena_alloc(parse->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));
	made->angle = 180.0f;
	made->repeating = repeating ? 1U : 0U;

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

		/* The argument: the direction when it is the first and has one, otherwise a color stop. */
		span = index - start;
		if (span == 0)
			return EINVAL;
		if (first && gradient_direction(tokens + start, span, made) == 0) {
			first = 0;
			start = index + 1U;
			continue;
		}
		first = 0;
		error = gradient_stop(tokens + start, span, made);
		if (error != 0)
			return error;
		start = index + 1U;
	}

	/* A gradient has at least two stops. */
	if (made->count < 2U)
		return EINVAL;

	/* Succeeded: the gradient. */
	*gradient = made;
	return 0;
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

/* Reads the first argument as a direction: an angle, or "to" and one or two sides; EINVAL when it is not one. */
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
	index = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	while (count > index && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (index >= count)
		return EINVAL;

	/* An angle alone. */
	if (count - index == 1U && tokens[index].type == CSS_TOKEN_DIMENSION) {
		error = gradient_angle(&tokens[index], &degrees);
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

/* Reads a stop's position: a length (px, em or zero) or a percentage. */
static int
gradient_position(
	const struct css_token *token,
	struct css_gradient_stop *stop)
{
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

/* Reads a color stop argument: a color, then up to two positions (two make two stops). */
static int
gradient_stop(
	const struct css_token *tokens,
	size_t count,
	struct css_gradient *made)
{
	struct css_gradient_stop stop;
	struct css_gradient_stop second;
	size_t index;
	size_t span;
	unsigned positions;
	int error;

	/* Whitespace around it is not part of it. */
	index = 0;
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;
	while (count > index && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	if (index >= count)
		return EINVAL;

	/* The color: a token, or a function. */
	memset(&stop, 0, sizeof(stop));
	memset(&second, 0, sizeof(second));
	span = gradient_component(tokens + index, count - index);
	error = css_parse_color(tokens + index, span, &stop.color);
	if (error != 0)
		return EINVAL;
	index += span;

	/* Up to two positions. */
	positions = 0;
	while (index < count) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE) {
			index++;
			continue;
		}
		if (positions == 0)
			error = gradient_position(&tokens[index], &stop);
		else if (positions == 1U)
			error = gradient_position(&tokens[index], &second);
		else
			return EINVAL;
		if (error != 0)
			return error;
		positions++;
		index++;
	}

	/* The stops, kept in order. */
	if (made->count + (positions == 2U ? 2U : 1U) > CSS_GRADIENT_STOPS)
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
