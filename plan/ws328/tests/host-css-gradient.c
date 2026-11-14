/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the parsing of the CSS gradient functions (linear, radial and conic, repeating and not, with
 * transition hints) against the grammar of CSS Images 3 and 4: the fields of the parsed gradient, the
 * declarations the grammar rejects, and the computed value getComputedStyle writes, which must parse back
 * to the same text.  The expected texts are written by hand from the grammar and the serialization rule
 * of CSS Images 4 section 8, not taken from the engine's output.
 */

#include "bind/internal.h"
#include "css/internal.h"
#include "page/page.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* One declaration: the property, the value, and the computed background-image it must give ("none" when the value is invalid). */
struct gradient_case {
	const char *property;
	const char *value;
	const char *expected;
};

/* The number of observations and the number that failed. */
static unsigned checks;
static unsigned failures;

/* The colors the cases use, as getComputedStyle writes them. */
#define RED "rgb(255, 0, 0)"
#define BLUE "rgb(0, 0, 255)"
#define YELLOW "rgb(255, 255, 0)"
#define BLACK "rgb(0, 0, 0)"
#define WHITE "rgb(255, 255, 255)"

/*
 * The declarations and their computed values.
 *
 * Each expected text omits what the grammar's defaults stand for (to bottom, ellipse farthest-corner at
 * center, from 0deg at center, the first stop's 0% and the last stop's 100%), writes positions as
 * lengths and percentages, and writes the colors as rgb().  The font size of the element is 10px.
 */
