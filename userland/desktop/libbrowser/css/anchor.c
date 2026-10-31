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
	struct anchor_sum percent;
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

		/* A calculation of percentages is a percentage too. */
		if (token->type == CSS_TOKEN_FUNCTION && css_ident_equal(token, "calc") && !size_function &&
		    query->side == -1) {
			error = anchor_atom(reader, &percent);
			if (error != 0)
				return error;
			if (percent.is_number || percent.expr.count != 0 || percent.expr.px != 0.0f ||
			    percent.expr.em != 0.0f || percent.expr.rem != 0.0f)
				return EINVAL;
			query->side = CSS_ANCHOR_PERCENT;
			query->percent = percent.expr.percent;
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

	/* The closing parenthesis (the end of the value closes it too). */
	if (!anchor_at_close(reader) && reader->at < reader->count)
		return EINVAL;
	if (reader->at < reader->count)
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
	struct css_anchor_expr *made;
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

	/* A fallback with anchor functions is kept as a sum of its own. */
	if (fallback.expr.count != 0) {
		made = wb_arena_alloc(reader->parse->arena, sizeof(*made));
		if (made == NULL)
			return ENOMEM;
		*made = fallback.expr;
		query->has_fallback = 1;
		query->fallback_expr = made;
		return 0;
	}

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

/*
 * The keywords of position-area: the axis each names (or none: the
 * generic start and end, center and span-all) and the regions of the 3x3
 * grid around the anchor it covers along it (0 before the anchor, 1 the
 * anchor, 2 after it).
 */
enum anchor_area_axis {
	ANCHOR_AREA_X,
	ANCHOR_AREA_Y,
	ANCHOR_AREA_BLOCK,
	ANCHOR_AREA_INLINE,
	ANCHOR_AREA_SELF_BLOCK,
	ANCHOR_AREA_SELF_INLINE,
	ANCHOR_AREA_GENERIC,
	ANCHOR_AREA_SELF_GENERIC,
	ANCHOR_AREA_CENTER,
	ANCHOR_AREA_ALL
};

/* One keyword: its name, its axis, its regions, and its short form (the generic keyword it is on its axis). */
struct anchor_area_keyword {
	const char *name;
	int axis;
	int first;
	int last;
	const char *brief;
};

static const struct anchor_area_keyword anchor_area_keywords[] = {
	{ "left", ANCHOR_AREA_X, 0, 0, NULL },
	{ "right", ANCHOR_AREA_X, 2, 2, NULL },
	{ "span-left", ANCHOR_AREA_X, 0, 1, NULL },
	{ "span-right", ANCHOR_AREA_X, 1, 2, NULL },
	{ "x-start", ANCHOR_AREA_X, 0, 0, NULL },
	{ "x-end", ANCHOR_AREA_X, 2, 2, NULL },
	{ "span-x-start", ANCHOR_AREA_X, 0, 1, NULL },
	{ "span-x-end", ANCHOR_AREA_X, 1, 2, NULL },
	{ "self-x-start", ANCHOR_AREA_X, 0, 0, NULL },
	{ "self-x-end", ANCHOR_AREA_X, 2, 2, NULL },
	{ "span-self-x-start", ANCHOR_AREA_X, 0, 1, NULL },
	{ "span-self-x-end", ANCHOR_AREA_X, 1, 2, NULL },
	{ "top", ANCHOR_AREA_Y, 0, 0, NULL },
	{ "bottom", ANCHOR_AREA_Y, 2, 2, NULL },
	{ "span-top", ANCHOR_AREA_Y, 0, 1, NULL },
	{ "span-bottom", ANCHOR_AREA_Y, 1, 2, NULL },
	{ "y-start", ANCHOR_AREA_Y, 0, 0, NULL },
	{ "y-end", ANCHOR_AREA_Y, 2, 2, NULL },
	{ "span-y-start", ANCHOR_AREA_Y, 0, 1, NULL },
	{ "span-y-end", ANCHOR_AREA_Y, 1, 2, NULL },
	{ "self-y-start", ANCHOR_AREA_Y, 0, 0, NULL },
	{ "self-y-end", ANCHOR_AREA_Y, 2, 2, NULL },
	{ "span-self-y-start", ANCHOR_AREA_Y, 0, 1, NULL },
	{ "span-self-y-end", ANCHOR_AREA_Y, 1, 2, NULL },
	{ "block-start", ANCHOR_AREA_BLOCK, 0, 0, "start" },
	{ "block-end", ANCHOR_AREA_BLOCK, 2, 2, "end" },
	{ "span-block-start", ANCHOR_AREA_BLOCK, 0, 1, "span-start" },
	{ "span-block-end", ANCHOR_AREA_BLOCK, 1, 2, "span-end" },
	{ "inline-start", ANCHOR_AREA_INLINE, 0, 0, "start" },
	{ "inline-end", ANCHOR_AREA_INLINE, 2, 2, "end" },
	{ "span-inline-start", ANCHOR_AREA_INLINE, 0, 1, "span-start" },
	{ "span-inline-end", ANCHOR_AREA_INLINE, 1, 2, "span-end" },
	{ "self-block-start", ANCHOR_AREA_SELF_BLOCK, 0, 0, "self-start" },
	{ "self-block-end", ANCHOR_AREA_SELF_BLOCK, 2, 2, "self-end" },
	{ "span-self-block-start", ANCHOR_AREA_SELF_BLOCK, 0, 1, "span-self-start" },
	{ "span-self-block-end", ANCHOR_AREA_SELF_BLOCK, 1, 2, "span-self-end" },
	{ "self-inline-start", ANCHOR_AREA_SELF_INLINE, 0, 0, "self-start" },
	{ "self-inline-end", ANCHOR_AREA_SELF_INLINE, 2, 2, "self-end" },
	{ "span-self-inline-start", ANCHOR_AREA_SELF_INLINE, 0, 1, "span-self-start" },
	{ "span-self-inline-end", ANCHOR_AREA_SELF_INLINE, 1, 2, "span-self-end" },
	{ "start", ANCHOR_AREA_GENERIC, 0, 0, NULL },
	{ "end", ANCHOR_AREA_GENERIC, 2, 2, NULL },
	{ "span-start", ANCHOR_AREA_GENERIC, 0, 1, NULL },
	{ "span-end", ANCHOR_AREA_GENERIC, 1, 2, NULL },
	{ "self-start", ANCHOR_AREA_SELF_GENERIC, 0, 0, NULL },
	{ "self-end", ANCHOR_AREA_SELF_GENERIC, 2, 2, NULL },
	{ "span-self-start", ANCHOR_AREA_SELF_GENERIC, 0, 1, NULL },
	{ "span-self-end", ANCHOR_AREA_SELF_GENERIC, 1, 2, NULL },
	{ "center", ANCHOR_AREA_CENTER, 1, 1, NULL },
	{ "span-all", ANCHOR_AREA_ALL, 0, 2, NULL }
};

#define ANCHOR_AREA_KEYWORDS	(sizeof(anchor_area_keywords) / sizeof(anchor_area_keywords[0]))

static int anchor_area_pair(int first, int second);
static int anchor_area_append(struct wb_units *out, const char *ascii);

/*
 * Parses position-area: none, or one or two keywords on different axes
 * (physical with physical, logical with logical, the generic ones
 * together; center and span-all with any).  The value is packed as
 * first + 256 * (second + 1), -1 for none.
 */
int
css_position_area_parse(
	const struct css_token *tokens,
	size_t count,
	int *packed)
{
	int keywords[2];
	int found;
	size_t index;
	size_t used;

	/* Up to two words. */
	used = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type != CSS_TOKEN_IDENT || used == 2)
			return EINVAL;
		if (used == 0 && css_ident_equal(&tokens[index], "none")) {
			keywords[used++] = -2;
			continue;
		}
		for (found = 0; found < (int)ANCHOR_AREA_KEYWORDS; found++) {
			if (css_ident_equal(&tokens[index], anchor_area_keywords[found].name))
				break;
		}
		if (found == (int)ANCHOR_AREA_KEYWORDS)
			return EINVAL;
		keywords[used++] = found;
	}
	if (used == 0)
		return EINVAL;

	/* none alone. */
	if (keywords[0] == -2) {
		if (used != 1)
			return EINVAL;
		*packed = -1;
		return 0;
	}
	if (used == 2 && (keywords[1] == -2 || !anchor_area_pair(keywords[0], keywords[1])))
		return EINVAL;

	/* Succeeded: the keywords are kept. */
	*packed = keywords[0] + 256 * (used == 2 ? keywords[1] + 1 : 0);
	return 0;
}

