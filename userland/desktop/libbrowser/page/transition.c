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
#include <stdio.h>
#include <stdlib.h>
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

/* Sets what a script's animation of a pseudo-element of a name gives now (empty text: nothing), and has the page laid out again. */
void
page_view_transition_style(
	void *context,
	int pseudo,
	const struct vm_string *name,
	const char *text)
{
	struct page *page;
	struct layout_transition_name *entry;
	char *slot;

	page = context;
	if (!page->transition_on || pseudo < CSS_PSEUDO_ELEMENT_VT_GROUP || pseudo > CSS_PSEUDO_ELEMENT_VT_NEW)
		return;
	entry = layout_transition_find(&page->transition, name);
	if (entry == NULL)
		return;
	slot = entry->animated[pseudo - CSS_PSEUDO_ELEMENT_VT_GROUP];
	if (strncmp(slot, text, sizeof(entry->animated[0])) == 0)
		return;
	strncpy(slot, text, sizeof(entry->animated[0]) - 1U);
	slot[sizeof(entry->animated[0]) - 1U] = '\0';
	page->transition_generation++;
}

/* Sets how long the animations of the shown tree have run (a script setting the time of an animation), and has the page laid out again. */
void
page_view_transition_time(
	void *context,
	double milliseconds)
{
	struct page *page;

	page = context;
	if (!page->transition_on)
		return;
	page->transition.elapsed = (float)(milliseconds / 1000.0);
	page->transition_generation++;
}

/* Lists the names of the shown transition, one "name flags" line each (1: an old snapshot, 2: a new one). */
int
page_view_transition_names(
	void *context,
	struct wb_units *units)
{
	struct page *page;
	const struct layout_transition_name *entry;
	size_t index;
	size_t at;
	int error;

	page = context;
	if (!page->transition_on)
		return 0;
	for (index = 0; index < page->transition.count; index++) {
		entry = &page->transition.names[index];
		if (!entry->has_old && !entry->has_new)
			continue;
		for (at = 0; entry->name != NULL && at < entry->name->length; at++) {
			error = wb_units_append_code_point(units, vm_string_at(entry->name, at));
			if (error != 0)
				return error;
		}
		error = wb_units_append_code_point(units, ' ');
		if (error == 0)
			error = wb_units_append_code_point(units, (uint32_t)('0' + (entry->has_old ? 1 : 0) + (entry->has_new ? 2 : 0)));
		if (error == 0)
			error = wb_units_append_code_point(units, '\n');
		if (error != 0)
			return error;
	}
	return 0;
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

/*
 * Computes the style of a view transition pseudo-element of the root element, each inheriting from the one
 * above it (the group's classes are the entry's of the name when there is a transition).
 */
int
page_vt_pseudo_style(
	void *context,
	struct dom_element *element,
	int pseudo,
	const struct vm_string *name,
	struct css_style *style)
{
	struct page *page;
	struct css_style *chain;
	const struct layout_transition_name *entry;
	static const char prefix[] = "-ua-view-transition-group-anim-";
	const char *given;
	uint16_t units[160];
	size_t used;
	size_t at;
	const struct vm_string *classes;
	size_t first;
	size_t last;
	size_t index;
	int siblings;
	int level;
	int count;
	int kind;
	int error;

	page = context;

