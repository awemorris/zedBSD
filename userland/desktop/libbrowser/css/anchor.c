/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values of anchor positioning: anchor() in the insets and
 * anchor-size() in the insets and the sizes of a box, alone or in a
 * calculation, and the anchor names of anchor-name and anchor-scope.
 *
 * A value with an anchor function is kept as a sum the layout finishes:
 * pixels, a percentage of the containing block, and up to
 * CSS_ANCHOR_TERMS anchor functions times a factor, each with its
 * fallback (a length and a percentage).  The calculation may add,
 * subtract, multiply and divide by numbers, and nest calc() and
 * parentheses; min(), max() and clamp() around an anchor function are not
 * kept (the declaration is dropped).
 * https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
 * https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
 */

#include "css/internal.h"

#include <errno.h>
#include <string.h>

/* A part of a calculation while it is parsed: a sum of lengths and anchor functions, or a plain number. */
struct anchor_sum {
	struct css_anchor_expr expr;
	int is_number;
	float number;
};

/* The tokens of a value and where the parse is. */
struct anchor_reader {
	struct css_parse *parse;
	const struct css_token *tokens;
	size_t count;
	size_t at;
	int sizes;
};

static int anchor_sum(struct anchor_reader *reader, struct anchor_sum *sum);
static int anchor_product(struct anchor_reader *reader, struct anchor_sum *sum);
static int anchor_atom(struct anchor_reader *reader, struct anchor_sum *sum);
static int anchor_function(struct anchor_reader *reader, int size_function, struct anchor_sum *sum);
static int anchor_fallback(struct anchor_reader *reader, struct css_anchor_query *query);
static int anchor_side(const struct css_token *token, int size_function, int *side);
static void anchor_skip_space(struct anchor_reader *reader);
static int anchor_at_close(struct anchor_reader *reader);
static void anchor_scale(struct anchor_sum *sum, float factor);
static int anchor_add(struct anchor_sum *sum, const struct anchor_sum *other, float sign);
static int anchor_is_dashed(const struct css_token *token);