/* Writes a position-area value as it is computed: the logical keywords in their short forms. */
int
css_position_area_serialize(
	int packed,
	struct wb_units *out)
{
	const struct anchor_area_keyword *first;
	const struct anchor_area_keyword *second;
	const struct anchor_area_keyword *swap;
	const char *a;
	const char *b;
	int axis;

	/* none, and one keyword. */
	if (packed < 0)
		return anchor_area_append(out, "none");
	first = &anchor_area_keywords[packed % 256];
	if (packed / 256 == 0)
		return anchor_area_append(out, first->name);
	second = &anchor_area_keywords[packed / 256 - 1];

	/* The same keyword twice is one, as are center center and span-all span-all. */
	if (first == second)
		return anchor_area_append(out, first->name);
	if ((first->axis == ANCHOR_AREA_CENTER || first->axis == ANCHOR_AREA_ALL) &&
	    (second->axis == ANCHOR_AREA_CENTER || second->axis == ANCHOR_AREA_ALL))
		goto both;

	/* The generic keywords: span-all makes the other a block (first) or inline (second) keyword. */
	if (first->axis == ANCHOR_AREA_GENERIC || first->axis == ANCHOR_AREA_SELF_GENERIC ||
	    second->axis == ANCHOR_AREA_GENERIC || second->axis == ANCHOR_AREA_SELF_GENERIC) {
		if (second->axis == ANCHOR_AREA_ALL || first->axis == ANCHOR_AREA_ALL) {
			swap = first->axis == ANCHOR_AREA_ALL ? second : first;
			axis = first->axis == ANCHOR_AREA_ALL ? ANCHOR_AREA_INLINE : ANCHOR_AREA_BLOCK;
			if (swap->axis == ANCHOR_AREA_SELF_GENERIC)
				axis = axis == ANCHOR_AREA_BLOCK ? ANCHOR_AREA_SELF_BLOCK : ANCHOR_AREA_SELF_INLINE;
			for (a = NULL, packed = 0; packed < (int)ANCHOR_AREA_KEYWORDS; packed++) {
				if (anchor_area_keywords[packed].axis == axis && anchor_area_keywords[packed].brief != NULL &&
				    strcmp(anchor_area_keywords[packed].brief, swap->name) == 0)
					a = anchor_area_keywords[packed].name;
			}
			return anchor_area_append(out, a != NULL ? a : swap->name);
		}
		goto both;
	}

