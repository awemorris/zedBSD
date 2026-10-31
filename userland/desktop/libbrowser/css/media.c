/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Media queries (ws074-p061): the lists of @media rules, of @import rules
 * and of <link media> read into tests of the viewport, and evaluated when
 * an element is styled (a sheet is parsed once and used at any size).
 *
 * The browser is a screen with a fine pointer that hovers, one device
 * pixel per CSS pixel, a light color scheme and no wish for reduced
 * motion; the features that depend only on that are decided while
 * parsing, and the width, height, aspect ratio and orientation stay tests.
 * A feature this pass does not know makes its query false.
 */

#include "css/internal.h"

#include <errno.h>
#include <string.h>

/* The most tests one query keeps (the rest of a longer query are dropped). */
#define MEDIA_TESTS_MAX 8

/* The pixels an em is worth in a media query (the initial font size). */
#define MEDIA_EM_PIXELS 16.0f

/* The features a test measures. */
enum media_feature {
	MEDIA_FEATURE_CONSTANT,
	MEDIA_FEATURE_WIDTH,
	MEDIA_FEATURE_HEIGHT,
	MEDIA_FEATURE_ASPECT_RATIO,
	MEDIA_FEATURE_ORIENTATION,
	MEDIA_FEATURE_RESOLUTION,
	MEDIA_FEATURE_COLOR,
	MEDIA_FEATURE_MONOCHROME
};

/* How a test compares the feature with its value. */
enum media_comparison {
	MEDIA_EQUAL,
	MEDIA_AT_LEAST,
	MEDIA_AT_MOST,
	MEDIA_MORE,
	MEDIA_LESS
};

/*
 * One named feature: its name, what it measures, and the comparison its
 * min- or max- form makes.
 */
struct media_name {
	const char *name;
	int feature;
	int comparison;
};

/*
 * A discrete feature's value this browser has: the feature, and the
 * values that are true of it (a list ended by NULL; the feature alone,
 * without a value, is true when the first value is not "none").
 */
struct media_discrete {
	const char *name;
	const char *values[3];
};

/* The range features by their names (the -webkit- pixel ratios are resolutions in dppx). */
static const struct media_name media_names[] = {
    {"color", MEDIA_FEATURE_COLOR, MEDIA_EQUAL},
    {"min-color", MEDIA_FEATURE_COLOR, MEDIA_AT_LEAST},
    {"max-color", MEDIA_FEATURE_COLOR, MEDIA_AT_MOST},
    {"monochrome", MEDIA_FEATURE_MONOCHROME, MEDIA_EQUAL},
    {"min-monochrome", MEDIA_FEATURE_MONOCHROME, MEDIA_AT_LEAST},
    {"max-monochrome", MEDIA_FEATURE_MONOCHROME, MEDIA_AT_MOST},
    {"inline-size", MEDIA_FEATURE_WIDTH, MEDIA_EQUAL},
    {"min-inline-size", MEDIA_FEATURE_WIDTH, MEDIA_AT_LEAST},
    {"max-inline-size", MEDIA_FEATURE_WIDTH, MEDIA_AT_MOST},
    {"block-size", MEDIA_FEATURE_HEIGHT, MEDIA_EQUAL},
    {"min-block-size", MEDIA_FEATURE_HEIGHT, MEDIA_AT_LEAST},
    {"max-block-size", MEDIA_FEATURE_HEIGHT, MEDIA_AT_MOST},
    {"width", MEDIA_FEATURE_WIDTH, MEDIA_EQUAL},
    {"min-width", MEDIA_FEATURE_WIDTH, MEDIA_AT_LEAST},
    {"max-width", MEDIA_FEATURE_WIDTH, MEDIA_AT_MOST},
    {"height", MEDIA_FEATURE_HEIGHT, MEDIA_EQUAL},
    {"min-height", MEDIA_FEATURE_HEIGHT, MEDIA_AT_LEAST},
    {"max-height", MEDIA_FEATURE_HEIGHT, MEDIA_AT_MOST},
    {"device-width", MEDIA_FEATURE_WIDTH, MEDIA_EQUAL},
    {"min-device-width", MEDIA_FEATURE_WIDTH, MEDIA_AT_LEAST},
    {"max-device-width", MEDIA_FEATURE_WIDTH, MEDIA_AT_MOST},
    {"aspect-ratio", MEDIA_FEATURE_ASPECT_RATIO, MEDIA_EQUAL},
    {"min-aspect-ratio", MEDIA_FEATURE_ASPECT_RATIO, MEDIA_AT_LEAST},
    {"max-aspect-ratio", MEDIA_FEATURE_ASPECT_RATIO, MEDIA_AT_MOST},
    {"resolution", MEDIA_FEATURE_RESOLUTION, MEDIA_EQUAL},
    {"min-resolution", MEDIA_FEATURE_RESOLUTION, MEDIA_AT_LEAST},
    {"max-resolution", MEDIA_FEATURE_RESOLUTION, MEDIA_AT_MOST},
    {"-webkit-device-pixel-ratio", MEDIA_FEATURE_RESOLUTION, MEDIA_EQUAL},
    {"-webkit-min-device-pixel-ratio", MEDIA_FEATURE_RESOLUTION, MEDIA_AT_LEAST},
    {"-webkit-max-device-pixel-ratio", MEDIA_FEATURE_RESOLUTION, MEDIA_AT_MOST},
    {NULL, 0, 0}};

