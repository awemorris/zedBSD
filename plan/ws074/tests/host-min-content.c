/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the min-content widths a layout keeps in its boxes
 * (BUG-397: a page of deeply nested shrink-to-fit boxes measured each
 * box's content again for every ancestor, which grew exponentially with
 * the depth, and a store page took seconds a layout).
 *
 *   host-min-content
 *
 * Run from the repository's root (the fonts are the checked-in ones).
 * 1. Pages of 40 nested inline-blocks, of 12 nested flex rows and of 40
 *    nested floats are laid out within a second each.
 * 2. A page laid out, changed by a script (a style, a text, a child, the
 *    viewport's size) and laid out again after each change has the same
 *    layout as a page that got the same changes before its first layout:
 *    nothing a layout kept outlives it.
 * Prints one line per failed check and a summary.
 */

#include "bind/internal.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How deep the nested boxes of the timed pages are (flex rows less: a stretched item is laid out again across, twice a level). */
#define MIN_CONTENT_DEPTH	40
#define MIN_CONTENT_FLEX_DEPTH	12

/* The longest a timed layout may take, in seconds. */
#define MIN_CONTENT_SECONDS	1.0

/* The checks made and the ones that failed. */
static unsigned checks;
static unsigned failures;

/* The changes of the second part, each a script run on both pages. */
static const char *const min_content_changes[] = {
	"document.getElementById('a').style.fontSize='30px'",
	"document.getElementById('b').firstChild.data='a much longer text with words that break'",
	"var s=document.createElement('span');s.style.display='inline-block';s.textContent='added child box';document.getElementById('c').appendChild(s)",
	"document.getElementById('d').style.minWidth='200px'",
	"document.getElementById('e').remove()",
};

/* The page the changes are made to: shrink-to-fit boxes in flex rows, inline-blocks and floats. */
static const char min_content_page[] =
	"<!doctype html><html><body style='margin:0'>"
	"<div style='display:flex'><div id=a style='display:inline-block'>one two <span style='display:inline-block'>three four</span></div>"
	"<div id=b style='flex:0 1 auto'>five six seven</div></div>"
	"<div style='float:left'><div id=c style='display:inline-flex'><span style='display:inline-block'>eight nine</span></div></div>"
	"<div style='position:absolute;left:10px;top:200px'><div id=d style='display:inline-block'>ten eleven twelve</div>"
	"<div id=e style='display:inline-block'>thirteen</div></div>"
	"<div style='display:grid;grid-template-columns:min-content auto'><div>fourteen fifteen</div><div style='display:inline-block'>sixteen</div></div>"
	"</body></html>";

static void min_content_check(int condition, const char *what);
static int min_content_open(const char *html, size_t length, struct page **page);
static int min_content_script(struct page *page, const char *source);
static int min_content_dump(struct page *page, int width, int height, struct wb_buffer *dump);
static void min_content_timed(const char *open, const char *close, const char *text, int deep, const char *what);
static void min_content_changed(void);
static double min_content_now(void);

/*
 * Checks the speed of nested shrink-to-fit boxes and that a layout keeps nothing for the next.
 */
int
main(
	void)
{
	/* The nested boxes of three kinds, timed. */
	min_content_timed("<div style='display:inline-block'>", "</div>", "word ", MIN_CONTENT_DEPTH, "nested inline-blocks");
	min_content_timed("<div style='display:flex'><div style='flex:0 1 auto'>", "</div></div>", "word ", MIN_CONTENT_FLEX_DEPTH, "nested flex rows");
	min_content_timed("<div style='float:left'>", "</div>", "word ", MIN_CONTENT_DEPTH, "nested floats");

	/* The changes between layouts. */
	min_content_changed();

	/* The summary. */
	printf("host-min-content: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
min_content_check(
	int condition,
	const char *what)
{
	/* Each check counts; a failed one is printed. */
	checks++;
	if (condition)
		return;
	failures++;
	printf("FAIL %s\n", what);
}

/* Makes a page of an HTML text with the checked-in fonts and an 800 by 600 viewport. */
static int
min_content_open(
	const char *html,
	size_t length,
	struct page **page)
{
	static const struct text_font_paths paths = {
		"userland/desktop/fonts/Mahora-Regular.ttf",
		"userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf",
	};
	int error;

	/* The page, its fonts, its viewport and its document. */
	error = page_create(page, __builtin_frame_address(0));
	if (error != 0)
		return error;
	page_set_fonts(*page, &paths);
	page_set_viewport(*page, 800, 600);
	error = page_load_html(*page, (const unsigned char *)html, length);
	if (error == 0)
		error = page_open_fonts(*page, &paths);
	if (error != 0) {
		page_destroy(*page);
		return error;
	}

	/* Succeeded: the page is ready to lay out. */
	return 0;
}

/* Runs a script in a page's realm. */
static int
min_content_script(
	struct page *page,
	const char *source)
{
	struct js_syntax_error syntax;
	struct wb_units units;
	vm_value answer;
	int error;

	/* The source as UTF-16, run as a classic script. */
	wb_units_init(&units);
	error = wb_utf8_to_units((const unsigned char *)source, strlen(source), &units);
	if (error == 0)
		error = js_run_script(page->realm, units.data, units.length, 0, &answer, &syntax);
	wb_units_release(&units);
	if (error != 0)
		return error;

	/* Succeeded: the script ran. */
	return 0;
}

/* Lays a page out at a size and dumps the layout. */
static int
min_content_dump(
	struct page *page,
	int width,
	int height,
	struct wb_buffer *dump)
{
	int error;

	/* The layout, then its dump in place of an earlier one. */
	dump->length = 0;
	error = page_layout(page, width, height);
	if (error == 0)
		error = layout_dump(&page->layout, dump);
	if (error != 0)
		return error;

	/* Succeeded: the dump is the layout's. */
	return 0;
}

/* Lays out a page of boxes nested deep levels (each with a text) and checks the time it takes. */
static void
min_content_timed(
	const char *open,
	const char *close,
	const char *text,
	int deep,
	const char *what)
{
	struct wb_buffer html;
	struct wb_buffer dump;
	struct page *page;
	char line[160];
	double start;
	double seconds;
	int depth;
	int error;

	/* The page's text. */
	wb_buffer_init(&html);
	wb_buffer_init(&dump);
	error = wb_buffer_append_string(&html, "<!doctype html><html><body>");
	for (depth = 0; depth < deep && error == 0; depth++) {
		error = wb_buffer_append_string(&html, open);
		if (error == 0)
			error = wb_buffer_append_string(&html, text);
	}
	for (depth = 0; depth < deep && error == 0; depth++)
		error = wb_buffer_append_string(&html, close);
	if (error == 0)
		error = wb_buffer_append_string(&html, "</body></html>");
	min_content_check(error == 0, "fixture text");
	if (error != 0) {
		wb_buffer_release(&html);
		return;
	}

	/* The page laid out, timed. */
	error = min_content_open((const char *)html.data, html.length, &page);
	snprintf(line, sizeof(line), "%s: page opened", what);
	min_content_check(error == 0, line);
	if (error == 0) {
		start = min_content_now();
		error = min_content_dump(page, 800, 600, &dump);
		seconds = min_content_now() - start;
		snprintf(line, sizeof(line), "%s: laid out", what);
		min_content_check(error == 0, line);
		snprintf(line, sizeof(line), "%s: laid out in %.3f s (at most %.1f s)", what, seconds, MIN_CONTENT_SECONDS);
		min_content_check(seconds <= MIN_CONTENT_SECONDS, line);
		printf("host-min-content: %s, %d deep: %.3f s\n", what, deep, seconds);
		fflush(stdout);
		page_destroy(page);
	}
	wb_buffer_release(&html);
	wb_buffer_release(&dump);
}

/*
 * Changes one page between its layouts and another before its first, and
 * compares their layouts after each change (the viewport's width changes
 * last, on both).
 */
static void
min_content_changed(void)
{
	struct wb_buffer changed;
	struct wb_buffer fresh;
	struct page *page;
	struct page *other;
	char line[160];
	size_t count;
	size_t index;
	size_t step;
	int width;
	int opened;
	int same;
	int error;

	/* The page changed between layouts. */
	wb_buffer_init(&changed);
	wb_buffer_init(&fresh);
	error = min_content_open(min_content_page, sizeof(min_content_page) - 1U, &page);
	min_content_check(error == 0, "changed page opened");
	if (error != 0)
		return;
	error = min_content_dump(page, 800, 600, &changed);
	min_content_check(error == 0, "changed page laid out");

	/* Each change, then each viewport width, compared with a page that had them before its first layout. */
	count = sizeof(min_content_changes) / sizeof(min_content_changes[0]);
	for (step = 0; step <= count + 1U && error == 0; step++) {
		width = step <= count ? 800 : 500;
		if (step < count)
			error = min_content_script(page, min_content_changes[step]);
		if (error == 0)
			error = min_content_dump(page, width, 600, &changed);
		snprintf(line, sizeof(line), "step %zu: the changed page laid out", step);
		min_content_check(error == 0, line);
		if (error != 0)
			break;

		/* The other page, with the changes so far made before its first layout. */
		error = min_content_open(min_content_page, sizeof(min_content_page) - 1U, &other);
		opened = error;
		for (index = 0; index <= step && index < count && error == 0; index++)
			error = min_content_script(other, min_content_changes[index]);
		if (error == 0)
			error = min_content_dump(other, width, 600, &fresh);
		if (opened == 0)
			page_destroy(other);
		snprintf(line, sizeof(line), "step %zu: the fresh page laid out", step);
		min_content_check(error == 0, line);
		if (error != 0)
			break;

		/* The two layouts are the same. */
		same = changed.length == fresh.length && memcmp(changed.data, fresh.data, changed.length) == 0;
		snprintf(line, sizeof(line), "step %zu: the changed page's layout equals the fresh page's", step);
		min_content_check(same, line);
	}

	/* The pages and the dumps. */
	page_destroy(page);
	wb_buffer_release(&changed);
	wb_buffer_release(&fresh);
}

/* The monotonic clock in seconds. */
static double
min_content_now(void)
{
	struct timespec time;

	/* The clock's seconds and nanoseconds. */
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}