	/* span-all leaves the other keyword alone. */
	if (first->axis == ANCHOR_AREA_ALL)
		return anchor_area_append(out, second->name);
	if (second->axis == ANCHOR_AREA_ALL)
		return anchor_area_append(out, first->name);

	/* Horizontal before vertical, block before inline (the logical ones in their short forms), center in the missing place. */
	if (first->axis == ANCHOR_AREA_Y || first->axis == ANCHOR_AREA_INLINE || first->axis == ANCHOR_AREA_SELF_INLINE ||
	    (first->axis == ANCHOR_AREA_CENTER && (second->axis == ANCHOR_AREA_X || second->axis == ANCHOR_AREA_BLOCK ||
	    second->axis == ANCHOR_AREA_SELF_BLOCK))) {
		swap = first;
		first = second;
		second = swap;
	}
	a = first->brief != NULL && second->axis != ANCHOR_AREA_CENTER ? first->brief : first->name;
	b = second->brief != NULL ? second->brief : second->name;
	if (first->brief != NULL && second->axis == ANCHOR_AREA_CENTER)
		a = first->brief;
	if (strcmp(a, b) == 0)
		return anchor_area_append(out, a);
	if (anchor_area_append(out, a) != 0 || anchor_area_append(out, " ") != 0)
		return ENOMEM;
	return anchor_area_append(out, b);

both:
	/* The two as they are. */
	if (anchor_area_append(out, first->name) != 0 || anchor_area_append(out, " ") != 0)
		return ENOMEM;
	return anchor_area_append(out, second->name);
}

/* Writes a position-area value as a declaration sets it: the keywords as given, in their canonical order. */
int
css_position_area_serialize_declared(
	int packed,
	struct wb_units *out)
{
	const struct anchor_area_keyword *first;
	const struct anchor_area_keyword *second;
	const struct anchor_area_keyword *swap;
	const char *a;
	const char *b;