/* Tells whether a value's tokens hold an anchor() or anchor-size() function (at any depth). */
int
css_anchor_mentioned(
	const struct css_token *tokens,
	size_t count)
{
	size_t index;

	/* Each function token. */
	for (index = 0; index < count; index++) {
		if (tokens[index].type != CSS_TOKEN_FUNCTION)
			continue;
		if (css_ident_equal(&tokens[index], "anchor") || css_ident_equal(&tokens[index], "anchor-size"))
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Parses an inset (sizes 0) or a size (sizes 1) that holds an anchor
 * function into a sum in the parse's arena.  anchor() is only for the
 * insets.
 */
int
css_anchor_parse(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	int sizes,
	const struct css_anchor_expr **expr)
{
	struct anchor_reader reader;
	struct anchor_sum sum;
	struct css_anchor_expr *made;
	int error;

	/* The whole value is one sum of lengths and anchor functions. */
	memset(&reader, 0, sizeof(reader));
	reader.parse = parse;
	reader.tokens = tokens;
	reader.count = count;
	reader.sizes = sizes;
	anchor_skip_space(&reader);
	error = anchor_sum(&reader, &sum);
	if (error != 0)
		return error;
	anchor_skip_space(&reader);
	if (reader.at != reader.count || sum.is_number || sum.expr.count == 0)
		return EINVAL;

	/* Kept in the arena. */
	made = wb_arena_alloc(parse->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	*made = sum.expr;
	*expr = made;

	/* Succeeded: the sum is parsed. */
	return 0;
}

/*
 * Parses the names of anchor-name or anchor-scope: dashed identifiers
 * separated by commas, into a list in the parse's arena.
 */
int
css_anchor_names_parse(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	const struct css_anchor_names **names)
{
	struct css_anchor_names *made;
	size_t index;
	int expect_name;

	/* The list. */
	made = wb_arena_alloc(parse->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));

	/* Names and commas in turn. */
	expect_name = 1;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (expect_name) {
			if (!anchor_is_dashed(&tokens[index]) || made->count == CSS_ANCHOR_NAMES)
				return EINVAL;
			made->names[made->count] = vm_atom_from_units(parse->heap, tokens[index].text, tokens[index].length);
			if (made->names[made->count] == NULL)
				return ENOMEM;
			made->count++;
			expect_name = 0;
			continue;
		}
		if (tokens[index].type != CSS_TOKEN_COMMA)
			return EINVAL;
		expect_name = 1;
	}
	if (expect_name)
		return EINVAL;

	/* Succeeded: the names are parsed. */
	*names = made;
	return 0;
}

/* Parses terms joined by + and - (which have whitespace around them). */
static int
anchor_sum(
	struct anchor_reader *reader,
	struct anchor_sum *sum)
{
	struct anchor_sum other;
	float sign;
	int error;

	/* The first term. */
	error = anchor_product(reader, sum);
	if (error != 0)
		return error;

	/* Each further term. */
	for (;;) {
		anchor_skip_space(reader);
		if (reader->at >= reader->count || reader->tokens[reader->at].type != CSS_TOKEN_DELIM ||
		    (reader->tokens[reader->at].delim != '+' && reader->tokens[reader->at].delim != '-'))
			return 0;
		sign = reader->tokens[reader->at].delim == '+' ? 1.0f : -1.0f;
		reader->at++;
		anchor_skip_space(reader);
		error = anchor_product(reader, &other);
		if (error != 0)
			return error;
		error = anchor_add(sum, &other, sign);
		if (error != 0)
			return error;
	}
}

/* Parses factors joined by * and / (one side of each a number). */
static int
anchor_product(
	struct anchor_reader *reader,
	struct anchor_sum *sum)
{
	struct anchor_sum other;
	uint32_t operation;
	int error;

	/* The first factor. */
	error = anchor_atom(reader, sum);
	if (error != 0)
		return error;

	/* Each further factor. */
	for (;;) {
		anchor_skip_space(reader);
		if (reader->at >= reader->count || reader->tokens[reader->at].type != CSS_TOKEN_DELIM ||
		    (reader->tokens[reader->at].delim != '*' && reader->tokens[reader->at].delim != '/'))
			return 0;
		operation = reader->tokens[reader->at].delim;
		reader->at++;
		anchor_skip_space(reader);
		error = anchor_atom(reader, &other);
		if (error != 0)
			return error;

		/* Division is by a number that is not zero. */
		if (operation == '/') {
			if (!other.is_number || other.number == 0.0f)
				return EINVAL;
			anchor_scale(sum, 1.0f / other.number);
			continue;
		}

		/* Multiplication has a number on one side. */
		if (other.is_number) {
			anchor_scale(sum, other.number);
		} else if (sum->is_number) {
			anchor_scale(&other, sum->number);
			*sum = other;
		} else {
			return EINVAL;
		}
	}
}

/* Parses a number, a length, a percentage, calc() or parentheses, or an anchor function. */
static int
anchor_atom(
	struct anchor_reader *reader,
	struct anchor_sum *sum)
{
	const struct css_token *token;
	int error;

	/* The token. */
	memset(sum, 0, sizeof(*sum));
	if (reader->at >= reader->count)
		return EINVAL;
	token = &reader->tokens[reader->at];

	/* A number. */
	if (token->type == CSS_TOKEN_NUMBER) {
		sum->is_number = 1;
		sum->number = (float)token->number;
		reader->at++;
		return 0;
	}

	/* A percentage. */
	if (token->type == CSS_TOKEN_PERCENTAGE) {
		sum->expr.percent = (float)token->number;
		sum->expr.has_percent = 1;
		reader->at++;
		return 0;
	}

	/* A length in pixels or ems. */
	if (token->type == CSS_TOKEN_DIMENSION) {
		if (css_ident_equal(token, "px"))
			sum->expr.px = (float)token->number;
		else if (css_ident_equal(token, "em"))
			sum->expr.em = (float)token->number;
		else if (css_ident_equal(token, "rem"))
			sum->expr.rem = (float)token->number;
		else
			return EINVAL;
		reader->at++;
		return 0;
	}

	/* calc() and parentheses hold a sum. */
	if ((token->type == CSS_TOKEN_FUNCTION && css_ident_equal(token, "calc")) || token->type == CSS_TOKEN_OPEN_PAREN) {
		reader->at++;
		anchor_skip_space(reader);
		error = anchor_sum(reader, sum);
		if (error != 0)
			return error;
		if (!anchor_at_close(reader))
			return EINVAL;
		reader->at++;
		return 0;
	}

	/* The anchor functions. */
	if (token->type == CSS_TOKEN_FUNCTION && css_ident_equal(token, "anchor"))
		return anchor_function(reader, 0, sum);
	if (token->type == CSS_TOKEN_FUNCTION && css_ident_equal(token, "anchor-size"))
		return anchor_function(reader, 1, sum);

	/* Anything else. */
	return EINVAL;
}

/* Parses anchor(name? side, fallback?) or anchor-size(name? size?, fallback?). */
static int
anchor_function(
	struct anchor_reader *reader,
	int size_function,
	struct anchor_sum *sum)
{
	struct css_anchor_query *query;
	const struct css_token *token;
	int found;
	int error;

	/* anchor() is for the insets only. */
	if (!size_function && reader->sizes)
		return EINVAL;
	reader->at++;
	query = &sum->expr.terms[0];
	sum->expr.count = 1;
	query->coef = 1.0f;
	query->side = size_function ? CSS_ANCHOR_SIZE_DEFAULT : -1;

	/* The name and the side, in either order. */
	anchor_skip_space(reader);
	while (reader->at < reader->count) {
		token = &reader->tokens[reader->at];
		if (token->type == CSS_TOKEN_COMMA || token->type == CSS_TOKEN_CLOSE_PAREN)
			break;
		if (token->type == CSS_TOKEN_WHITESPACE) {
			reader->at++;
			continue;
		}
		if (anchor_is_dashed(token)) {
			if (query->name != NULL)
				return EINVAL;
			query->name = vm_atom_from_units(reader->parse->heap, token->text, token->length);
			if (query->name == NULL)
				return ENOMEM;
			reader->at++;
			continue;
		}
		if (token->type == CSS_TOKEN_PERCENTAGE && !size_function &&
		    query->side == -1) {
			query->side = CSS_ANCHOR_PERCENT;
			query->percent = (float)token->number;
			reader->at++;
			continue;
		}
		found = anchor_side(token, size_function, &query->side);
		if (!found || (!size_function && query->side == -1))
			return EINVAL;
		reader->at++;
	}

	/* anchor() needs its side. */
	if (!size_function && query->side == -1)
		return EINVAL;

	/* The fallback (after a name or a side: a comma alone is not one). */
	if (reader->at < reader->count && reader->tokens[reader->at].type == CSS_TOKEN_COMMA) {
		if (query->name == NULL && query->side == CSS_ANCHOR_SIZE_DEFAULT)
			return EINVAL;
		reader->at++;
		error = anchor_fallback(reader, query);
		if (error != 0)
			return error;
	}

	/* The closing parenthesis. */
	if (!anchor_at_close(reader))
		return EINVAL;
	reader->at++;

	/* Succeeded: one anchor function. */
	return 0;
}

/* Parses a fallback: a length, a percentage or a calculation of them (without anchor functions). */
static int
anchor_fallback(
	struct anchor_reader *reader,
	struct css_anchor_query *query)
{
	struct anchor_sum fallback;
	int error;

	/* A sum. */
	anchor_skip_space(reader);
	error = anchor_sum(reader, &fallback);
	if (error != 0)
		return error;
	anchor_skip_space(reader);

	/* A zero number is a zero length; another number is not a length. */
	if (fallback.is_number && fallback.number != 0.0f)
		return EINVAL;
	if (fallback.expr.count != 0)
		return EINVAL;

	/* Succeeded: the fallback is kept. */
	query->has_fallback = 1;
	query->fallback_px = fallback.expr.px;
	query->fallback_em = fallback.expr.em;
	query->fallback_rem = fallback.expr.rem;
	query->fallback_percent = fallback.expr.percent;
	query->fallback_has_percent = fallback.expr.has_percent;
	return 0;
}

/* Reads the side keyword of anchor() or the size keyword of anchor-size(). */
static int
anchor_side(
	const struct css_token *token,
	int size_function,
	int *side)
{
	static const char *const sides[] = {
		"top", "right", "bottom", "left", "start", "end", "self-start", "self-end", "center", "",
		"inside", "outside"
	};
	static const char *const sizes[] = {
		"width", "height", "block", "inline", "self-block", "self-inline"
	};
	size_t index;

	/* Only words. */
	if (token->type != CSS_TOKEN_IDENT)
		return 0;

	/* A size. */
	if (size_function) {
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
			if (css_ident_equal(token, sizes[index]) && *side == CSS_ANCHOR_SIZE_DEFAULT) {
				*side = CSS_ANCHOR_WIDTH + (int)index;
				return 1;
			}
		}
		return 0;
	}

	/* A side, once. */
	for (index = 0; index < sizeof(sides) / sizeof(sides[0]); index++) {
		if (sides[index][0] != '\0' && css_ident_equal(token, sides[index]) && *side == -1) {
			*side = (int)index;
			return 1;
		}
	}
	return 0;
}