/* The discrete features and the values the browser has. */
static const struct media_discrete media_discretes[] = {
    {"prefers-reduced-motion", {"no-preference", NULL, NULL}},
    {"prefers-color-scheme", {"light", NULL, NULL}},
    {"prefers-contrast", {"no-preference", NULL, NULL}},
    {"prefers-reduced-transparency", {"no-preference", NULL, NULL}},
    {"forced-colors", {"none", NULL, NULL}},
    {"inverted-colors", {"none", NULL, NULL}},
    {"hover", {"hover", NULL, NULL}},
    {"any-hover", {"hover", NULL, NULL}},
    {"pointer", {"fine", NULL, NULL}},
    {"any-pointer", {"fine", NULL, NULL}},
    {"display-mode", {"browser", NULL, NULL}},
    {"scan", {"progressive", NULL, NULL}},
    {"update", {"fast", NULL, NULL}},
    {"color-gamut", {"srgb", NULL, NULL}},
    {"dynamic-range", {"standard", NULL, NULL}},
    {"grid", {"none", NULL, NULL}},
    {NULL, {NULL, NULL, NULL}}};

static int media_query(struct wb_arena *arena, const struct css_token *tokens, size_t count, struct css_media_query *query);
static int media_condition(const struct css_token *tokens, size_t count, struct css_media_test *test);
static int media_range(const struct css_token *tokens, size_t count, struct css_media_test *tests, size_t *made);
static int media_feature_name(const struct css_token *token, int *feature, int *comparison);
static int media_discrete_true(const struct css_token *name, const struct css_token *value);
static int media_value(const struct css_token *tokens, size_t count, int feature, float *value);
static int media_operator(const struct css_token *tokens, size_t count, size_t *index, int *comparison);
static int media_flip(int comparison);
static int media_test_holds(const struct css_media_test *test, float width, float height);
static int media_query_holds(const struct css_media_query *query, float width, float height);
static size_t media_skip_space(const struct css_token *tokens, size_t count, size_t index);

/*
 * Reads a media query list into the arena.
 * The tokens come from an @media prelude, @import media or a media attribute.
 * The parent is the enclosing @media list, or NULL at the top; it must hold too.
 * An empty list is true.
 */
int
css_media_parse(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	const struct css_media *parent,
	struct css_media **media)
{
	struct css_media *made;
	size_t start;
	size_t index;
	size_t commas;
	int depth;
	int error;