	/* none, and one keyword. */
	if (packed < 0)
		return anchor_area_append(out, "none");
	first = &anchor_area_keywords[packed % 256];
	if (packed / 256 == 0)
		return anchor_area_append(out, first->name);
	second = &anchor_area_keywords[packed / 256 - 1];

	/* The same keyword twice is one, as are center center and span-all span-all. */
	if (first == second)
		return anchor_area_append(out, first->name);
	if ((first->axis == ANCHOR_AREA_CENTER || first->axis == ANCHOR_AREA_ALL) &&
	    (second->axis == ANCHOR_AREA_CENTER || second->axis == ANCHOR_AREA_ALL))
		goto both;

	/* The generic keywords stay as they are, whatever they are paired with. */
	if (first->axis == ANCHOR_AREA_GENERIC || first->axis == ANCHOR_AREA_SELF_GENERIC ||
	    second->axis == ANCHOR_AREA_GENERIC || second->axis == ANCHOR_AREA_SELF_GENERIC) {
		goto both;
	}

	/* span-all leaves the other keyword alone. */
	if (first->axis == ANCHOR_AREA_ALL)
		return anchor_area_append(out, second->name);
	if (second->axis == ANCHOR_AREA_ALL)
		return anchor_area_append(out, first->name);

	/* Horizontal before vertical, block before inline (the logical ones in their short forms), center in the missing place. */
	if (first->axis == ANCHOR_AREA_Y || first->axis == ANCHOR_AREA_INLINE || first->axis == ANCHOR_AREA_SELF_INLINE ||
	    (first->axis == ANCHOR_AREA_CENTER && (second->axis == ANCHOR_AREA_X || second->axis == ANCHOR_AREA_BLOCK ||
	    second->axis == ANCHOR_AREA_SELF_BLOCK))) {
		swap = first;
		first = second;
		second = swap;
	}
	a = first->name;
	b = second->name;
	if (strcmp(a, b) == 0)
		return anchor_area_append(out, a);
	if (anchor_area_append(out, a) != 0 || anchor_area_append(out, " ") != 0)
		return ENOMEM;
	return anchor_area_append(out, b);

both:
	/* The two as they are. */
	if (anchor_area_append(out, first->name) != 0 || anchor_area_append(out, " ") != 0)
		return ENOMEM;
	return anchor_area_append(out, second->name);
}

/*
 * Finds the regions of the 3x3 grid around the anchor a position-area
 * covers, across (x0 to x1) and down (y0 to y1), for a box of a writing
 * mode and direction (its containing block's taken to be the same).
 * Returns 0 for none.
 */