static const struct gradient_case gradient_cases[] = {
	/* linear: CSS Images 3 section 3.1 and the serialization example of Images 4 section 8. */
	{ "background-image", "linear-gradient(red, blue)", "linear-gradient(" RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(to bottom, red 0%, yellow,black 100px)", "linear-gradient(" RED ", " YELLOW ", " BLACK " 100px)" },
	{ "background-image", "Linear-Gradient( to bottom, red 0%,yellow,black 100px)", "linear-gradient(" RED ", " YELLOW ", " BLACK " 100px)" },
	{ "background-image", "linear-gradient(to top right, red, blue)", "linear-gradient(to right top, " RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(to left, red, blue)", "linear-gradient(to left, " RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(90deg, red, blue)", "linear-gradient(90deg, " RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(0.25turn, red, blue)", "linear-gradient(90deg, " RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(0, red, blue)", "linear-gradient(0deg, " RED ", " BLUE ")" },
	{ "background-image", "linear-gradient(red 10% 20%, blue)", "linear-gradient(" RED " 10%, " RED " 20%, " BLUE ")" },
	{ "background-image", "linear-gradient(red)", "linear-gradient(" RED ")" },
	{ "background-image", "repeating-linear-gradient(red, blue 20px)", "repeating-linear-gradient(" RED ", " BLUE " 20px)" },
	{ "background-image", "linear-gradient(red 1em, blue 2em)", "linear-gradient(" RED " 10px, " BLUE " 20px)" },
	{ "background-image", "linear-gradient(to top top, red, blue)", "none" },
	{ "background-image", "linear-gradient(to, red, blue)", "none" },
	{ "background-image", "linear-gradient(red 10% 20% 30%, blue)", "none" },
	{ "background-image", "linear-gradient(red, , blue)", "none" },
	{ "background-image", "linear-gradient()", "none" },
	{ "background-image", "linear-gradient(90deg)", "none" },
	{ "background-image", "linear-gradient(red 10deg, blue)", "none" },

	/* transition hints: CSS Images 4 section 3.5.1. */
	{ "background-image", "linear-gradient(red, 30%, blue)", "linear-gradient(" RED ", 30%, " BLUE ")" },
	{ "background-image", "linear-gradient(red, 20px, blue 100px)", "linear-gradient(" RED ", 20px, " BLUE " 100px)" },
	{ "background-image", "linear-gradient(red 10%, 0, blue)", "linear-gradient(" RED " 10%, 0px, " BLUE ")" },
	{ "background-image", "linear-gradient(red, 1em, yellow, 50%, blue)", "linear-gradient(" RED ", 10px, " YELLOW ", 50%, " BLUE ")" },
	{ "background-image", "radial-gradient(red, 30%, blue)", "radial-gradient(" RED ", 30%, " BLUE ")" },
	{ "background-image", "linear-gradient(30%, red, blue)", "none" },
	{ "background-image", "linear-gradient(red, blue, 30%)", "none" },
	{ "background-image", "linear-gradient(red, 30%, 40%, blue)", "none" },
	{ "background-image", "linear-gradient(red 10%, 20%)", "none" },
	{ "background-image", "linear-gradient(red, 30% 40%, blue)", "none" },
	{ "background-image", "linear-gradient(red, 30deg, blue)", "none" },

	/* radial: shape, size and position. */
	{ "background-image", "radial-gradient(red, blue)", "radial-gradient(" RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(ellipse farthest-corner at center, red 0%, blue 100%)", "radial-gradient(" RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at 50% 50%, red, blue)", "radial-gradient(" RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle, red, blue)", "radial-gradient(circle, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle farthest-corner, red, blue)", "radial-gradient(circle, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle closest-side at 10px 20%, red, blue)", "radial-gradient(circle closest-side at 10px 20%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(closest-side circle at 10px 20%, red, blue)", "radial-gradient(circle closest-side at 10px 20%, " RED ", " BLUE ")" },
	{ "background-image", "RADIAL-GRADIENT(Circle CLOSEST-SIDE, red, blue)", "radial-gradient(circle closest-side, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(farthest-side, red, blue)", "radial-gradient(farthest-side, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(ellipse closest-corner, red, blue)", "radial-gradient(closest-corner, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(closest-side farthest-corner, red, blue)", "radial-gradient(closest-side farthest-corner, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(10px, red, blue)", "radial-gradient(10px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle 10px, red, blue)", "radial-gradient(10px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(5em circle at top left, red, blue)", "radial-gradient(50px at 0px 0px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle 0, red, blue)", "radial-gradient(0px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(circle 50%, red, blue)", "radial-gradient(circle 50%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(10px 20%, red, blue)", "radial-gradient(10px 20%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(ellipse 10px 20px at 5px 6px, red, blue)", "radial-gradient(10px 20px at 5px 6px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(20px 30px at 20px 30px, red, yellow, blue)", "radial-gradient(20px 30px at 20px 30px, " RED ", " YELLOW ", " BLUE ")" },
	{ "background-image", "radial-gradient(at top left, red, blue)", "radial-gradient(at 0px 0px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at left top, red, blue)", "radial-gradient(at 0px 0px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at top, red, blue)", "radial-gradient(at 50% 0px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at center left, red, blue)", "radial-gradient(at 0px 50%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at 25%, red, blue)", "radial-gradient(at 25% 50%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at 10px bottom, red, blue)", "radial-gradient(at 10px 100%, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at right 10px bottom 20px, red, blue)", "radial-gradient(at right 10px bottom 20px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(at top 5px right 6px, red, blue)", "radial-gradient(at right 6px top 5px, " RED ", " BLUE ")" },
	{ "background-image", "radial-gradient(2em at 1em 10%, red 1em, blue)", "radial-gradient(20px at 10px 10%, " RED " 10px, " BLUE ")" },
	{ "background-image", "repeating-radial-gradient(circle, red, blue 10px)", "repeating-radial-gradient(circle, " RED ", " BLUE " 10px)" },
	{ "background-image", "radial-gradient(circle -10px, red, blue)", "none" },
	{ "background-image", "radial-gradient(-10%, red, blue)", "none" },
	{ "background-image", "radial-gradient(10px -20px, red, blue)", "none" },
	{ "background-image", "radial-gradient(circle 10px 20px, red, blue)", "none" },
	{ "background-image", "radial-gradient(ellipse 10px, red, blue)", "none" },
	{ "background-image", "radial-gradient(ellipse circle, red, blue)", "none" },
	{ "background-image", "radial-gradient(circle ellipse, red, blue)", "none" },
	{ "background-image", "radial-gradient(circle closest-side farthest-side, red, blue)", "none" },
	{ "background-image", "radial-gradient(closest-side closest-side closest-side, red, blue)", "none" },
	{ "background-image", "radial-gradient(10px closest-side, red, blue)", "none" },
	{ "background-image", "radial-gradient(at, red, blue)", "none" },
	{ "background-image", "radial-gradient(at top top, red, blue)", "none" },
	{ "background-image", "radial-gradient(at left right, red, blue)", "none" },
	{ "background-image", "radial-gradient(at 10px left, red, blue)", "none" },
	{ "background-image", "radial-gradient(at left 10px top, red, blue)", "none" },
	{ "background-image", "radial-gradient(at center 10px bottom 20px, red, blue)", "none" },
	{ "background-image", "radial-gradient(circle at 10px 20px 30px, red, blue)", "none" },
	{ "background-image", "radial-gradient(circle)", "none" },
	{ "background-image", "radial-gradient()", "none" },
	{ "background-image", "radial-gradient(in srgb, red, blue)", "none" },
	{ "background-image", "radial-gradient(red 10deg, blue)", "none" },

	/* conic: CSS Images 4 sections 3.3 and 3.4. */
	{ "background-image", "conic-gradient(red, blue)", "conic-gradient(" RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from 0deg at center, red 0deg, blue 360deg)", "conic-gradient(" RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from 0, red, blue)", "conic-gradient(" RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from 90deg, red, blue)", "conic-gradient(from 90deg, " RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from 100grad, red, blue)", "conic-gradient(from 90deg, " RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from -45deg, red, blue)", "conic-gradient(from -45deg, " RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(from 0.25turn at 10px 20%, red, blue)", "conic-gradient(from 90deg at 10px 20%, " RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(at 25% 30%, white, black 60%)", "conic-gradient(at 25% 30%, " WHITE ", " BLACK " 60%)" },
	{ "background-image", "conic-gradient(at top left, red, blue)", "conic-gradient(at 0px 0px, " RED ", " BLUE ")" },
	{ "background-image", "conic-gradient(red 0deg 90deg, blue 90deg 360deg)", "conic-gradient(" RED ", " RED " 90deg, " BLUE " 90deg, " BLUE ")" },
	{ "background-image", "conic-gradient(red 0.25turn, blue)", "conic-gradient(" RED " 90deg, " BLUE ")" },
	{ "background-image", "conic-gradient(red 50%, blue)", "conic-gradient(" RED " 50%, " BLUE ")" },
	{ "background-image", "conic-gradient(red 0, blue 0)", "conic-gradient(" RED ", " BLUE " 0deg)" },
	{ "background-image", "conic-gradient(red, 30%, blue)", "conic-gradient(" RED ", 30%, " BLUE ")" },
	{ "background-image", "conic-gradient(red, 45deg, blue)", "conic-gradient(" RED ", 45deg, " BLUE ")" },
	{ "background-image", "conic-gradient(red, 0, blue)", "conic-gradient(" RED ", 0deg, " BLUE ")" },
	{ "background-image", "repeating-conic-gradient(red, blue 20deg)", "repeating-conic-gradient(" RED ", " BLUE " 20deg)" },
	{ "background-image", "conic-gradient(red -50%, yellow 150%)", "conic-gradient(" RED " -50%, " YELLOW " 150%)" },
	{ "background-image", "conic-gradient(red 10px, blue)", "none" },
	{ "background-image", "conic-gradient(red, 10px, blue)", "none" },
	{ "background-image", "conic-gradient(from 10px, red, blue)", "none" },
	{ "background-image", "conic-gradient(from, red, blue)", "none" },
	{ "background-image", "conic-gradient(at, red, blue)", "none" },
	{ "background-image", "conic-gradient(at 10px 10px from 10deg, red, blue)", "none" },
	{ "background-image", "conic-gradient(circle, red, blue)", "none" },
	{ "background-image", "conic-gradient(to left, red, blue)", "none" },
	{ "background-image", "conic-gradient(90deg, red, blue)", "none" },
	{ "background-image", "conic-gradient(red, blue, 90deg)", "none" },

	/* a page zoom scales the pixels of the geometry and the stops, and the computed value reports them without it. */
	{ "zoom:2;background-image", "linear-gradient(red 10px, 1em, blue 3em)", "linear-gradient(" RED " 10px, 10px, " BLUE " 30px)" },
	{ "zoom:2;background-image", "radial-gradient(5px at 3px 4px, red, blue)", "radial-gradient(5px at 3px 4px, " RED ", " BLUE ")" },
	{ "zoom:2;background-image", "radial-gradient(at right 6px top 1em, red, blue)", "radial-gradient(at right 6px top 10px, " RED ", " BLUE ")" },

	/* the background shorthand takes the same functions. */
	{ "background", "radial-gradient(circle at 10px 20px, red, blue) no-repeat", "radial-gradient(circle at 10px 20px, " RED ", " BLUE ")" },
	{ "background", "conic-gradient(from 45deg, red, blue) repeat-x", "conic-gradient(from 45deg, " RED ", " BLUE ")" },
	{ "background", "radial-gradient(circle -1px, red, blue)", "none" }
};

static void gradient_check(int condition, const char *name, const char *detail);
static int gradient_script(struct vm_realm *realm, const char *source, vm_value *answer);
static int gradient_computed(struct page *page, const char *property, const char *value, struct wb_buffer *text);
static void gradient_run_cases(struct page *page);
static void gradient_run_limits(struct page *page);
static int gradient_parse(const char *text, const struct css_gradient **gradient, struct wb_arena *arena);
static void gradient_run_fields(void);
static void gradient_run_paint(const struct text_font_paths *paths);

/*
 * Runs the parser on its own, then every declaration through a real page.
 */
int
main(
	void)
{
	struct page *page;
	struct text_font_paths paths;
	static const unsigned char html[] = "<!doctype html><html><body></body></html>";
	int status;

	/* The parsed fields, without a page. */
	gradient_run_fields();

	/* Checked-in fonts supply the ordinary page environment. */
	paths.sans = "userland/desktop/fonts/Mahora-Regular.ttf";
	paths.mono = "userland/desktop/fonts/JetBrainsMono-Regular.ttf";
	paths.fallback = "userland/desktop/fonts/DroidSansFallbackFull.ttf";

	/* A real page to compute styles in. */
	status = page_create(&page, __builtin_frame_address(0));
	if (status != 0)
		return 2;
	page_set_fonts(page, &paths);
	page_set_viewport(page, 800, 600);
	status = page_load_html(page, html, sizeof(html) - 1U);
	if (status != 0) {
		page_destroy(page);
		return 2;
	}

	/* Every declaration, and the bound on the stop list. */
	gradient_run_cases(page);
	gradient_run_limits(page);

	/* Laying out and painting a page whose gradients are of each kind. */
	gradient_run_paint(&paths);

	/* The page goes away before the summary. */
	vm_heap_set_stack_base(page->heap, __builtin_frame_address(0));
	page_destroy(page);
	printf("css gradient: %u/%u passed\n", checks - failures, checks);
	if (failures != 0)
		return 1;
	return 0;
}

/* Records one observation. */
static void
gradient_check(
	int condition,
	const char *name,
	const char *detail)
{
	/* A failure is kept and reported with what was seen. */
	checks++;
	if (!condition) {
		failures++;
		fprintf(stderr, "FAIL %s: %s\n", name, detail);
	}
}

/* Runs a script and gives its answer. */
static int
gradient_script(
	struct vm_realm *realm,
	const char *source,
	vm_value *answer)
{
	struct wb_units units;
	struct js_syntax_error syntax;
	int status;

	/* The source as the interpreter takes it. */
	wb_units_init(&units);
	status = wb_utf8_to_units((const unsigned char *)source, strlen(source), &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* The run. */
	status = js_run_script(realm, units.data, units.length, 0, answer, &syntax);
	wb_units_release(&units);
	return status;
}

/*
 * Sets a declaration on a fresh element (font size 10px) and reads its computed
 * background-image into text.
 */
static int
gradient_computed(
	struct page *page,
	const char *property,
	const char *value,
	struct wb_buffer *text)
{
	struct wb_buffer source;
	vm_value answer;
	int status;

	/* The script: a new element with the declaration, and its computed value. */
	wb_buffer_init(&source);
	status = wb_buffer_printf(&source,
	    "(function(){var e=document.createElement('div');document.body.appendChild(e);"
	    "e.style.cssText='font-size:10px;%s:%s';return getComputedStyle(e).backgroundImage;})()",
	    property, value);
	if (status != 0) {
		wb_buffer_release(&source);
		return status;
	}
	status = gradient_script(page->realm, wb_buffer_string(&source), &answer);
	wb_buffer_release(&source);
	if (status != 0)
		return status;

	/* The answer must be a string. */
	if (!vm_value_is_string(answer))
		return EINVAL;
	wb_buffer_clear(text);
	status = vm_string_to_utf8((struct vm_string *)vm_value_as_cell(answer), text);
	return status;
}

/* Runs each declaration: its computed value, and that value parsed again gives itself. */
static void
gradient_run_cases(
	struct page *page)
{
	struct wb_buffer text;
	struct wb_buffer again;
	size_t index;
	size_t cases;
	int status;
	int same;

	/* Each case. */
	wb_buffer_init(&text);
	wb_buffer_init(&again);
	cases = sizeof(gradient_cases) / sizeof(gradient_cases[0]);
	for (index = 0; index < cases; index++) {
		status = gradient_computed(page, gradient_cases[index].property, gradient_cases[index].value, &text);
		if (status != 0) {
			gradient_check(0, gradient_cases[index].value, "the script did not run");
			continue;
		}
		same = strcmp(wb_buffer_string(&text), gradient_cases[index].expected) == 0;
		gradient_check(same, gradient_cases[index].value, wb_buffer_string(&text));

		/* The serialization is a value that parses to the same gradient and serializes to itself. */
		if (strcmp(gradient_cases[index].expected, "none") == 0)
			continue;
		status = gradient_computed(page, "background-image", gradient_cases[index].expected, &again);
		if (status != 0) {
			gradient_check(0, gradient_cases[index].expected, "the round trip did not run");
			continue;
		}
		same = strcmp(wb_buffer_string(&again), gradient_cases[index].expected) == 0;
		gradient_check(same, gradient_cases[index].expected, wb_buffer_string(&again));
	}
	wb_buffer_release(&again);
	wb_buffer_release(&text);
}

/* Tells whether a stop list of the given pieces is a valid gradient, through the page. */
static void
gradient_run_limits(
	struct page *page)
{
	struct wb_buffer value;
	struct wb_buffer text;
	unsigned total;
	unsigned index;
	int status;
	int valid;
	char name[64];

	/* A list of N red stops: the list holds 32 entries, so 32 are valid and 33 are not. */
	wb_buffer_init(&value);
	wb_buffer_init(&text);
	for (total = 31U; total <= 33U; total++) {
		wb_buffer_clear(&value);
		wb_buffer_append_string(&value, "linear-gradient(red");
		for (index = 1; index < total; index++)
			wb_buffer_append_string(&value, ",red");
		wb_buffer_append_string(&value, ")");
		status = gradient_computed(page, "background-image", wb_buffer_string(&value), &text);
		valid = status == 0 && strcmp(wb_buffer_string(&text), "none") != 0;
		snprintf(name, sizeof(name), "%u stops", total);
		gradient_check(valid == (total <= 32U), name, wb_buffer_string(&text));
	}

	/* Hints count as entries: 16 stops and 15 hints are 31 entries, one more stop makes 32 and a hint then a stop 34. */
	for (total = 0; total < 3U; total++) {
		wb_buffer_clear(&value);
		wb_buffer_append_string(&value, "linear-gradient(red");
		for (index = 1; index < 16U + total; index++)
			wb_buffer_append_string(&value, ",50%,red");
		wb_buffer_append_string(&value, ")");
		status = gradient_computed(page, "background-image", wb_buffer_string(&value), &text);
		valid = status == 0 && strcmp(wb_buffer_string(&text), "none") != 0;
		snprintf(name, sizeof(name), "%u stops and hints", 16U + total + 15U + total);
		gradient_check(valid == (total == 0), name, wb_buffer_string(&text));
	}
	wb_buffer_release(&text);
	wb_buffer_release(&value);
}

/* Tokenizes a gradient function and parses it into the arena; the parse's own answer. */
static int
gradient_parse(
	const char *text,
	const struct css_gradient **gradient,
	struct wb_arena *arena)
{
	struct css_parse parse;
	struct wb_units units;
	struct css_token *tokens;
	size_t count;
	int error;

	/* The tokens, without the end of the input. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)text, strlen(text), &units);
	if (error == 0)
		error = css_tokenize(arena, units.data, units.length, &tokens, &count);
	wb_units_release(&units);
	if (error != 0)
		return error;
	if (count != 0 && tokens[count - 1U].type == CSS_TOKEN_EOF)
		count--;

	/* The parse. */
	parse.heap = NULL;
	parse.arena = arena;
	error = css_gradient_parse(&parse, tokens, count, gradient);
	return error;
}

/* Checks the fields a parse leaves, before the cascade resolves anything. */
static void
gradient_run_fields(void)
{
	struct wb_arena arena;
	struct css_gradient plain;
	const struct css_gradient *gradient;
	int error;

	/* The defaults of a radial gradient: an ellipse reaching the farthest corner, centered. */
	wb_arena_init(&arena, 4096);
	error = gradient_parse("radial-gradient(red, blue)", &gradient, &arena);
	gradient_check(error == 0, "fields radial default parse", "error");
	if (error == 0) {
		gradient_check(gradient->kind == CSS_GRADIENT_RADIAL, "radial kind", "kind");
		gradient_check(gradient->circle == 0, "radial default is an ellipse", "circle");
		gradient_check(gradient->extent[0] == CSS_GRADIENT_FARTHEST_CORNER && gradient->extent[1] == CSS_GRADIENT_FARTHEST_CORNER,
		    "radial default size", "extent");
		gradient_check(gradient->at[0].percent == 50.0f && gradient->at[1].percent == 50.0f &&
		    gradient->at[0].pixels == 0.0f && gradient->at[1].pixels == 0.0f, "radial default center", "at");
		gradient_check(gradient->repeating == 0 && gradient->count == 2U, "radial default stops", "count");
		gradient_check(gradient->stops[0].color == 0xffff0000U && gradient->stops[1].color == 0xff0000ffU, "radial stop colors", "color");
		gradient_check(gradient->stops[0].unit == CSS_GRADIENT_AT_AUTO && gradient->stops[0].hint == 0, "radial stop without position", "unit");
	}

	/* A circle sized by a keyword, with a center in pixels and a percentage. */
	error = gradient_parse("repeating-radial-gradient(circle closest-side at 10px 20%, red, blue)", &gradient, &arena);
	gradient_check(error == 0, "fields circle parse", "error");
	if (error == 0) {
		gradient_check(gradient->repeating == 1U && gradient->circle == 1U, "repeating circle", "flags");
		gradient_check(gradient->extent[0] == CSS_GRADIENT_CLOSEST_SIDE && gradient->extent[1] == CSS_GRADIENT_EXTENT_NONE, "circle keyword", "extent");
		gradient_check(gradient->at[0].pixels == 10.0f && gradient->at[0].percent == 0.0f && gradient->at[1].percent == 20.0f &&
		    gradient->at[1].pixels == 0.0f, "circle center", "at");
	}

	/* An ellipse sized in ems and percentages, the ems kept for the cascade. */
	error = gradient_parse("radial-gradient(2em 30% at 1em 3px, red 1em, blue)", &gradient, &arena);
	gradient_check(error == 0, "fields ellipse parse", "error");
	if (error == 0) {
		gradient_check(gradient->circle == 0 && gradient->extent[0] == CSS_GRADIENT_EXTENT_NONE, "explicit ellipse", "shape");
		gradient_check(gradient->radius[0].ems == 2.0f && gradient->radius[1].percent == 30.0f, "ellipse radii", "radius");
		gradient_check(gradient->at[0].ems == 1.0f && gradient->at[1].pixels == 3.0f, "ellipse center", "at");
		gradient_check(gradient->stops[0].unit == CSS_GRADIENT_AT_EM && gradient->stops[0].position == 1.0f, "stop in ems", "unit");
	}

	/* A conic gradient's rotation, center, and stops in angles and percentages. */
	error = gradient_parse("conic-gradient(from 0.25turn at left 5px top, red 10grad, blue 25% 50%)", &gradient, &arena);
	gradient_check(error != 0, "fields conic with left 5px top is three words", "error");
	error = gradient_parse("conic-gradient(from 0.25turn at left 5px top 6px, red 10grad, blue 25% 50%)", &gradient, &arena);
	gradient_check(error == 0, "fields conic parse", "error");
	if (error == 0) {
		gradient_check(gradient->kind == CSS_GRADIENT_CONIC && gradient->angle == 90.0f, "conic rotation", "angle");
		gradient_check(gradient->at[0].pixels == 5.0f && gradient->at[0].percent == 0.0f && gradient->at[1].pixels == 6.0f, "conic center", "at");
		gradient_check(gradient->count == 3U, "conic stops, a pair is two entries", "count");
		gradient_check(gradient->stops[0].unit == CSS_GRADIENT_AT_DEGREES && gradient->stops[0].position == 9.0f, "conic stop in grad", "position");
		gradient_check(gradient->stops[1].unit == CSS_GRADIENT_AT_PERCENT && gradient->stops[1].position == 25.0f &&
		    gradient->stops[2].position == 50.0f && gradient->stops[2].color == gradient->stops[1].color, "conic stop with two positions", "position");
	}

	/* A hint is an entry of its own, between two stops; the consumers that ignore hints see the stops only. */
	error = gradient_parse("linear-gradient(red, 30%, yellow 60%, 80%, blue)", &gradient, &arena);
	gradient_check(error == 0, "fields hint parse", "error");
	if (error == 0) {
		gradient_check(gradient->count == 5U, "hint entries", "count");
		gradient_check(gradient->stops[1].hint == 1 && gradient->stops[1].unit == CSS_GRADIENT_AT_PERCENT &&
		    gradient->stops[1].position == 30.0f, "first hint", "hint");
		gradient_check(gradient->stops[3].hint == 1 && gradient->stops[3].position == 80.0f, "second hint", "hint");
		gradient_check(gradient->stops[0].hint == 0 && gradient->stops[2].hint == 0 && gradient->stops[4].hint == 0, "stops are not hints", "hint");
		gradient_check(css_gradient_color_stops(gradient, &plain) == 3U && plain.count == 3U && plain.stops[1].color == 0xffffff00U &&
		    plain.stops[2].color == 0xff0000ffU, "color stops without hints", "stops");
	}

	/* The defaults of a linear gradient and of a conic one. */
	error = gradient_parse("linear-gradient(red, blue)", &gradient, &arena);
	gradient_check(error == 0 && gradient->kind == CSS_GRADIENT_LINEAR && gradient->angle == 180.0f && gradient->corner == 0, "linear default", "angle");
	error = gradient_parse("conic-gradient(red, blue)", &gradient, &arena);
	gradient_check(error == 0 && gradient->kind == CSS_GRADIENT_CONIC && gradient->angle == 0.0f &&
	    gradient->at[0].percent == 50.0f && gradient->at[1].percent == 50.0f, "conic default", "angle");
	wb_arena_release(&arena);
}

/*
 * Lays out and paints a page with a gradient of each kind as a background and as a float's shape: the linear
 * gradient (with a hint) is the one image painted, the radial and conic ones paint nothing and break nothing.
 */
static void
gradient_run_paint(
	const struct text_font_paths *paths)
{
	struct page *page;
	const struct paint_item *item;
	size_t index;
	unsigned images;
	int error;

	/* The page of the fixture. */
	error = page_create(&page, __builtin_frame_address(0));
	if (error != 0) {
		gradient_check(0, "paint page", "page_create");
		return;
	}
	page_set_fonts(page, paths);
	error = page_load_file(page, "plan/ws328/tests/pages/css-gradient.html");
	if (error == 0)
		error = page_open_fonts(page, paths);
	if (error == 0)
		error = page_layout(page, 800, 600);
	if (error == 0)
		error = page_paint(page);
	gradient_check(error == 0, "paint page lays out and paints", "error");

	/* The images painted. */
	images = 0;
	if (error == 0) {
		for (index = 0; index < page->paint.items.count; index++) {
			item = wb_vector_at(&page->paint.items, index);
			if (item->kind == PAINT_IMAGE)
				images++;
		}
		gradient_check(images == 1U, "only the linear gradient is an image", "images");
	}
	vm_heap_set_stack_base(page->heap, __builtin_frame_address(0));
	page_destroy(page);
}
