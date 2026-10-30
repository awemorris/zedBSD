/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The view transition of a page (https://drafts.csswg.org/css-view-transitions-1/): the script side (the
 * prelude's startViewTransition) asks the page to capture the old state, to make the new one and show the
 * pseudo-element tree, and to end; the layout and the painting read the transition from the page.
 */

#include "page/page.h"

#include <errno.h>
#include <string.h>

static void transition_end(struct page *page);
static double transition_longest(const struct layout_box *box, int depth);

/* Ends the transition: its snapshots are freed and the page is laid out without its tree. */
static void
transition_end(
	struct page *page)
{
	if (!page->transition_on)
		return;
	layout_transition_release(&page->transition);
	page->transition_on = 0;
	page->transition_generation++;
}

/* Makes the page's layout current. */
static int
transition_layout(
	struct page *page)
{
	int error;

	if (!page_needs_layout(page))
		return 0;

	/* A page whose view has given it no fonts or no size yet cannot be laid out. */
	if (page->font_paths == NULL || page->viewport_width <= 0 || page->viewport_height <= 0)
		return EINVAL;
	error = page_open_fonts(page, page->font_paths);
	if (error != 0)
		return error;
	return page_layout(page, page->viewport_width, page->viewport_height);
}

/*
 * One step of a view transition: 1 captures the old state (the root's and each named box's painting and
 * rectangle), 2 measures the new state and shows the tree, 0 ends it.  Returns whether the step was made; a
 * step that was not ends the transition.
 */
int
page_view_transition(
	void *context,
	int step)
{
	struct page *page;
	struct layout_transition_name *entry;
	const struct layout_box *box;
	size_t index;
	int error;

	page = context;
	if (step == 0) {
		transition_end(page);
		return 1;
	}
	if (step == 1) {
		transition_end(page);

		/* The layout as it is now; nothing of the transition is in it yet. */
		error = transition_layout(page);
		if (error != 0 || page->layout.root == NULL)
			return 0;
		page->transition_on = 1;
		page->transition.heap = page->heap;
		error = layout_transition_measure(&page->layout, &page->transition, 1);
		for (index = 0; error == 0 && index < page->transition.count; index++) {
			entry = &page->transition.names[index];
			box = NULL;
			if (!entry->is_root) {
				box = layout_transition_box(&page->transition, page->layout.root, entry->name, 0);
				if (box == NULL)
					continue;
			}
			error = paint_transition_capture(&page->layout, &page->transition, box, &entry->old_image);
		}
		if (error != 0) {
			transition_end(page);
			return 0;
		}
		page->transition_generation++;
		return 1;
	}

	/* The new state: measured without the tree, then shown. */
	if (!page->transition_on)
		return 0;
	page->transition.show = 0;
	page->transition_generation++;
	error = transition_layout(page);
	if (error == 0 && page->layout.root != NULL)
		error = layout_transition_measure(&page->layout, &page->transition, 0);
	if (error != 0) {
		transition_end(page);
		return 0;
	}
	page->transition.show = 1;
	page->transition.elapsed = 0.0f;
	page->transition_generation++;
	return 1;
}

/* The longest an animation of the transition's pseudo-elements runs below a box, in seconds (a paused one never ends). */
static double
transition_longest(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	double longest;
	double length;

	if (depth > LAYOUT_DEPTH_MAX)
		return 0.0;
	longest = 0.0;
	if (box->vt_tree && box->style.animation_name != NULL) {
		if (box->style.animation_paused)
			longest = 1.0e9;
		else
			longest = (double)box->style.animation_duration + (box->style.animation_delay > 0 ? (double)box->style.animation_delay : 0.0);
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		length = transition_longest(child, depth + 1);
		if (length > longest)
			longest = length;
	}
	return longest;
}

/* The milliseconds until the animations of the shown tree have all ended. */
double
page_view_transition_duration(
	void *context)
{
	struct page *page;

	page = context;
	if (!page->transition_on || transition_layout(page) != 0 || page->layout.root == NULL)
		return 0.0;
	return transition_longest(page->layout.root, 0) * 1000.0;
}
