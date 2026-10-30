/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * View transitions (https://drafts.csswg.org/css-view-transitions-1/): the names a transition has and where
 * their boxes are, measured from a laid out tree.  The tree of pseudo-elements itself is built with the boxes
 * (box.c) and painted from the snapshots (paint/list.c).
 */

#include "layout/layout.h"
#include "paint/paint.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int transition_collect(const struct layout_box *box, struct layout_transition *transition, int old_state, int depth);
static struct layout_transition_name *transition_add(struct layout_transition *transition, struct vm_string *name);
static void transition_rect(const struct layout_box *box, struct layout_rect *rect);
static float transition_ease(float progress);

/* Finds the entry of a name (NULL when the transition has none). */
struct layout_transition_name *
layout_transition_find(
	const struct layout_transition *transition,
	const struct vm_string *name)
{
	size_t index;

	if (transition == NULL || name == NULL)
		return NULL;
	for (index = 0; index < transition->count; index++) {
		if (transition->names[index].name != NULL && vm_string_equal(transition->names[index].name, name))
			return &transition->names[index];
	}
	return NULL;
}

/* Finds the box of a name in a laid out tree (NULL: none). */
const struct layout_box *
layout_transition_box(
	const struct layout_box *box,
	const struct vm_string *name,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	if (depth > LAYOUT_DEPTH_MAX || box->vt_tree)
		return NULL;
	if (box->node != NULL && box->parent != NULL && box->style.view_transition_name != NULL &&
	    (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_REPLACED) && vm_string_equal(box->style.view_transition_name, name))
		return box;
	for (child = box->first_child; child != NULL; child = child->next) {
		found = layout_transition_box(child, name, depth + 1);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/* Frees what a transition holds (not the transition itself). */
void
layout_transition_release(
	struct layout_transition *transition)
{
	size_t index;

	for (index = 0; index < transition->count; index++)
		paint_snapshot_release(transition->names[index].old_image);
	free(transition->names);
	memset(transition, 0, sizeof(*transition));
}

/* Adds an entry for a name at the end. */
static struct layout_transition_name *
transition_add(
	struct layout_transition *transition,
	struct vm_string *name)
{
	struct layout_transition_name *grown;
	struct layout_transition_name *entry;
	size_t capacity;

	if (transition->count == transition->capacity) {
		capacity = transition->capacity == 0 ? 8U : transition->capacity * 2U;
		grown = realloc(transition->names, capacity * sizeof(*grown));
		if (grown == NULL)
			return NULL;
		transition->names = grown;
		transition->capacity = capacity;
	}
	entry = &transition->names[transition->count++];
	memset(entry, 0, sizeof(*entry));
	entry->name = name;
	return entry;
}

/* A box's border box in the document. */
static void
transition_rect(
	const struct layout_box *box,
	struct layout_rect *rect)
{
	rect->x = box->x;
	rect->y = box->y;
	rect->width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	rect->height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
}

/*
 * Finds the boxes with a view-transition-name below a box, in tree order, and records each in the transition:
 * the old state makes entries, the new one marks them.  A name two boxes share makes the transition fail.
 */
static int
transition_collect(
	const struct layout_box *box,
	struct layout_transition *transition,
	int old_state,
	int depth)
{
	const struct layout_box *child;
	struct layout_transition_name *entry;
	struct layout_rect rect;
	int error;

	if (depth > LAYOUT_DEPTH_MAX)
		return 0;
	if (box->vt_tree)
		return 0;
	if ((box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_REPLACED) && box->node != NULL && box->parent != NULL &&
	    box->style.view_transition_name != NULL && box->style.display != CSS_DISPLAY_NONE) {
		entry = layout_transition_find(transition, box->style.view_transition_name);
		transition_rect(box, &rect);
		if (old_state) {
			if (entry != NULL && entry->has_old)
				return EEXIST;
			entry = transition_add(transition, box->style.view_transition_name);
			if (entry == NULL)
				return ENOMEM;
			entry->has_old = 1;
			entry->old_rect = rect;
			entry->classes = box->style.view_transition_class;
		} else {
			if (entry != NULL && entry->has_new)
				return EEXIST;
			if (entry == NULL) {
				entry = transition_add(transition, box->style.view_transition_name);
				if (entry == NULL)
					return ENOMEM;
			}
			entry->has_new = 1;
			entry->new_rect = rect;
			if (entry->classes == NULL)
				entry->classes = box->style.view_transition_class;
		}
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		error = transition_collect(child, transition, old_state, depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Measures a laid out tree for a transition: with old_state, makes the root's entry and an entry for each named
 * box (the old rectangles); otherwise marks the entries whose boxes are in the tree and adds the ones new to it.
 * Fails (EEXIST) when two boxes have one name.
 */
int
layout_transition_measure(
	const struct layout_tree *tree,
	struct layout_transition *transition,
	int old_state,
	struct vm_string *root_name)
{
	struct layout_transition_name *entry;
	size_t index;

	if (tree->root == NULL)
		return EINVAL;
	if (old_state) {
		/* The root group comes first. */
		if (transition->count == 0 && transition_add(transition, root_name) == NULL)
			return ENOMEM;
		entry = &transition->names[0];
		entry->is_root = 1;
		entry->has_old = 1;
		entry->old_rect.x = 0;
		entry->old_rect.y = 0;
		entry->old_rect.width = tree->viewport_width;
		entry->old_rect.height = tree->viewport_height;
	} else {
		for (index = 0; index < transition->count; index++)
			transition->names[index].has_new = 0;
		if (transition->count == 0)
			return EINVAL;
		entry = &transition->names[0];
		entry->has_new = 1;
		entry->new_rect.x = 0;
		entry->new_rect.y = 0;
		entry->new_rect.width = tree->viewport_width;
		entry->new_rect.height = tree->viewport_height;
	}
	return transition_collect(tree->root, transition, old_state, 0);
}

/* The ease timing function: cubic-bezier(0.25, 0.1, 0.25, 1). */
static float
transition_ease(
	float progress)
{
	float low;
	float high;
	float mid;
	float x;
	int step;

	/* Solves x(t) = progress by bisection; then y(t). */
	low = 0.0f;
	high = 1.0f;
	mid = progress;
	for (step = 0; step < 40; step++) {
		mid = (low + high) / 2.0f;
		x = 3.0f * (1.0f - mid) * (1.0f - mid) * mid * 0.25f + 3.0f * (1.0f - mid) * mid * mid * 0.25f + mid * mid * mid;
		if (x < progress)
			low = mid;
		else
			high = mid;
	}
	return 3.0f * (1.0f - mid) * (1.0f - mid) * mid * 0.1f + 3.0f * (1.0f - mid) * mid * mid * 1.0f + mid * mid * mid;
}

/*
 * How far an animation of a pseudo-element has run, 0 to 1 and eased, once the transition has run for elapsed
 * seconds: it fills both ways, so before its start it is at 0 and after its end at 1.  A paused one stays where
 * its delay put it.
 */
float
layout_transition_progress(
	const struct css_style *style,
	float elapsed)
{
	float seconds;
	float progress;

	seconds = style->animation_paused ? 0.0f : elapsed;
	seconds -= style->animation_delay;
	if (style->animation_duration <= 0.0f)
		progress = seconds >= 0.0f ? 1.0f : 0.0f;
	else
		progress = seconds / style->animation_duration;
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;
	return transition_ease(progress);
}

/*
 * Finds the border box of a group at the time the animation of its pseudo-element has reached: the new state's
 * when it has no animation, between the old and the new by the ease timing function otherwise.  A name with one
 * state only keeps its place.  Returns whether a rectangle was found.
 */
int
layout_transition_group_rect(
	const struct layout_transition_name *name,
	const struct css_style *style,
	float elapsed,
	struct layout_rect *rect)
{
	float progress;

	if (!name->has_old && !name->has_new)
		return 0;
	if (!name->has_old) {
		*rect = name->new_rect;
		return 1;
	}
	if (!name->has_new) {
		*rect = name->old_rect;
		return 1;
	}
	if (style->animation_name == NULL) {
		*rect = name->new_rect;
		return 1;
	}

	progress = layout_transition_progress(style, elapsed);
	rect->x = name->old_rect.x + (layout_unit)((float)(name->new_rect.x - name->old_rect.x) * progress);
	rect->y = name->old_rect.y + (layout_unit)((float)(name->new_rect.y - name->old_rect.y) * progress);
	rect->width = name->old_rect.width + (layout_unit)((float)(name->new_rect.width - name->old_rect.width) * progress);
	rect->height = name->old_rect.height + (layout_unit)((float)(name->new_rect.height - name->old_rect.height) * progress);
	return 1;
}