	/* Counts the queries, which commas outside parentheses separate. */
	commas = 0;
	depth = 0;
	for (index = 0; index < count; index++) {
		if (tokens[index].type == CSS_TOKEN_OPEN_PAREN || tokens[index].type == CSS_TOKEN_FUNCTION)
			depth++;
		if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
			depth--;
		if (tokens[index].type == CSS_TOKEN_COMMA && depth == 0)
			commas++;
	}

	/* The list and its queries. */
	made = wb_arena_zalloc(arena, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The arena owns the list and its separately checked query storage. */
	made->parent = parent;
	made->queries = wb_arena_zalloc(arena, (commas + 1U) * sizeof(struct css_media_query));
	if (made->queries == NULL)
		return ENOMEM;

	/* Reads each query between the commas; an all-whitespace list has none. */
	start = 0;
	depth = 0;
	for (index = 0; index <= count; index++) {
		if (index < count) {
			if (tokens[index].type == CSS_TOKEN_OPEN_PAREN || tokens[index].type == CSS_TOKEN_FUNCTION)
				depth++;
			if (tokens[index].type == CSS_TOKEN_CLOSE_PAREN)
				depth--;
			if (tokens[index].type != CSS_TOKEN_COMMA || depth != 0)
				continue;
		}

		/* The query between start and index. */
		error = media_query(arena, tokens + start, index - start, &made->queries[made->query_count]);
		if (error == ENOMEM)
			return error;
		if (error == 0)
			made->query_count++;
		start = index + 1U;
	}

	/* Succeeded: the list is read. */
	*media = made;
	return 0;
}

/*
 * Tests a media query list and its parents against the CSS viewport.
 * A NULL list holds at every viewport size.
 */
int
css_media_matches(
	const struct css_media *media,
	float width,
	float height)
{
	const struct css_media *list;
	size_t index;
	int holds;

	/* Every list of the chain must hold. */
	for (list = media; list != NULL; list = list->parent) {
		/* A list without queries (an empty prelude) holds. */
		if (list->query_count == 0)
			continue;

		/* One of its queries must hold. */
		holds = 0;
		for (index = 0; index < list->query_count; index++) {
			holds = media_query_holds(&list->queries[index], width, height);
			if (holds)
				break;
		}

		/* No query held. */
		if (!holds)
			return 0;
	}

	/* Every list held. */
	return 1;
}

/*
 * Reads one media query: [not|only] [type] [and (condition)]..., or
 * (condition) [and (condition)]....  Returns EINVAL for an empty one
 * (which the list leaves out) and reads anything it does not understand
 * as a query that is false.
 */
static int
media_query(
	struct wb_arena *arena,
	const struct css_token *tokens,
	size_t count,
	struct css_media_query *query)
{
	struct css_media_test tests[MEDIA_TESTS_MAX];
	size_t index;
	size_t end;
	size_t made;
	size_t range_made;
	int is_word;
	int error;

	/* The query starts true of every type. */
	memset(query, 0, sizeof(*query));
	query->type_matches = 1;
	index = media_skip_space(tokens, count, 0);
	if (index >= count)
		return EINVAL;

	/* not and only in front. */
	if (tokens[index].type == CSS_TOKEN_IDENT) {
		is_word = css_ident_equal(&tokens[index], "not");
		if (is_word) {
			query->negate = 1;
			index = media_skip_space(tokens, count, index + 1U);
		} else {
			is_word = css_ident_equal(&tokens[index], "only");
			if (is_word)
				index = media_skip_space(tokens, count, index + 1U);
		}
	}