	/* ::before and ::after of any element: the cascade's style for the pseudo-element, inheriting from the element's. */
	if (pseudo == CSS_PSEUDO_ELEMENT_BEFORE || pseudo == CSS_PSEUDO_ELEMENT_AFTER) {
		chain = malloc(sizeof(*chain));
		if (chain == NULL)
			return ENOMEM;
		error = page_computed_style(page, element, chain);
		if (error == 0)
			error = css_engine_compute_pseudo(page->css, element, pseudo, chain, style);
		free(chain);
		return error;
	}
	if (element != (struct dom_element *)page->document->node.first_child && element->node.parent != &page->document->node)
		return ENOENT;
	chain = malloc(5U * sizeof(*chain));
	if (chain == NULL)
		return ENOMEM;
	error = page_computed_style(page, element, &chain[0]);
	classes = NULL;
	if (error == 0 && page->transition_on) {
		entry = layout_transition_find(&page->transition, name);
		if (entry != NULL)
			classes = entry->classes;
	}
	/* The root, the group and the pair come before an old or new image; each is the parent of the next. */
	count = pseudo <= CSS_PSEUDO_ELEMENT_VT_PAIR ? pseudo - CSS_PSEUDO_ELEMENT_VT_ROOT + 1 : 4;
	siblings = CSS_VT_FIRST | CSS_VT_LAST;
	if (page->transition_on) {
		first = page->transition.count;
		last = 0;
		for (index = 0; index < page->transition.count; index++) {
			if (!page->transition.names[index].has_old && !page->transition.names[index].has_new)
				continue;
			if (first == page->transition.count)
				first = index;
			last = index;
		}
		entry = layout_transition_find(&page->transition, name);
		if (entry != NULL && pseudo == CSS_PSEUDO_ELEMENT_VT_GROUP)
			siblings = (entry == &page->transition.names[first] ? CSS_VT_FIRST : 0) | (entry == &page->transition.names[last] ? CSS_VT_LAST : 0);
		if (entry != NULL && entry->has_old && entry->has_new && pseudo == CSS_PSEUDO_ELEMENT_VT_OLD)
			siblings = CSS_VT_FIRST;
		if (entry != NULL && entry->has_old && entry->has_new && pseudo == CSS_PSEUDO_ELEMENT_VT_NEW)
			siblings = CSS_VT_LAST;
	}
	for (level = 0; error == 0 && level < count; level++) {
		kind = level == count - 1 ? pseudo : CSS_PSEUDO_ELEMENT_VT_ROOT + level;
		error = css_engine_compute_vt_pseudo(page->css, element, kind, kind == CSS_PSEUDO_ELEMENT_VT_ROOT ? NULL : name,
		    kind == CSS_PSEUDO_ELEMENT_VT_ROOT ? NULL : classes, kind == pseudo ? siblings : CSS_VT_FIRST | CSS_VT_LAST,
		    &chain[level], &chain[level + 1]);
	}
	if (error == 0)
		*style = chain[count];

	/* The UA's animations by the snapshots a name has: the group moves only between two, an image fades by its state, and a group's mix-blend-mode needs both. */
	if (error == 0 && page->transition_on && style->animation_name != NULL && pseudo >= CSS_PSEUDO_ELEMENT_VT_GROUP && pseudo <= CSS_PSEUDO_ELEMENT_VT_NEW) {
		entry = layout_transition_find(&page->transition, name);
		if (entry != NULL) {
			given = NULL;
			used = 0;
			if (pseudo == CSS_PSEUDO_ELEMENT_VT_GROUP) {
				if (entry->has_old && entry->has_new) {
					for (at = 0; prefix[at] != '\0'; at++)
						units[used++] = (uint16_t)(unsigned char)prefix[at];
					for (at = 0; entry->name != NULL && at < entry->name->length && used < 160U; at++)
						units[used++] = vm_string_at(entry->name, at);
				}
			} else if (pseudo == CSS_PSEUDO_ELEMENT_VT_OLD) {
				if (entry->has_old)
					given = entry->has_new ? "-ua-view-transition-fade-out, -ua-mix-blend-mode-plus-lighter" : "-ua-view-transition-fade-out";
			} else if (pseudo == CSS_PSEUDO_ELEMENT_VT_NEW) {
				if (entry->has_new)
					given = entry->has_old ? "-ua-view-transition-fade-in, -ua-mix-blend-mode-plus-lighter" : "-ua-view-transition-fade-in";
			}
			for (at = 0; given != NULL && given[at] != '\0' && used < 160U; at++)
				units[used++] = (uint16_t)(unsigned char)given[at];
			if (used == 0)
				style->animation_name = NULL;
			else if (pseudo != CSS_PSEUDO_ELEMENT_VT_PAIR)
				style->animation_name = vm_atom_from_units(page->heap, units, used);
		}
	}
	free(chain);
	return error;
}

/* Tells the style engine whether a view transition is active and its types, and has the page styled again. */
void
page_view_transition_active(
	void *context,
	int active,
	struct vm_string **types,
	size_t count)
{
	struct page *page;
	size_t index;

	page = context;
	page->vt_active = active;
	page->vt_type_count = 0;
	for (index = 0; index < count && index < 8U; index++)
		page->vt_types[page->vt_type_count++] = types[index];
	if (page->css != NULL) {
		css_engine_set_view_transition(page->css, active, page->vt_types, page->vt_type_count);
		css_engine_forget_styles(page->css);
	}
	if (page->query_css != NULL)
		css_engine_set_view_transition(page->query_css, active, page->vt_types, page->vt_type_count);
	page->document->generation++;
}
