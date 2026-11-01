/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of the color scheme a view's pages are shown in
 * (browser_view_set_color_scheme, prefers-color-scheme).
 *
 *   scheme-test PAGE
 *
 * PAGE is a file whose sheet gives the body one color in the light scheme
 * and another under @media (prefers-color-scheme: dark), and whose script
 * says on the console what matchMedia answers for both schemes and the
 * body's computed color (scheme-test.sh writes it).
 *
 * What it checks:
 *  1. the media queries of prefers-color-scheme in either scheme: light
 *     and dark hold in their own scheme only, the feature alone holds in
 *     both, another value in neither, and the query joins not, and and
 *     the lists as the other features do;
 *  2. a view starts light: the script sees matchMedia light, the style
 *     dump has the light color;
 *  3. a view told dark before the page loads: the script sees dark and
 *     the body has the dark color;
 *  4. a view told dark after the page loaded: the style dump changes to
 *     the dark color, and back to the light one when told light again.
 * Last line: test-scheme: PASS (<n> checks), or the failures.
 */

#include "check.h"

#include "css/internal.h"

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The console lines a view of the test keeps, and the room for each. */
#define TEST_LINES_MAX		8
#define TEST_LINE_SIZE		256

/* The body's colors in the style dump in the light and the dark scheme. */
#define TEST_LIGHT_COLOR	"color=#ff102030"
#define TEST_DARK_COLOR		"color=#ffe0e0f0"

/* What the page's script says in the light and the dark scheme. */
#define TEST_LIGHT_LINE		"dark=false light=true color=rgb(16, 32, 48)"
#define TEST_DARK_LINE		"dark=true light=false color=rgb(224, 224, 240)"

/* One media query and whether it holds in the light and in the dark scheme. */
struct test_media_case {
	const char *query;
	int light;
	int dark;
};

/* What a test view's console said: its lines in order. */
struct test_console {
	char lines[TEST_LINES_MAX][TEST_LINE_SIZE];
	size_t count;
};

/* The queries of part 1, constant for the life of the process. */
static const struct test_media_case test_media_cases[] = {
    {"(prefers-color-scheme: light)", 1, 0},
    {"(prefers-color-scheme: dark)", 0, 1},
    {"(prefers-color-scheme:dark)", 0, 1},
    {"(PREFERS-COLOR-SCHEME: DARK)", 0, 1},
    {"(prefers-color-scheme)", 1, 1},
    {"(prefers-color-scheme: no-preference)", 0, 0},
    {"(prefers-color-scheme: blue)", 0, 0},
    {"(prefers-color-scheme: 1)", 0, 0},
    {"(prefers-color-scheme: dark extra)", 0, 0},
    {"not all and (prefers-color-scheme: dark)", 1, 0},
    {"screen and (prefers-color-scheme: dark)", 0, 1},
    {"print and (prefers-color-scheme: dark)", 0, 0},
    {"(prefers-color-scheme: dark) and (min-width: 100px)", 0, 1},
    {"(prefers-color-scheme: dark) and (min-width: 100000px)", 0, 0},
    {"(prefers-color-scheme: dark), (prefers-color-scheme: light)", 1, 1},
    {"(prefers-reduced-motion: no-preference) and (prefers-color-scheme: light)", 1, 0}};

/* The outermost frame the views' heaps scan to, set once by main. */
static const void *test_stack_base;

/* The page of parts 2 to 4, set once by main. */
static const char *test_page_path;

static void test_media(void);
static void test_view_light(void);
static void test_view_dark_first(void);
static void test_view_switch(void);
static int test_media_holds(const char *query, int dark, int *holds);
static int test_view(struct test_console *console, struct browser_view **view);
static int test_style_has(struct browser_view *view, const char *wanted);
static void test_console_line(void *context, struct browser_view *view, int level, const char *text, size_t length);

/*
 * Runs the parts in order and prints the summary.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The page. */
	if (argc != 2) {
		fprintf(stderr, "usage: scheme-test PAGE\n");
		return 2;
	}
	test_page_path = argv[1];
	test_stack_base = __builtin_frame_address(0);

	/* The parts. */
	test_media();
	test_view_light();
	test_view_dark_first();
	test_view_switch();

	/* Succeeded or not: the summary line. */
	status = check_finish("scheme");
	return status;
}

/* The queries of part 1 in both schemes. */
static void
test_media(void)
{
	size_t index;
	int holds;
	int error;

	/* Each query in the light scheme, then in the dark one. */
	for (index = 0; index < sizeof(test_media_cases) / sizeof(test_media_cases[0]); index++) {
		error = test_media_holds(test_media_cases[index].query, 0, &holds);
		CHECK_EQ(error, 0);
		if (holds != test_media_cases[index].light)
			fprintf(stderr, "light: %s\n", test_media_cases[index].query);
		CHECK_EQ(holds, test_media_cases[index].light);
		error = test_media_holds(test_media_cases[index].query, 1, &holds);
		CHECK_EQ(error, 0);
		if (holds != test_media_cases[index].dark)
			fprintf(stderr, "dark: %s\n", test_media_cases[index].query);
		CHECK_EQ(holds, test_media_cases[index].dark);
	}
}

/* A view that is told nothing shows the page light. */
static void
test_view_light(void)
{
	struct test_console console;
	struct browser_view *view;
	int error;
	int has;

	/* The page, settled. */
	error = test_view(&console, &view);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = browser_view_load(view, test_page_path);
	CHECK_EQ(error, 0);
	error = browser_view_settle(view, 0, 0);
	CHECK_EQ(error, 0);

	/* The script and the style saw the light scheme. */
	CHECK_EQ(console.count, 1);
	CHECK(strcmp(console.lines[0], TEST_LIGHT_LINE) == 0);
	has = test_style_has(view, TEST_LIGHT_COLOR);
	CHECK_EQ(has, 1);
	browser_view_destroy(view);
}