	/* A media type: all and screen are this browser's, any other is not. */
	made = 0;
	if (index < count && tokens[index].type == CSS_TOKEN_IDENT) {
		is_word = css_ident_equal(&tokens[index], "all");
		if (!is_word)
			is_word = css_ident_equal(&tokens[index], "screen");
		query->type_matches = is_word;
		index = media_skip_space(tokens, count, index + 1U);

		/* After the type, each condition follows and. */
		if (index < count) {
			is_word = 0;
			if (tokens[index].type == CSS_TOKEN_IDENT)
				is_word = css_ident_equal(&tokens[index], "and");
			if (!is_word) {
				query->type_matches = 0;
				index = count;
			}

			/* The and is read. */
			index = media_skip_space(tokens, count, index + 1U);
		}
	}

	/* The conditions in parentheses, joined by and. */
	while (index < count) {
		if (tokens[index].type != CSS_TOKEN_OPEN_PAREN) {
			/* Anything but a condition (or, a function) makes the query false. */
			query->type_matches = 0;
			break;
		}

		/* The condition's tokens up to its closing parenthesis. */
		end = index + 1U;
		while (end < count && tokens[end].type != CSS_TOKEN_CLOSE_PAREN)
			end++;
		range_made = 0;
		if (made + 2U <= MEDIA_TESTS_MAX) {
			error = media_range(tokens + index + 1U, end - index - 1U, &tests[made], &range_made);
			if (error != 0) {
				/* A range form did not read: a name and a value, or a name alone. */
				error = media_condition(tokens + index + 1U, end - index - 1U, &tests[made]);
				if (error != 0) {
					tests[made].feature = MEDIA_FEATURE_CONSTANT;
					tests[made].value = 0;
				}

				/* Either recognized syntax or a false constant occupies one test. */
				range_made = 1;
			}
		}

		/* The condition's tests are kept. */
		made += range_made;

		/* Past the parenthesis, then an and before the next condition. */
		index = media_skip_space(tokens, count, end + 1U);
		if (index < count) {
			is_word = 0;
			if (tokens[index].type == CSS_TOKEN_IDENT)
				is_word = css_ident_equal(&tokens[index], "and");
			if (!is_word) {
				query->type_matches = 0;
				break;
			}

			/* The and is read. */
			index = media_skip_space(tokens, count, index + 1U);
		}
	}

	/* Keeps the tests in the arena. */
	if (made > MEDIA_TESTS_MAX)
		made = MEDIA_TESTS_MAX;
	if (made != 0) {
		query->tests = wb_arena_alloc(arena, made * sizeof(struct css_media_test));
		if (query->tests == NULL)
			return ENOMEM;

		/* Copies complete parsed tests after storage allocation succeeds. */
		memcpy(query->tests, tests, made * sizeof(struct css_media_test));
		query->test_count = made;
	}

	/* Succeeded: the query is read. */
	return 0;
}

/*
 * Reads a condition of the form name: value, or name alone, into one
 * test; EINVAL for one it cannot read.
 */
static int
media_condition(
	const struct css_token *tokens,
	size_t count,
	struct css_media_test *test)
{
	const struct css_token *name;
	size_t index;
	size_t end;
	int feature;
	int comparison;
	int known;
	int holds;
	int error;

	/* The feature's name. */
	index = media_skip_space(tokens, count, 0);
	if (index >= count || tokens[index].type != CSS_TOKEN_IDENT)
		return EINVAL;
	name = &tokens[index];
	index = media_skip_space(tokens, count, index + 1U);

	/* A name alone is a boolean test: a range feature is true, a discrete one unless its value is none. */
	if (index >= count) {
		known = media_feature_name(name, &feature, &comparison);
		test->feature = MEDIA_FEATURE_CONSTANT;
		test->value = 1;
		/* Numeric depth features use their actual zero/nonzero display-model value. */
		if (known &&
		    (feature == MEDIA_FEATURE_COLOR || feature == MEDIA_FEATURE_MONOCHROME)) {
			/* A min/max prefix requires an explicit comparison value. */
			if (comparison != MEDIA_EQUAL)
				return EINVAL;

			/* Compare the native depth against zero for the Boolean feature form. */
			test->feature = feature;
			test->comparison = MEDIA_MORE;
			test->value = 0;
		}

		/* Other existing discrete features keep their established Boolean behavior. */
		if (!known) {
			holds = media_discrete_true(name, NULL);
			test->value = (float)holds;
		}

		/* The boolean test is read. */
		return 0;
	}