int
css_position_area_regions(
	int packed,
	int cb_mode,
	int cb_direction,
	int self_mode,
	int self_direction,
	int *x0,
	int *x1,
	int *y0,
	int *y1)
{
	const struct anchor_area_keyword *keys[2];
	const struct anchor_area_keyword *key;
	int count;
	int index;
	int axis;
	int first;
	int last;
	int flip;
	int across;
	int self;
	int writing_mode;
	int direction;
	int logical;

	/* The keywords (one alone stands for both axes when it names none). */
	if (packed < 0)
		return 0;
	keys[0] = &anchor_area_keywords[packed % 256];
	count = 1;
	if (packed / 256 != 0) {
		keys[1] = &anchor_area_keywords[packed / 256 - 1];
		count = 2;
	}
	*x0 = 0;
	*x1 = 2;
	*y0 = 0;
	*y1 = 2;

	/* Each keyword's axis: across or down, and whether it runs from the far side. */
	for (index = 0; index < count; index++) {
		key = keys[index];
		axis = key->axis;
		if (axis == ANCHOR_AREA_ALL)
			continue;
		if (axis == ANCHOR_AREA_GENERIC || axis == ANCHOR_AREA_SELF_GENERIC || axis == ANCHOR_AREA_CENTER) {
			if (count == 1) {
				/* Both axes. */
				css_position_area_regions(key - anchor_area_keywords + 256 * (int)(key - anchor_area_keywords + 1),
				    cb_mode, cb_direction, self_mode, self_direction, x0, x1, y0, y1);
				if (axis != ANCHOR_AREA_CENTER)
					return 1;
				*x0 = *x1 = *y0 = *y1 = 1;
				return 1;
			}
			if (axis == ANCHOR_AREA_CENTER) {
				/* The axis the other keyword leaves. */
				axis = keys[1 - index]->axis;
				if (axis == ANCHOR_AREA_X)
					axis = ANCHOR_AREA_Y;
				else if (axis == ANCHOR_AREA_Y)
					axis = ANCHOR_AREA_X;
				else if (axis == ANCHOR_AREA_BLOCK || axis == ANCHOR_AREA_SELF_BLOCK)
					axis = ANCHOR_AREA_INLINE;
				else if (axis == ANCHOR_AREA_INLINE || axis == ANCHOR_AREA_SELF_INLINE)
					axis = ANCHOR_AREA_BLOCK;
				else
					axis = index == 0 ? ANCHOR_AREA_BLOCK : ANCHOR_AREA_INLINE;
			} else {
				axis = index == 0 ? ANCHOR_AREA_BLOCK : ANCHOR_AREA_INLINE;
			}
		}
		if (axis == ANCHOR_AREA_SELF_BLOCK)
			axis = ANCHOR_AREA_BLOCK;
		if (axis == ANCHOR_AREA_SELF_INLINE)
			axis = ANCHOR_AREA_INLINE;

		/* The keywords with self use the box's own writing mode and direction, the others the containing block's. */
		self = strstr(key->name, "self") != NULL;
		writing_mode = self ? self_mode : cb_mode;
		direction = self ? self_direction : cb_direction;

		/* sideways-lr (the mode 3) is vertical-lr with its inline axis the other way up. */
		if (writing_mode == 3) {
			writing_mode = CSS_WRITING_VERTICAL_LR;
			direction = direction == CSS_DIRECTION_RTL ? CSS_DIRECTION_LTR : CSS_DIRECTION_RTL;
		}

		/* The physical axis and its direction (left, right, top and bottom do not turn with the mode). */
		logical = strstr(key->name, "start") != NULL || strstr(key->name, "end") != NULL;
		flip = 0;
		across = axis == ANCHOR_AREA_X;
		if (axis == ANCHOR_AREA_X && logical) {
			/* The x axis is the inline axis of a horizontal mode, the block axis of a vertical one. */
			flip = writing_mode == CSS_WRITING_HORIZONTAL_TB ? direction == CSS_DIRECTION_RTL :
			    writing_mode == CSS_WRITING_VERTICAL_RL;
		}
		if (axis == ANCHOR_AREA_Y && logical) {
			/* The y axis is the inline axis of a vertical mode. */
			flip = writing_mode != CSS_WRITING_HORIZONTAL_TB && direction == CSS_DIRECTION_RTL;
		}
		if (axis == ANCHOR_AREA_BLOCK) {
			across = writing_mode != CSS_WRITING_HORIZONTAL_TB;
			flip = writing_mode == CSS_WRITING_VERTICAL_RL;
		}
		if (axis == ANCHOR_AREA_INLINE) {
			across = writing_mode == CSS_WRITING_HORIZONTAL_TB;
			flip = direction == CSS_DIRECTION_RTL;
		}
		first = key->first;
		last = key->last;
		if (flip) {
			first = 2 - key->last;
			last = 2 - key->first;
		}
		if (across) {
			*x0 = first;
			*x1 = last;
		} else {
			*y0 = first;
			*y1 = last;
		}
	}

	/* Succeeded: the regions are known. */
	return 1;
}

/* Tells whether two keywords can make a position-area together. */
static int
anchor_area_pair(
	int first,
	int second)
{
	int a;
	int b;

	/* center and span-all go with any keyword. */
	a = anchor_area_keywords[first].axis;
	b = anchor_area_keywords[second].axis;
	if (a == ANCHOR_AREA_CENTER || a == ANCHOR_AREA_ALL || b == ANCHOR_AREA_CENTER || b == ANCHOR_AREA_ALL)
		return 1;

	/* Otherwise the two axes of one kind. */
	if ((a == ANCHOR_AREA_X && b == ANCHOR_AREA_Y) || (a == ANCHOR_AREA_Y && b == ANCHOR_AREA_X))
		return 1;
	if ((a == ANCHOR_AREA_BLOCK && b == ANCHOR_AREA_INLINE) || (a == ANCHOR_AREA_INLINE && b == ANCHOR_AREA_BLOCK))
		return 1;
	if ((a == ANCHOR_AREA_SELF_BLOCK && b == ANCHOR_AREA_SELF_INLINE) || (a == ANCHOR_AREA_SELF_INLINE && b == ANCHOR_AREA_SELF_BLOCK))
		return 1;
	return (a == ANCHOR_AREA_GENERIC && b == ANCHOR_AREA_GENERIC) ||
	    (a == ANCHOR_AREA_SELF_GENERIC && b == ANCHOR_AREA_SELF_GENERIC);
}

