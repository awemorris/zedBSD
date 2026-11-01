/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of form controls sized by a percentage inside nested flex
 * containers (BUG-399: the search field of a search page was as wide as
 * the width a layout measures content in, 262144 pixels, since the
 * percentage was taken of the container while the container was being
 * measured).
 *
 *   host-control-percent
 *
 * Run from the repository's root (the fonts are the checked-in ones).
 * Each page has a 600 pixel wide flex row holding a 50 pixel box, a nested
 * flex item with the control at width:100%, and another 50 pixel box; the
 * control must stay inside the row, and fill the room left between the two
 * boxes (500 pixels with its border and padding).  Prints one line per failed check and
 * a summary.
 */

#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The checks made and the ones that failed. */
static unsigned checks;
static unsigned failures;

/* One page: what it is, and its control (without a frame, or with box-sizing:border-box, so that its box is 500 pixels wide). */
struct control_case {
	const char *what;
	const char *control;
};

/* The controls, each in the same nested flex items. */
static const struct control_case control_cases[] = {
	{ "textarea, flex:1 items two deep", "<textarea rows=1 style='width:100%;border:0;padding:0'></textarea>" },
	{ "textarea, display:flex, flex:100%", "<textarea rows=1 style='width:100%;display:flex;flex:100%;border:0;padding:0'></textarea>" },
	{ "text field", "<input style='width:100%;border:0;padding:0'>" },
	{ "text field, box-sizing:border-box", "<input style='width:100%;box-sizing:border-box;border:2px solid;padding:3px'>" },
};

static void control_check(int condition, const char *what);
static void control_run(const struct control_case *control);
static const struct layout_box *control_find(const struct layout_box *box, int depth);

/*
 * Lays out each page and checks its control's box.
 */
int
main(
	void)
{
	size_t index;

	/* Each page. */
	for (index = 0; index < sizeof(control_cases) / sizeof(control_cases[0]); index++)
		control_run(&control_cases[index]);

	/* The summary. */
	printf("host-control-percent: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
control_check(
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

/* Lays out one page and checks where its control's border box is. */
static void
control_run(
	const struct control_case *control)
{
	static const struct text_font_paths paths = {
		"userland/desktop/fonts/Mahora-Regular.ttf",
		"userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf",
	};
	const struct layout_box *box;
	struct page *page;
	char html[1024];
	char line[256];
	double left;
	double right;
	int length;
	int error;

	/* The page: a 600 pixel row, a box, the nested items with the control, a box. */
	length = snprintf(html, sizeof(html),
	    "<!doctype html><html><body style='margin:0'><div style='width:600px'><div style='display:flex'>"
	    "<div style='width:50px;flex:none'>a</div><div style='flex:1;display:flex'><div style='display:flex;flex:1;flex-wrap:wrap'>%s"
	    "</div></div><div style='width:50px;flex:none'>b</div></div></div></body></html>", control->control);
	control_check(length > 0 && (size_t)length < sizeof(html), "fixture text");

	/* Laid out at 800 by 600. */
	error = page_create(&page, __builtin_frame_address(0));
	control_check(error == 0, "page made");
	if (error != 0)
		return;
	page_set_fonts(page, &paths);
	page_set_viewport(page, 800, 600);
	error = page_load_html(page, (const unsigned char *)html, (size_t)length);
	if (error == 0)
		error = page_open_fonts(page, &paths);
	if (error == 0)
		error = page_layout(page, 800, 600);
	snprintf(line, sizeof(line), "%s: laid out", control->what);
	control_check(error == 0, line);
	if (error != 0) {
		page_destroy(page);
		return;
	}

	/* The control's box. */
	box = control_find(page->layout.root, 0);
	snprintf(line, sizeof(line), "%s: the control has a box", control->what);
	control_check(box != NULL, line);
	if (box == NULL) {
		page_destroy(page);
		return;
	}

	/* Its box (x is its border edge), between the two 50 pixel boxes. */
	left = layout_to_px(box->x);
	right = layout_to_px(box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] +
	    box->border[CSS_RIGHT]);
	snprintf(line, sizeof(line), "%s: the control spans 50 to 550 (it spans %.2f to %.2f)", control->what, left, right);
	control_check(left > 49.5 && left < 50.5 && right > 549.5 && right < 550.5, line);
	page_destroy(page);
}

/* Finds the first box of a form control in a layout tree, depth first. */
static const struct layout_box *
control_find(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	/* The box itself, then each child's tree. */
	if (box == NULL || depth > 64)
		return NULL;
	if (box->control != DOM_CONTROL_NONE)
		return box;
	for (child = box->first_child; child != NULL; child = child->next) {
		found = control_find(child, depth + 1);
		if (found != NULL)
			return found;
	}
	return NULL;
}