	/* Otherwise a colon and a value. */
	if (tokens[index].type != CSS_TOKEN_COLON)
		return EINVAL;
	index = media_skip_space(tokens, count, index + 1U);
	if (index >= count)
		return EINVAL;

	/* A range feature compares its value; a discrete one is decided now. */
	known = media_feature_name(name, &feature, &comparison);
	if (!known) {
		/* Discrete values occupy one token, followed only by whitespace. */
		end = media_skip_space(tokens, count, index + 1U);
		if (end != count)
			return EINVAL;

		/* The validated discrete token contributes one constant test. */
		test->feature = MEDIA_FEATURE_CONSTANT;
		holds = 0;
		if (tokens[index].type == CSS_TOKEN_IDENT)
			holds = media_discrete_true(name, &tokens[index]);
		if (tokens[index].type == CSS_TOKEN_NUMBER)
			holds = media_discrete_true(name, &tokens[index]);
		test->value = (float)holds;
		return 0;
	}

	/* orientation takes portrait or landscape. */
	test->feature = feature;
	test->comparison = comparison;
	error = media_value(tokens + index, count - index, feature, &test->value);
	if (error != 0)
		return error;

	/* Succeeded: the test is read. */
	return 0;
}

/*
 * Reads a condition of the range form (width >= 600px, 600px < width,
 * 400px <= width <= 800px) into one or two tests; EINVAL when it is not
 * one.
 */
static int
media_range(
	const struct css_token *tokens,
	size_t count,
	struct css_media_test *tests,
	size_t *made)
{
	size_t index;
	size_t value_start;
	size_t value_end;
	int feature;
	int comparison;
	int second;
	int known;
	int error;

	/* The form that starts with the name: name op value. */
	*made = 0;
	index = media_skip_space(tokens, count, 0);
	if (index >= count)
		return EINVAL;
	if (tokens[index].type == CSS_TOKEN_IDENT) {
		known = media_feature_name(&tokens[index], &feature, &comparison);
		if (!known || comparison != MEDIA_EQUAL)
			return EINVAL;
		index = media_skip_space(tokens, count, index + 1U);
		error = media_operator(tokens, count, &index, &comparison);
		if (error != 0)
			return error;

		/* The value after the operator. */
		index = media_skip_space(tokens, count, index);
		tests[0].feature = feature;
		tests[0].comparison = comparison;
		error = media_value(tokens + index, count - index, feature, &tests[0].value);
		if (error != 0)
			return error;

		/* One test. */
		*made = 1;
		return 0;
	}

	/* The form that starts with a value: value op name [op value]. */
	value_start = index;
	value_end = index;
	while (value_end < count && tokens[value_end].type != CSS_TOKEN_IDENT) {
		/* A comparison's delimiter ends the value. */
		if (tokens[value_end].type == CSS_TOKEN_DELIM &&
		    (tokens[value_end].delim == '<' ||
		     tokens[value_end].delim == '>' ||
		     tokens[value_end].delim == '='))
			break;
		value_end++;
	}

	/* The operator after the value. */
	index = value_end;
	error = media_operator(tokens, count, &index, &comparison);
	if (error != 0)
		return error;
	index = media_skip_space(tokens, count, index);
	if (index >= count || tokens[index].type != CSS_TOKEN_IDENT)
		return EINVAL;
	known = media_feature_name(&tokens[index], &feature, &second);
	if (!known || second != MEDIA_EQUAL)
		return EINVAL;

	/* value op name is name op' value, with the operator turned round. */
	tests[0].feature = feature;
	tests[0].comparison = media_flip(comparison);
	error = media_value(tokens + value_start, value_end - value_start, feature, &tests[0].value);
	if (error != 0)
		return error;
	*made = 1;

	/* A second operator and value after the name. */
	index = media_skip_space(tokens, count, index + 1U);
	if (index >= count)
		return 0;
	error = media_operator(tokens, count, &index, &comparison);
	if (error != 0)
		return error;
	index = media_skip_space(tokens, count, index);
	tests[1].feature = feature;
	tests[1].comparison = comparison;
	error = media_value(tokens + index, count - index, feature, &tests[1].value);
	if (error != 0)
		return error;

	/* Two tests. */
	*made = 2;
	return 0;
}

/* Looks a range feature's name up; returns 1 with what it measures and how its prefix compares. */
static int
media_feature_name(
	const struct css_token *token,
	int *feature,
	int *comparison)
{
	size_t index;
	int same;

	/* Compares with each range feature's name. */
	for (index = 0; media_names[index].name != NULL; index++) {
		same = css_ident_equal(token, media_names[index].name);
		if (same) {
			*feature = media_names[index].feature;
			*comparison = media_names[index].comparison;
			return 1;
		}
	}

	/* orientation is a feature of the viewport too. */
	same = css_ident_equal(token, "orientation");
	if (same) {
		*feature = MEDIA_FEATURE_ORIENTATION;
		*comparison = MEDIA_EQUAL;
		return 1;
	}

	/* Not a range feature. */
	return 0;
}

/*
 * Tells whether a discrete feature has a value (NULL: the feature alone,
 * true unless its value is none); an unknown feature is false.
 */
static int
media_discrete_true(
	const struct css_token *name,
	const struct css_token *value)
{
	size_t index;
	size_t choice;
	int same;
	int differs;
	int number;

	/* Finds the feature. */
	for (index = 0; media_discretes[index].name != NULL; index++) {
		same = css_ident_equal(name, media_discretes[index].name);
		if (!same)
			continue;

		/* Alone, the feature is true unless its value is none. */
		if (value == NULL) {
			differs = strcmp(media_discretes[index].values[0], "none");
			if (differs == 0)
				return 0;
			return 1;
		}

		/* A number is only the eight bits of color. */
		if (value->type == CSS_TOKEN_NUMBER) {
			number = (int)value->number;
			differs = strcmp(media_discretes[index].values[0], "8");
			if (differs != 0 || number != 8)
				return 0;
			return 1;
		}

		/* One of the values the browser has. */
		for (choice = 0; choice < 3 && media_discretes[index].values[choice] != NULL; choice++) {
			same = css_ident_equal(value, media_discretes[index].values[choice]);
			if (same)
				return 1;
		}

		/* Another value of the feature. */
		return 0;
	}

	/* An unknown feature. */
	return 0;
}

/* Reads a feature's value: a length in pixels, a ratio, a resolution in dppx, or an orientation. */
static int
media_value(
	const struct css_token *tokens,
	size_t count,
	int feature,
	float *value)
{
	const struct css_token *token;
	size_t index;
	size_t end;
	float denominator;
	int is_word;

	/* The first token past whitespace. */
	index = media_skip_space(tokens, count, 0);
	if (index >= count)
		return EINVAL;
	token = &tokens[index];

	/* Ordinary values must consume the complete condition token sequence. */
	end = media_skip_space(tokens, count, index + 1U);
	if (feature != MEDIA_FEATURE_ASPECT_RATIO && end != count)
		return EINVAL;

	/* The kind of feature picks the kind of value. */
	switch (feature) {
	case MEDIA_FEATURE_COLOR:
	case MEDIA_FEATURE_MONOCHROME:
		/* Depth comparisons accept one nonnegative integer and trailing whitespace only. */
		if (token->type != CSS_TOKEN_NUMBER ||
		    !token->integer ||
		    token->number < 0)
			return EINVAL;

		/* Reject extra tokens instead of silently accepting a numeric prefix. */
		index = media_skip_space(tokens, count, index + 1U);
		if (index != count)
			return EINVAL;

		/* The exact parsed threshold is compared to the existing display model. */
		*value = (float)token->number;
		return 0;
	case MEDIA_FEATURE_WIDTH:
	case MEDIA_FEATURE_HEIGHT:
		/* A length: zero, px or em (rem) at the initial size. */
		if (token->type == CSS_TOKEN_NUMBER && token->number == 0) {
			*value = 0;
			return 0;
		}

		/* Otherwise a dimension: pixels first. */
		if (token->type != CSS_TOKEN_DIMENSION)
			return EINVAL;
		is_word = css_ident_equal(token, "px");
		if (is_word) {
			*value = (float)token->number;
			return 0;
		}

		/* Then em and rem. */
		is_word = css_ident_equal(token, "em");
		if (!is_word)
			is_word = css_ident_equal(token, "rem");
		if (!is_word)
			return EINVAL;
		*value = (float)token->number * MEDIA_EM_PIXELS;
		return 0;
	case MEDIA_FEATURE_ASPECT_RATIO:
		/* A ratio: a number, or a number / a number. */
		if (token->type != CSS_TOKEN_NUMBER)
			return EINVAL;
		*value = (float)token->number;
		index = media_skip_space(tokens, count, index + 1U);
		if (index < count &&
		    tokens[index].type == CSS_TOKEN_DELIM &&
		    tokens[index].delim == '/') {
			index = media_skip_space(tokens, count, index + 1U);
			if (index >= count || tokens[index].type != CSS_TOKEN_NUMBER)
				return EINVAL;
			denominator = (float)tokens[index].number;
			if (denominator <= 0)
				return EINVAL;
			*value = *value / denominator;
			index = media_skip_space(tokens, count, index + 1U);
		}

		/* A ratio cannot silently ignore an unrelated trailing token. */
		if (index != count)
			return EINVAL;

		/* The ratio is read. */
		return 0;
	case MEDIA_FEATURE_RESOLUTION:
		/* A pixel ratio: a number, or dppx, x, dpi or dpcm. */
		if (token->type == CSS_TOKEN_NUMBER) {
			*value = (float)token->number;
			return 0;
		}

		/* A dimension in dppx or x, or converted from dpi or dpcm. */
		if (token->type != CSS_TOKEN_DIMENSION)
			return EINVAL;
		is_word = css_ident_equal(token, "dppx");
		if (!is_word)
			is_word = css_ident_equal(token, "x");
		if (is_word) {
			*value = (float)token->number;
			return 0;
		}

		/* Converts the two physical density units to CSS pixel density. */
		is_word = css_ident_equal(token, "dpi");
		if (is_word) {
			*value = (float)token->number / 96.0f;
			return 0;
		}

		/* Unknown dimension units cannot act as dppx. */
		is_word = css_ident_equal(token, "dpcm");
		if (!is_word)
			return EINVAL;
		*value = (float)token->number * 2.54f / 96.0f;
		return 0;
	case MEDIA_FEATURE_ORIENTATION:
		/* portrait is 0 and landscape 1. */
		if (token->type != CSS_TOKEN_IDENT)
			return EINVAL;
		is_word = css_ident_equal(token, "portrait");
		if (is_word) {
			*value = 0;
			return 0;
		}

		/* Or landscape. */
		is_word = css_ident_equal(token, "landscape");
		if (!is_word)
			return EINVAL;
		*value = 1;
		return 0;
	default:
		return EINVAL;
	}
}

/* Reads a comparison operator (<, <=, >, >=, =) at *index and moves past it. */
static int
media_operator(
	const struct css_token *tokens,
	size_t count,
	size_t *index,
	int *comparison)
{
	uint32_t first;
	int equals;

	/* The first delimiter. */
	*index = media_skip_space(tokens, count, *index);
	if (*index >= count || tokens[*index].type != CSS_TOKEN_DELIM)
		return EINVAL;
	first = tokens[*index].delim;
	(*index)++;

	/* An = straight after < or > makes it inclusive. */
	equals = 0;
	if (*index < count &&
	    tokens[*index].type == CSS_TOKEN_DELIM &&
	    tokens[*index].delim == '=') {
		equals = 1;
		(*index)++;
	}

	/* The operator the two make. */
	switch (first) {
	case '=':
		*comparison = MEDIA_EQUAL;
		return 0;
	case '<':
		*comparison = MEDIA_LESS;
		if (equals)
			*comparison = MEDIA_AT_MOST;
		return 0;
	case '>':
		*comparison = MEDIA_MORE;
		if (equals)
			*comparison = MEDIA_AT_LEAST;
		return 0;
	default:
		return EINVAL;
	}
}

/* Turns a comparison round, for a value written before the feature. */
static int
media_flip(
	int comparison)
{
	/* The mirror of each comparison. */
	switch (comparison) {
	case MEDIA_AT_LEAST:
		return MEDIA_AT_MOST;
	case MEDIA_AT_MOST:
		return MEDIA_AT_LEAST;
	case MEDIA_MORE:
		return MEDIA_LESS;
	case MEDIA_LESS:
		return MEDIA_MORE;
	default:
		return comparison;
	}
}

/* Tells whether one test holds for the viewport. */
static int
media_test_holds(
	const struct css_media_test *test,
	float width,
	float height)
{
	float actual;

	/* The feature's value in this viewport. */
	switch (test->feature) {
	case MEDIA_FEATURE_CONSTANT:
		return test->value != 0;
	case MEDIA_FEATURE_WIDTH:
		actual = width;
		break;
	case MEDIA_FEATURE_HEIGHT:
		actual = height;
		break;
	case MEDIA_FEATURE_ASPECT_RATIO:
		actual = 0;
		if (height > 0)
			actual = width / height;
		break;
	case MEDIA_FEATURE_ORIENTATION:
		actual = 0;
		if (width > height)
			actual = 1;
		break;
	case MEDIA_FEATURE_COLOR:
		/* The current color output model has eight bits per component. */
		actual = 8;
		break;
	case MEDIA_FEATURE_MONOCHROME:
		/* Color output has no monochrome bits. */
		actual = 0;
		break;
	case MEDIA_FEATURE_RESOLUTION:
		actual = 1;
		break;
	default:
		return 0;
	}

	/* The comparison. */
	switch (test->comparison) {
	case MEDIA_AT_LEAST:
		return actual >= test->value;
	case MEDIA_AT_MOST:
		return actual <= test->value;
	case MEDIA_MORE:
		return actual > test->value;
	case MEDIA_LESS:
		return actual < test->value;
	default:
		return actual == test->value;
	}
}

/* Tells whether one query holds: its type, every test, then its not. */
static int
media_query_holds(
	const struct css_media_query *query,
	float width,
	float height)
{
	size_t index;
	int holds;

	/* The type, then each test. */
	holds = query->type_matches;
	for (index = 0; index < query->test_count && holds; index++) {
		holds = media_test_holds(&query->tests[index], width, height);
	}

	/* not turns the answer round. */
	if (query->negate)
		holds = !holds;

	/* Reports the answer. */
	return holds;
}

/* Skips whitespace tokens from index; returns the first other index (or count). */
static size_t
media_skip_space(
	const struct css_token *tokens,
	size_t count,
	size_t index)
{
	/* Moves past the whitespace. */
	while (index < count && tokens[index].type == CSS_TOKEN_WHITESPACE)
		index++;

	/* Reports where the rest starts. */
	return index;
}