/* Appends ASCII characters. */
static int
anchor_area_append(
	struct wb_units *out,
	const char *ascii)
{
	uint16_t unit;
	size_t index;
	int error;

	/* Each character. */
	for (index = 0; ascii[index] != '\0'; index++) {
		unit = (uint16_t)(unsigned char)ascii[index];
		error = wb_units_append(out, &unit, 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/* The try-tactics of position-try-fallbacks, by their ids. */
static const char *const anchor_tactics[CSS_TRY_TACTICS] = {
	"flip-block", "flip-inline", "flip-start", "flip-x", "flip-y"
};

/* The keywords of position-try-order, by their values. */
static const char *const anchor_try_orders[5] = {
	"normal", "most-width", "most-height", "most-block-size", "most-inline-size"
};

/* The keywords of position-visibility, by their bits. */
static const char *const anchor_visibilities[3] = {
	"anchors-valid", "anchors-visible", "no-overflow"
};

static int anchor_try_entry(struct css_parse *parse, const struct css_token *tokens, size_t count, struct css_try_entry *entry);

/*
 * Parses position-try-fallbacks: none, or entries a comma apart, each a
 * position-area, or a name of a @position-try rule and try-tactics (in
 * either order), into a list in the parse's arena (NULL for none).
 */
int
css_try_fallbacks_parse(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	const struct css_try_list **list)
{
	struct css_try_list *made;
	size_t start;
	size_t index;
	int error;

	/* none alone. */
	while (count > 0 && tokens[count - 1U].type == CSS_TOKEN_WHITESPACE)
		count--;
	start = 0;
	while (start < count && tokens[start].type == CSS_TOKEN_WHITESPACE)
		start++;
	if (start + 1U == count && css_ident_equal(&tokens[start], "none")) {
		*list = NULL;
		return 0;
	}

	/* The entries. */
	made = wb_arena_alloc(parse->arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));
	for (index = start; index <= count; index++) {
		if (index < count && tokens[index].type != CSS_TOKEN_COMMA)
			continue;
		if (made->count == CSS_TRY_ENTRIES)
			return EINVAL;
		error = anchor_try_entry(parse, tokens + start, index - start, &made->entries[made->count]);
		if (error != 0)
			return error;
		made->count++;
		start = index + 1U;
	}

	/* Succeeded: the list is parsed. */
	*list = made;
	return 0;
}

/* Parses one entry of position-try-fallbacks. */
static int
anchor_try_entry(
	struct css_parse *parse,
	const struct css_token *tokens,
	size_t count,
	struct css_try_entry *entry)
{
	size_t index;
	int tactic;
	int other;
	int area;
	int name_after;
	int error;

	/* A position-area. */
	name_after = 0;
	memset(entry, 0, sizeof(*entry));
	entry->area = -2;
	error = css_position_area_parse(tokens, count, &area);
	if (error == 0 && area >= 0) {
		entry->area = area;
		return 0;
	}

	/* Otherwise a name and tactics (each once, side by side, with the name before or after them). */
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (anchor_is_dashed(&tokens[index]) && entry->name == NULL) {
			if (entry->tactic_count > 0)
				name_after = 1;
			entry->name = vm_atom_from_units(parse->heap, tokens[index].text, tokens[index].length);
			if (entry->name == NULL)
				return ENOMEM;
			continue;
		}
		for (tactic = 0; tactic < CSS_TRY_TACTICS; tactic++) {
			if (css_ident_equal(&tokens[index], anchor_tactics[tactic]))
				break;
		}
		if (tactic == CSS_TRY_TACTICS || entry->tactic_count == CSS_TRY_TACTICS || name_after)
			return EINVAL;
		for (other = 0; other < entry->tactic_count; other++) {
			if (entry->tactics[other] == tactic)
				return EINVAL;
		}
		entry->tactics[entry->tactic_count++] = tactic;
	}
	if (entry->name == NULL && entry->tactic_count == 0)
		return EINVAL;

	/* Succeeded: the entry is parsed. */
	return 0;
}

/* Writes position-try-fallbacks (NULL is none). */
int
css_try_fallbacks_serialize(
	const struct css_try_list *list,
	struct wb_units *out)
{
	const struct css_try_entry *entry;
	int index;
	int tactic;
	int error;

	/* none. */
	if (list == NULL || list->count == 0)
		return anchor_area_append(out, "none");

	/* Each entry: its area, or its name then its tactics. */
	for (index = 0; index < list->count; index++) {
		entry = &list->entries[index];
		if (index > 0 && anchor_area_append(out, ", ") != 0)
			return ENOMEM;
		if (entry->area >= 0) {
			error = css_position_area_serialize(entry->area, out);
			if (error != 0)
				return error;
			continue;
		}
		if (entry->name != NULL) {
			error = css_serialize_ident(entry->name, out);
			if (error != 0)
				return error;
		}
		for (tactic = 0; tactic < entry->tactic_count; tactic++) {
			if ((entry->name != NULL || tactic > 0) && anchor_area_append(out, " ") != 0)
				return ENOMEM;
			if (anchor_area_append(out, anchor_tactics[entry->tactics[tactic]]) != 0)
				return ENOMEM;
		}
	}
	return 0;
}

/* Parses position-try-order's keyword. */
int
css_try_order_parse(
	const struct css_token *tokens,
	size_t count,
	int *order)
{
	size_t index;
	int found;

	/* One word. */
	found = -1;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (found >= 0 || tokens[index].type != CSS_TOKEN_IDENT)
			return EINVAL;
		for (found = 0; found < 5; found++) {
			if (css_ident_equal(&tokens[index], anchor_try_orders[found]))
				break;
		}
		if (found == 5)
			return EINVAL;
	}
	if (found < 0)
		return EINVAL;
	*order = found;
	return 0;
}