/* A view told dark before the page loads shows it dark from the start. */
static void
test_view_dark_first(void)
{
	struct test_console console;
	struct browser_view *view;
	int error;
	int has;

	/* The view in the dark scheme, then the page. */
	error = test_view(&console, &view);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	browser_view_set_color_scheme(view, BROWSER_COLOR_SCHEME_DARK);
	error = browser_view_load(view, test_page_path);
	CHECK_EQ(error, 0);
	error = browser_view_settle(view, 0, 0);
	CHECK_EQ(error, 0);

	/* The script and the style saw the dark scheme. */
	CHECK_EQ(console.count, 1);
	if (console.count >= 1U)
		fprintf(stderr, "dark first: %s\n", console.lines[0]);
	CHECK(strcmp(console.lines[0], TEST_DARK_LINE) == 0);
	has = test_style_has(view, TEST_DARK_COLOR);
	CHECK_EQ(has, 1);
	browser_view_destroy(view);
}

/* A page shown is styled again when its view's scheme changes, and back. */
static void
test_view_switch(void)
{
	struct test_console console;
	struct browser_view *view;
	int error;
	int has;

	/* The page, light. */
	error = test_view(&console, &view);
	CHECK_EQ(error, 0);
	if (error != 0)
		return;
	error = browser_view_load(view, test_page_path);
	CHECK_EQ(error, 0);
	error = browser_view_settle(view, 0, 0);
	CHECK_EQ(error, 0);
	has = test_style_has(view, TEST_LIGHT_COLOR);
	CHECK_EQ(has, 1);

	/* Dark: the body's color follows (told twice, the second changes nothing). */
	browser_view_set_color_scheme(view, BROWSER_COLOR_SCHEME_DARK);
	browser_view_set_color_scheme(view, BROWSER_COLOR_SCHEME_DARK);
	has = test_style_has(view, TEST_DARK_COLOR);
	CHECK_EQ(has, 1);
	has = test_style_has(view, TEST_LIGHT_COLOR);
	CHECK_EQ(has, 0);

	/* Light again. */
	browser_view_set_color_scheme(view, BROWSER_COLOR_SCHEME_LIGHT);
	has = test_style_has(view, TEST_LIGHT_COLOR);
	CHECK_EQ(has, 1);
	browser_view_destroy(view);
}

/* Parses one media query and tests it in a scheme (dark nonzero); 0 with *holds set, or an error. */
static int
test_media_holds(
	const char *query,
	int dark,
	int *holds)
{
	struct wb_arena arena;
	struct wb_units units;
	struct css_token *tokens;
	struct css_media *media;
	size_t count;
	int error;

	/* The query as the tokenizer's UTF-16. */
	*holds = -1;
	wb_arena_init(&arena, 256);
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)query, strlen(query), &units);
	if (error != 0) {
		wb_units_release(&units);
		wb_arena_release(&arena);
		return error;
	}

	/* Its tokens, without the end. */
	error = css_tokenize(&arena, units.data, units.length, &tokens, &count);
	wb_units_release(&units);
	if (error != 0) {
		wb_arena_release(&arena);
		return error;
	}
	if (count != 0 && tokens[count - 1U].type == CSS_TOKEN_EOF)
		count--;

	/* The list, tested at a 320 by 240 viewport. */
	error = css_media_parse(&arena, tokens, count, NULL, &media);
	if (error != 0) {
		wb_arena_release(&arena);
		return error;
	}
	*holds = css_media_matches(media, 320.0f, 240.0f, dark);
	wb_arena_release(&arena);

	/* Succeeded: *holds is the answer. */
	return 0;
}

/* Makes a view without a window, every page read at once, its console kept in console. */
static int
test_view(
	struct test_console *console,
	struct browser_view **view)
{
	static struct browser_callbacks callbacks;
	struct browser_view_options options;
	int error;

	/* The console, empty. */
	memset(console, 0, sizeof(*console));
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.console = test_console_line;
	callbacks.context = console;

	/* The view. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.callbacks = &callbacks;
	options.stack_base = test_stack_base;
	options.width = 320;
	options.height = 240;
	options.fetch = BROWSER_FETCH_AT_ONCE;
	error = browser_view_create(&options, view);
	if (error != 0)
		return error;

	/* Succeeded: the view. */
	return 0;
}

/* Tells whether the view's style dump has a text (1), not (0), or could not be made (-1). */
static int
test_style_has(
	struct browser_view *view,
	const char *wanted)
{
	char *text;
	size_t length;
	int error;
	int has;

	/* The dump, as the page is styled now. */
	error = browser_view_dump(view, BROWSER_DUMP_STYLE, &text, &length);
	if (error != 0)
		return -1;

	/* The text in it. */
	has = 0;
	if (strstr(text, wanted) != NULL)
		has = 1;
	browser_free(text);

	/* Succeeded: whether it has the text. */
	return has;
}

/* Keeps one line of a view's console. */
static void
test_console_line(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct test_console *console;

	(void)view;
	(void)level;

	/* The line, cut to its room. */
	console = context;
	if (console->count == TEST_LINES_MAX)
		return;
	snprintf(console->lines[console->count], sizeof(console->lines[0]), "%.*s", (int)length, text);
	console->count++;
}