/* Skips whitespace. */
static void
anchor_skip_space(
	struct anchor_reader *reader)
{
	/* Each whitespace token. */
	while (reader->at < reader->count && reader->tokens[reader->at].type == CSS_TOKEN_WHITESPACE)
		reader->at++;
}

/* Tells whether the reader is at a closing parenthesis (after whitespace). */
static int
anchor_at_close(
	struct anchor_reader *reader)
{
	/* After the whitespace. */
	anchor_skip_space(reader);
	return reader->at < reader->count && reader->tokens[reader->at].type == CSS_TOKEN_CLOSE_PAREN;
}

/* Multiplies a sum by a number. */
static void
anchor_scale(
	struct anchor_sum *sum,
	float factor)
{
	int index;

	/* A number, or each part of a sum. */
	if (sum->is_number) {
		sum->number *= factor;
		return;
	}
	sum->expr.px *= factor;
	sum->expr.em *= factor;
	sum->expr.rem *= factor;
	sum->expr.percent *= factor;
	for (index = 0; index < sum->expr.count; index++)
		sum->expr.terms[index].coef *= factor;
}

/* Adds another sum (times a sign) to a sum: numbers to numbers, lengths to lengths. */
static int
anchor_add(
	struct anchor_sum *sum,
	const struct anchor_sum *other,
	float sign)
{
	int index;

	/* Numbers. */
	if (sum->is_number != other->is_number)
		return EINVAL;
	if (sum->is_number) {
		sum->number += sign * other->number;
		return 0;
	}

	/* The lengths and the anchor functions. */
	sum->expr.px += sign * other->expr.px;
	sum->expr.em += sign * other->expr.em;
	sum->expr.rem += sign * other->expr.rem;
	sum->expr.percent += sign * other->expr.percent;
	sum->expr.has_percent |= other->expr.has_percent;
	for (index = 0; index < other->expr.count; index++) {
		if (sum->expr.count == CSS_ANCHOR_TERMS)
			return EINVAL;
		sum->expr.terms[sum->expr.count] = other->expr.terms[index];
		sum->expr.terms[sum->expr.count].coef *= sign;
		sum->expr.count++;
	}

	/* Succeeded: the sums are added. */
	return 0;
}

/* Tells whether a token is a dashed identifier (an author's name). */
static int
anchor_is_dashed(
	const struct css_token *token)
{
	/* An identifier that starts with two hyphens. */
	return token->type == CSS_TOKEN_IDENT && token->length > 2 && token->text[0] == '-' && token->text[1] == '-';
}