/* Writes position-try-order. */
int
css_try_order_serialize(
	int order,
	struct wb_units *out)
{
	/* The keyword. */
	if (order < 0 || order > 4)
		order = 0;
	return anchor_area_append(out, anchor_try_orders[order]);
}

/*
 * Parses position-visibility: always (0), or each of anchors-valid (1),
 * anchors-visible (2) and no-overflow (4) once.
 */
int
css_position_visibility_parse(
	const struct css_token *tokens,
	size_t count,
	int *bits)
{
	size_t index;
	int seen;
	int words;
	int bit;

	/* The words. */
	seen = 0;
	words = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_WHITESPACE)
			continue;
		if (tokens[index].type != CSS_TOKEN_IDENT)
			return EINVAL;
		words++;
		if (css_ident_equal(&tokens[index], "always")) {
			if (words != 1)
				return EINVAL;
			seen = -1;
			continue;
		}
		if (seen < 0)
			return EINVAL;
		for (bit = 0; bit < 3; bit++) {
			if (css_ident_equal(&tokens[index], anchor_visibilities[bit]))
				break;
		}
		if (bit == 3 || (seen & (1 << bit)) != 0)
			return EINVAL;
		seen |= 1 << bit;
	}
	if (words == 0)
		return EINVAL;
	*bits = seen < 0 ? 0 : seen;
	return 0;
}

/* Writes position-visibility. */
int
css_position_visibility_serialize(
	int bits,
	struct wb_units *out)
{
	int bit;
	int written;

	/* always, or the keywords in order. */
	if (bits == 0)
		return anchor_area_append(out, "always");
	written = 0;
	for (bit = 0; bit < 3; bit++) {
		if ((bits & (1 << bit)) == 0)
			continue;
		if (written++ > 0 && anchor_area_append(out, " ") != 0)
			return ENOMEM;
		if (anchor_area_append(out, anchor_visibilities[bit]) != 0)
			return ENOMEM;
	}
	return 0;
}

/*
 * Writes a declared value of position-try, position-try-fallbacks or
 * position-visibility in its canonical form (handled is 0 for another
 * property or a value that does not parse).
 */
