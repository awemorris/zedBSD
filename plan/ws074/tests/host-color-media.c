/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the native color and monochrome media features without a window.
 */

#include "css/internal.h"

#include <stdio.h>
#include <string.h>

/* One independent query and expected result for the existing color display model. */
struct media_case {
	const char *query;
	int expected;
};

/* The bounded numeric/Boolean/range/syntax corpus lives for the fixture process. */
static const struct media_case media_cases[] = {
    {"(color)", 1},
    {"(monochrome)", 0},
    {"(color: 8)", 1},
    {"(color: 7)", 0},
    {"(monochrome: 0)", 1},
    {"(monochrome: 1)", 0},
    {"(min-color: 0)", 1},
    {"(min-color: 8)", 1},
    {"(min-color: 9)", 0},
    {"(max-color: 7)", 0},
    {"(max-color: 8)", 1},
    {"(max-monochrome: 0)", 1},
    {"(min-monochrome: 0)", 1},
    {"(min-monochrome: 1)", 0},
    {"(color >= 8)", 1},
    {"(color > 8)", 0},
    {"(7 < color <= 8)", 1},
    {"(8 < color < 10)", 0},
    {"(0 <= monochrome < 1)", 1},
    {"(min-color: -1)", 0},
    {"(color: 8.5)", 0},
    {"(color: 8px)", 0},
    {"(color: 8 extra)", 0},
    {"(color: 8 9)", 0},
    {"not all and (monochrome)", 1},
    {"only screen and (color)", 1},
    {"(max-color: 0), (min-monochrome: 0)", 1},
    {"(max-width: 0px) and (min-color: 8)", 1},
    {"(min-width: 1em) and (color)", 0},
    {"(max-width: 0px extra)", 0},
    {"(max-height: 0 extra)", 0},
    {"(aspect-ratio: 0/1 extra)", 0},
    {"(orientation: portrait extra)", 0},
    {"(resolution: 1px)", 0},
    {"(hover: hover extra)", 0},
    {"(min-resolution: 1dppx)", 1},
    {"(resolution: 96dpi)", 1},
    {"(min-resolution: 37dpcm)", 1},
    {"(resolution: 1x)", 1}};

static int media_run(const struct media_case *test, int *matches);

/*
 * Checks real native media parsing and evaluation for a zero-size viewport.
 */
int
main(
	void)
{
	size_t index;
	unsigned failures;
	int matches;
	int error;
	int printed;

	/* Every case is separately parsed with the unchanged CSS tokenizer. */
	failures = 0;
	for (index = 0; index < sizeof(media_cases) / sizeof(media_cases[0]); index++) {
		error = media_run(&media_cases[index], &matches);
		if (error != 0)
			return 2;

		/* Counts failed observations and preserves each exact query in the refusal report. */
		if (matches != media_cases[index].expected) {
			failures++;
			printed = fprintf(stderr, "FAIL %s\n", media_cases[index].query);
			if (printed < 0)
				return 2;
		}
	}

	/* Publish bounded observations after every query allocation was released. */
	printed = printf("native color media: %zu/%zu passed\n", index - failures, index);
	if (printed < 0)
		return 2;

	/* Any wrong comparison rejects native feature acceptance. */
	if (failures != 0)
		return 1;

	/* Succeeded: numeric and Boolean depth features obey the display model. */
	return 0;
}

/* Parses and evaluates one query while keeping its token arena alive. */
static int
media_run(
	const struct media_case *test,
	int *matches)
{
	struct wb_arena arena;
	struct wb_units units;
	struct css_token *tokens;
	struct css_media *media;
	size_t count;
	int error;

	/* Source conversion and tokenizer storage are independent checked allocations. */
	wb_arena_init(&arena, 256);
	wb_units_init(&units);

	/* Converts the corpus's UTF8 query to the tokenizer's owned UTF16 input. */
	error = wb_utf8_to_units((const unsigned char *)test->query, strlen(test->query), &units);
	if (error != 0) {
		wb_units_release(&units);
		wb_arena_release(&arena);
		return error;
	}

	/* Tokenize the same UTF16 syntax consumed by actual stylesheet media preludes. */
	error = css_tokenize(&arena, units.data, units.length, &tokens, &count);
	if (error != 0) {
		wb_units_release(&units);
		wb_arena_release(&arena);
		return error;
	}

	/* Tokens now refer only to checked arena-owned storage. */
	wb_units_release(&units);

	/* A stylesheet prelude excludes the tokenizer's terminal EOF token. */
	if (count != 0 && tokens[count - 1U].type == CSS_TOKEN_EOF)
		count--;

	/* Parse then evaluate while all original arena-owned query tests remain alive. */
	error = css_media_parse(&arena, tokens, count, NULL, &media);
	if (error != 0) {
		wb_arena_release(&arena);
		return error;
	}

	/* Numeric display features combine with the actual zero-size viewport. */
	*matches = css_media_matches(media, 0, 0, 0);
	wb_arena_release(&arena);

	/* Succeeded: the caller can compare this query's real result. */
	return 0;
}
