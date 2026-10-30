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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int transition_collect(const struct layout_box *box, struct layout_transition *transition, int old_state, layout_unit viewport_width, layout_unit viewport_height, int depth);
static struct layout_transition_name *transition_add(struct layout_transition *transition, struct vm_string *name);
static void transition_rect(const struct layout_box *box, struct layout_rect *rect);
static float transition_ease(float progress, const struct vm_string *timing);
static void transition_sort(const struct layout_tree *tree, struct layout_transition *transition);

/*
 * The name a box has in a transition: its view-transition-name, or for match-element a name made from its
 * element (the same one in both states), or NULL.
 */
struct vm_string *
layout_transition_name_of(
	const struct layout_transition *transition,
	const struct layout_box *box)
{
	struct vm_string *name;
	char text[48];
	uint16_t units[48];
	size_t index;
	int length;

	name = box->style.view_transition_name;
	if (name == NULL || transition->heap == NULL || !vm_string_equal_ascii(name, "match-element"))
		return name;
	length = snprintf(text, sizeof(text), "-ua-match-%llx", (unsigned long long)(uintptr_t)box->node);
	for (index = 0; index < (size_t)length; index++)
		units[index] = (uint16_t)text[index];
	return vm_atom_from_units(transition->heap, units, (size_t)length);
}

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
	const struct layout_transition *transition,
	const struct layout_box *box,
	const struct vm_string *name,
	int depth)
{
	const struct layout_box *child;
	const struct layout_box *found;

	if (depth > LAYOUT_DEPTH_MAX || box->vt_tree)
		return NULL;
	if (box->node != NULL && box->parent != NULL && box->style.view_transition_name != NULL &&
	    (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_REPLACED) && vm_string_equal(layout_transition_name_of(transition, box), name))
		return box;
	for (child = box->first_child; child != NULL; child = child->next) {
		found = layout_transition_box(transition, child, name, depth + 1);
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
	layout_unit viewport_width,
	layout_unit viewport_height,
	int depth)
{
	const struct layout_box *child;
	struct layout_transition_name *entry;
	struct layout_rect rect;
	struct vm_string *name;
	int error;

	if (depth > LAYOUT_DEPTH_MAX)
		return 0;
	if (box->vt_tree)
		return 0;
	if ((box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_REPLACED) && box->node != NULL &&
	    box->style.view_transition_name != NULL && box->style.display != CSS_DISPLAY_NONE) {
		name = layout_transition_name_of(transition, box);
		entry = layout_transition_find(transition, name);
		transition_rect(box, &rect);
		if (box->parent == NULL) {
			/* The root is the whole viewport. */
			rect.x = 0;
			rect.y = 0;
			rect.width = viewport_width;
			rect.height = viewport_height;
		}
		if (old_state) {
			if (entry != NULL && entry->has_old)
				return EEXIST;
			entry = transition_add(transition, name);
			if (entry == NULL)
				return ENOMEM;
			entry->has_old = 1;
			entry->is_root = box->parent == NULL;
			entry->old_rect = rect;
			entry->classes = box->style.view_transition_class;
		} else {
			if (entry != NULL && entry->has_new)
				return EEXIST;
			if (entry == NULL) {
				entry = transition_add(transition, name);
				if (entry == NULL)
					return ENOMEM;
			}
			entry->has_new = 1;
			entry->new_rect = rect;
			entry->classes = box->style.view_transition_class;
		}
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		error = transition_collect(child, transition, old_state, viewport_width, viewport_height, depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Measures a laid out tree for a transition: with old_state, makes an entry for the root and each named box
 * (the old rectangles); otherwise marks the entries whose boxes are in the tree and adds the ones new to it.
 * Fails (EEXIST) when two boxes have one name.
 */
int
layout_transition_measure(
	const struct layout_tree *tree,
	struct layout_transition *transition,
	int old_state)
{
	size_t index;
	int error;

	if (tree->root == NULL)
		return EINVAL;
	if (!old_state) {
		for (index = 0; index < transition->count; index++)
			transition->names[index].has_new = 0;
	}
	error = transition_collect(tree->root, transition, old_state, tree->viewport_width, tree->viewport_height, 0);
	if (error != 0)
		return error;
	transition_sort(tree, transition);
	return 0;
}

/*
 * Puts the entries in the order their boxes are painted in (the groups stack that way): the boxes of the normal
 * flow in tree order, between the positioned boxes below them and above them in their stacking order.  The root
 * stays first.
 */
static void
transition_sort(
	const struct layout_tree *tree,
	struct layout_transition *transition)
{
	struct wb_vector order;
	struct layout_transition_name held;
	const struct layout_box *box;
	const struct layout_box *layer;
	size_t flow_index;
	size_t first;
	size_t index;
	size_t other;
	size_t slot;
	double *keys;
	double key;

	first = transition->count != 0 && transition->names[0].is_root ? 1U : 0U;
	if (transition->count < first + 2U)
		return;
	wb_vector_init(&order, sizeof(const struct layout_box *));
	if (layout_stacking_order(tree, &order, &flow_index) != 0) {
		wb_vector_release(&order);
		return;
	}
	keys = calloc(transition->count, sizeof(*keys));
	if (keys == NULL) {
		wb_vector_release(&order);
		return;
	}
	for (index = first; index < transition->count; index++) {
		keys[index] = (double)flow_index - 0.5 + (double)index / 100000.0;
		box = layout_transition_box(transition, tree->root, transition->names[index].name, 0);
		if (box == NULL)
			continue;
		for (slot = 0; slot < order.count; slot++) {
			layer = *(const struct layout_box **)wb_vector_at(&order, slot);
			if (layer == box) {
				keys[index] = (double)slot;
				break;
			}
		}
	}

	/* An insertion sort keeps equal keys in the order they came. */
	for (index = first + 1U; index < transition->count; index++) {
		held = transition->names[index];
		key = keys[index];
		for (other = index; other > first && keys[other - 1] > key; other--) {
			transition->names[other] = transition->names[other - 1];
			keys[other] = keys[other - 1];
		}
		transition->names[other] = held;
		keys[other] = key;
	}
	free(keys);
	wb_vector_release(&order);
}

/* Solves a cubic bezier easing (control points x1 y1 x2 y2) at a progress. */
static float
transition_bezier(
	float progress,
	float x1,
	float y1,
	float x2,
	float y2)
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
		x = 3.0f * (1.0f - mid) * (1.0f - mid) * mid * x1 + 3.0f * (1.0f - mid) * mid * mid * x2 + mid * mid * mid;
		if (x < progress)
			low = mid;
		else
			high = mid;
	}
	return 3.0f * (1.0f - mid) * (1.0f - mid) * mid * y1 + 3.0f * (1.0f - mid) * mid * mid * y2 + mid * mid * mid;
}

/* Applies a timing function (the style's text; NULL is ease) to a progress from 0 to 1. */
static float
transition_ease(
	float progress,
	const struct vm_string *timing)
{
	char text[64];
	size_t index;
	size_t length;
	float a;
	float b;
	float c;
	float d;
	int steps;
	char position[8];
	float step;

	if (timing == NULL)
		return transition_bezier(progress, 0.25f, 0.1f, 0.25f, 1.0f);
	length = timing->length < sizeof(text) - 1U ? timing->length : sizeof(text) - 1U;
	for (index = 0; index < length; index++)
		text[index] = (char)vm_string_at(timing, index);
	text[length] = '\0';
	if (strcmp(text, "linear") == 0)
		return progress;
	if (strcmp(text, "ease-in") == 0)
		return transition_bezier(progress, 0.42f, 0.0f, 1.0f, 1.0f);
	if (strcmp(text, "ease-out") == 0)
		return transition_bezier(progress, 0.0f, 0.0f, 0.58f, 1.0f);
	if (strcmp(text, "ease-in-out") == 0)
		return transition_bezier(progress, 0.42f, 0.0f, 0.58f, 1.0f);
	if (strcmp(text, "step-start") == 0)
		return 1.0f;
	if (strcmp(text, "step-end") == 0)
		return progress >= 1.0f ? 1.0f : 0.0f;
	if (sscanf(text, "bezier %f %f %f %f", &a, &b, &c, &d) == 4)
		return transition_bezier(progress, a, b, c, d);
	if (sscanf(text, "steps %d %7s", &steps, position) == 2 && steps >= 1) {
		step = (float)(int)(progress * (float)steps);
		if (progress >= 1.0f)
			step = (float)steps;
		if (strcmp(position, "start") == 0) {
			step = progress >= 1.0f ? (float)steps : step + 1.0f;
			return step / (float)steps;
		}
		if (strcmp(position, "both") == 0)
			return (step + (progress >= 1.0f ? 0.0f : 1.0f)) / (float)(steps + 1);
		if (strcmp(position, "none") == 0)
			return steps > 1 ? step / (float)(steps - 1) : progress;
		return step / (float)steps;
	}
	return progress;
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
	return transition_ease(progress, style->animation_timing);
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