int
css_anchor_canonical(
	struct vm_heap *heap,
	const struct vm_string *name,
	const struct vm_string *value,
	struct wb_units *out,
	int *handled)
{
	const struct css_try_list *list;
	struct css_parse parse;
	struct css_token *tokens;
	struct wb_units text;
	struct wb_arena arena;
	size_t count;
	size_t first;
	int shorthand;
	int fallbacks;
	int visibility;
	int area;
	int order;
	int error;

	/* The properties. */
	*handled = 0;
	shorthand = vm_string_equal_ascii(name, "position-try");
	fallbacks = vm_string_equal_ascii(name, "position-try-fallbacks");
	visibility = vm_string_equal_ascii(name, "position-visibility");
	area = vm_string_equal_ascii(name, "position-area");
	if (!shorthand && !fallbacks && !visibility && !area)
		return 0;

	/* The value's tokens. */
	wb_units_init(&text);
	wb_arena_init(&arena, 0);
	error = vm_string_append_units(value, &text);
	if (error == 0)
		error = css_tokenize(&arena, text.data, text.length, &tokens, &count);
	if (error != 0)
		goto cleanup;
	while (count > 0 && (tokens[count - 1U].type == CSS_TOKEN_EOF || tokens[count - 1U].type == CSS_TOKEN_WHITESPACE))
		count--;
	parse.heap = heap;
	parse.arena = &arena;

	/* position-area. */
	if (area) {
		if (css_position_area_parse(tokens, count, &order) == 0) {
			error = css_position_area_serialize_declared(order, out);
			*handled = error == 0;
		}
		goto cleanup;
	}

	/* position-visibility. */
	if (visibility) {
		if (css_position_visibility_parse(tokens, count, &order) == 0) {
			error = css_position_visibility_serialize(order, out);
			*handled = error == 0;
		}
		goto cleanup;
	}

	/* position-try's order, when it has one other than normal. */
	first = 0;
	order = 0;
	if (shorthand) {
		while (first < count && tokens[first].type == CSS_TOKEN_WHITESPACE)
			first++;
		if (first < count && css_try_order_parse(tokens + first, 1, &order) == 0)
			first++;
	}

	/* The fallbacks. */
	if (css_try_fallbacks_parse(&parse, tokens + first, count - first, &list) != 0)
		goto cleanup;
	if (order != 0) {
		error = css_try_order_serialize(order, out);
		if (error == 0)
			error = anchor_area_append(out, " ");
	}
	if (error == 0)
		error = css_try_fallbacks_serialize(list, out);
	*handled = error == 0;

cleanup:
	wb_arena_release(&arena);
	wb_units_release(&text);
	return error;
}

/* The longhands of position-try (a list that ends with NULL), or NULL for another property. */
const char *const *
css_anchor_shorthand_longhands(
	const struct vm_string *name)
{
	static const char *const longhands[] = { "position-try-order", "position-try-fallbacks", NULL };

	/* position-try. */
	if (vm_string_equal_ascii(name, "position-try"))
		return longhands;
	return NULL;
}

/* Writes the value a position-try declaration gives one of its longhands (found 0 when it does not parse). */
int
css_anchor_shorthand_part(
	struct vm_heap *heap,
	const struct vm_string *value,
	const char *longhand,
	struct wb_units *out,
	int *found)
{
	const struct css_try_list *list;
	struct css_parse parse;
	struct css_token *tokens;
	struct wb_units text;
	struct wb_arena arena;
	size_t count;
	size_t first;
	int order;
	int error;

	/* The tokens. */
	*found = 0;
	wb_units_init(&text);
	wb_arena_init(&arena, 0);
	error = vm_string_append_units(value, &text);
	if (error == 0)
		error = css_tokenize(&arena, text.data, text.length, &tokens, &count);
	if (error != 0)
		goto cleanup;
	while (count > 0 && (tokens[count - 1U].type == CSS_TOKEN_EOF || tokens[count - 1U].type == CSS_TOKEN_WHITESPACE))
		count--;
	parse.heap = heap;
	parse.arena = &arena;

	/* The order, then the fallbacks. */
	first = 0;
	order = 0;
	while (first < count && tokens[first].type == CSS_TOKEN_WHITESPACE)
		first++;
	if (first < count && css_try_order_parse(tokens + first, 1, &order) == 0)
		first++;
	if (css_try_fallbacks_parse(&parse, tokens + first, count - first, &list) != 0)
		goto cleanup;
	if (strcmp(longhand, "position-try-order") == 0)
		error = css_try_order_serialize(order, out);
	else
		error = css_try_fallbacks_serialize(list, out);
	*found = error == 0;

cleanup:
	wb_arena_release(&arena);
	wb_units_release(&text);
	return error;
}
