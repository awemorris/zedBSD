/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Taking the content of a block from the previous tree.
 *
 * When a page changes a little and is laid out again, most blocks come out
 * as they did.  A block that starts a formatting context of its own is laid
 * out from its containing block's width and height alone; if its element and
 * everything below it did not change (the style engine says which elements
 * got another style, the document's record which ones changed otherwise),
 * its style is the same, and the same width and height are given, then what
 * the previous tree laid out below it is what this one would lay out, and is
 * copied instead.
 *
 * Only blocks whose whole content is plain flow are taken: blocks, inline
 * boxes, text, line breaks, images, floats and inline blocks, none of them
 * positioned, transformed, scrolled, in columns, with generated content or
 * a first line of its own.  Anything else is laid out as it always was.
 *
 * The previous tree is read once to find, for each element, its box and
 * whether the box and everything below it is plain.  The copy puts the
 * boxes where the layout leaves them before the final pass makes the
 * positions absolute: the final pass runs over the new tree as it does over
 * any, so the copied boxes' positions are the ones relative to their
 * parents' content boxes (the previous tree's absolute ones less the origin
 * the final pass added).
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* One element's box in the previous tree: the box, whether it and all below it is plain flow, and whether the element has more than one box. */
struct reuse_entry {
	const struct dom_node *node;
	const struct layout_box *box;
	int plain;
	int ambiguous;
};

/* The index of the previous tree: its elements' boxes by element. */
struct layout_reuse {
	struct reuse_entry *table;
	size_t capacity;
	size_t used;
};

/* A box of the previous tree and its copy. */
struct reuse_pair {
	const struct layout_box *old;
	struct layout_box *copy;
};

/* The state of one copy: the pairs made, and a table to find a copy by the box it is of. */
struct reuse_copy {
	struct layout_tree *tree;
	struct reuse_pair *pairs;
	size_t count;
	size_t capacity;
	size_t *table;
	size_t table_capacity;
};

static size_t reuse_count(const struct layout_box *box, int depth);
static int reuse_walk(struct layout_reuse *reuse, const struct layout_box *box, int depth);
static int reuse_local_ok(const struct layout_box *box);
static size_t reuse_slot(const struct reuse_entry *table, size_t capacity, const struct dom_node *node);
static struct reuse_entry *reuse_find(const struct layout_reuse *reuse, const struct dom_node *node);
static unsigned reuse_inputs(const struct layout_tree *tree, const struct layout_box *box);
static int reuse_allowed(const struct layout_tree *tree, const struct layout_box *box);
static int reuse_copy_descend(struct reuse_copy *copy, const struct layout_box *old, struct layout_box *fresh, layout_unit x, layout_unit y, int depth);
static int reuse_copy_blocks(struct reuse_copy *copy, const struct layout_box *old, struct layout_box *fresh, layout_unit x, layout_unit y, int depth);
static int reuse_copy_inlines(struct reuse_copy *copy, const struct layout_box *first, struct layout_box *fresh, layout_unit x, layout_unit y, int depth);
static struct layout_box *reuse_copy_one(struct reuse_copy *copy, const struct layout_box *old, struct layout_box *fresh, layout_unit x, layout_unit y);
static int reuse_copy_pair(struct reuse_copy *copy, const struct layout_box *old, struct layout_box *fresh);
static int reuse_copy_lines(struct reuse_copy *copy, const struct layout_box *old, struct layout_box *fresh);
static struct layout_box *reuse_copy_of(const struct reuse_copy *copy, const struct layout_box *old);
static int reuse_copy_index(struct reuse_copy *copy);
static void reuse_box_results(const struct layout_box *old, struct layout_box *fresh);
static size_t reuse_hash(const void *pointer);

/*
 * Reads the previous tree: the index of its elements' boxes.  Without
 * memory the tree is laid out whole (the tree has no index).
 */
int
layout_reuse_open(
	struct layout_tree *tree,
	const struct layout_tree *previous)
{
	struct layout_reuse *reuse;
	size_t count;
	size_t capacity;

	/* Nothing to take from a tree without boxes. */
	tree->reuse = NULL;
	if (previous->root == NULL)
		return 0;

	/* The index, with room for twice the elements. */
	count = reuse_count(previous->root, 0);
	capacity = 16;
	while (capacity < count * 2U + 2U)
		capacity *= 2U;
	reuse = calloc(1, sizeof(*reuse));
	if (reuse == NULL)
		return ENOMEM;
	reuse->table = calloc(capacity, sizeof(*reuse->table));
	if (reuse->table == NULL) {
		free(reuse);
		return ENOMEM;
	}
	reuse->capacity = capacity;

	/* Each element's box, and whether it is plain all the way down. */
	(void)reuse_walk(reuse, previous->root, 0);
	tree->reuse = reuse;
	return 0;
}

/* Frees the index of the previous tree. */
void
layout_reuse_close(
	struct layout_tree *tree)
{
	if (tree->reuse == NULL)
		return;
	free(tree->reuse->table);
	free(tree->reuse);
	tree->reuse = NULL;
}

/* Counts the boxes of elements below a box (and itself). */
static size_t
reuse_count(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	size_t count;

	count = 0;
	if (depth > LAYOUT_DEPTH_MAX + 8)
		return 0;
	if (box->node != NULL && box->node->type == DOM_ELEMENT)
		count++;
	for (child = box->first_child; child != NULL; child = child->next)
		count += reuse_count(child, depth + 1);
	return count;
}

/*
 * Enters a box and what is below it in the index, children first; returns
 * whether the box and everything below it is plain flow.
 */
static int
reuse_walk(
	struct layout_reuse *reuse,
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	struct reuse_entry *entry;
	size_t slot;
	int plain;

	/* Too deep to be sure. */
	if (depth > LAYOUT_DEPTH_MAX + 8)
		return 0;

	/* The box itself, then all that is below it. */
	plain = reuse_local_ok(box);
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!reuse_walk(reuse, child, depth + 1))
			plain = 0;
	}

	/* An element's box goes in the index; a second one of the same element makes it ambiguous. */
	if (box->node != NULL && box->node->type == DOM_ELEMENT && reuse->used + 1U < reuse->capacity) {
		slot = reuse_slot(reuse->table, reuse->capacity, box->node);
		entry = &reuse->table[slot];
		if (entry->node != NULL) {
			entry->ambiguous = 1;
		} else {
			entry->node = box->node;
			entry->box = box;
			entry->plain = plain;
			reuse->used++;
		}
	}
	return plain;
}

/* Tells whether a box, on its own, is plain flow whose layout and final position need nothing from outside it. */
static int
reuse_local_ok(
	const struct layout_box *box)
{
	double x;
	double y;

	/* The kinds of box, in the flow, not positioned. */
	switch (box->kind) {
	case LAYOUT_BLOCK:
	case LAYOUT_ANONYMOUS_BLOCK:
	case LAYOUT_INLINE:
	case LAYOUT_TEXT:
	case LAYOUT_LINE_BREAK:
	case LAYOUT_REPLACED:
		break;
	default:
		return 0;
	}
	if (box->out_of_flow || box->style.position != CSS_POSITION_STATIC)
		return 0;

	/* The displays whose layout is the block and inline one. */
	if (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_ANONYMOUS_BLOCK || box->kind == LAYOUT_INLINE) {
		if (box->style.display != CSS_DISPLAY_BLOCK && box->style.display != CSS_DISPLAY_INLINE &&
		    box->style.display != CSS_DISPLAY_INLINE_BLOCK && box->style.display != CSS_DISPLAY_LIST_ITEM)
			return 0;
	}

	/* No generated content, first line or letter of its own, columns, other writing mode, containment, shape, transform. */
	if (box->pseudo != 0 || box->continuation != 0 || box->fragmented != 0 || box->logical_root != 0)
		return 0;
	if (box->first_line_style != NULL || box->physical_style != NULL || box->try_base != NULL)
		return 0;
	if (box->vt_tree != 0 || box->vt_old != NULL || box->vt_new != NULL)
		return 0;
	if (box->anchored != 0 || box->anchor_box != NULL || box->grid_area_set != 0 || box->grid_used_valid != 0)
		return 0;
	if (box->subgrid_column_span != 0 || box->subgrid_row_span != 0 || box->grid_orthogonal != 0 || box->cb_set != 0)
		return 0;
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB || box->style.transform_set != 0 || box->style.shape_outside != NULL)
		return 0;
	if (box->style.content_visibility != 0 || box->style.container_type != 0 || box->style.contain != 0U || box->style.column_count != 0)
		return 0;
	if (layout_multicol_wanted(box))
		return 0;

	/* A form control, or a box a scroll moves the content of. */
	if (box->control != 0)
		return 0;
	if (layout_scroll_offset(box, &x, &y))
		return 0;
	return 1;
}

/* Hashes a pointer. */
static size_t
reuse_hash(
	const void *pointer)
{
	return (size_t)((((uintptr_t)pointer >> 4) * 0x9e3779b97f4a7c15ULL) >> 20);
}

/* Finds the slot of the table an element's entry is in, or the empty one it goes in. */
static size_t
reuse_slot(
	const struct reuse_entry *table,
	size_t capacity,
	const struct dom_node *node)
{
	size_t slot;

	slot = reuse_hash(node) & (capacity - 1U);
	while (table[slot].node != NULL && table[slot].node != node)
		slot = (slot + 1U) & (capacity - 1U);
	return slot;
}

/* Finds an element's entry, NULL when the previous tree has no box of it. */
static struct reuse_entry *
reuse_find(
	const struct layout_reuse *reuse,
	const struct dom_node *node)
{
	size_t slot;

	slot = reuse_slot(reuse->table, reuse->capacity, node);
	if (reuse->table[slot].node == NULL)
		return NULL;
	return &reuse->table[slot];
}

/* The flags a box's layout depends on besides its containing block's size: what the tree and the box say about heights, measuring and the writing mode. */
static unsigned
reuse_inputs(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	unsigned inputs;

	inputs = 0U;
	if (tree->containing_height_definite)
		inputs |= 1U;
	if (box->height_indefinite)
		inputs |= 2U;
	if (tree->measuring != 0)
		inputs |= 4U;
	if (box->max_width_hidden)
		inputs |= 8U;
	inputs |= (unsigned)tree->logical_mode << 8;
	return inputs;
}

/*
 * Records what a block's layout was made with, for a later tree to compare:
 * the width of its containing block, the height the tree held (and whether
 * it was definite) and the flags of reuse_inputs.
 */
void
layout_reuse_record(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width)
{
	box->reuse_width = containing_width;
	box->reuse_height = tree->containing_height;
	box->reuse_inputs = reuse_inputs(tree, box);
	box->reuse_recorded = 1;
}

/* Tells whether a box of the new tree may take its content from the previous one: a block of its own formatting context in a block's flow, laid out outside any measuring. */
static int
reuse_allowed(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	const struct layout_box *parent;
	const struct dom_document *document;

	/* A block in the flow, not the root, laid out for real. */
	if (tree->measuring != 0 || box->kind != LAYOUT_BLOCK || box->parent == NULL || box->out_of_flow || box->replaced)
		return 0;
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;

	/* In the flow of a block (a flex or grid item, a table part, is laid out by its container). */
	parent = box->parent;
	if (parent->kind != LAYOUT_BLOCK && parent->kind != LAYOUT_ANONYMOUS_BLOCK)
		return 0;
	if (parent->style.display != CSS_DISPLAY_BLOCK && parent->style.display != CSS_DISPLAY_LIST_ITEM &&
	    parent->style.display != CSS_DISPLAY_INLINE_BLOCK)
		return 0;
	if (box->flex_hypothetical != 0 || box->item_row != 0 || box->item_column != 0 || box->grid_room != 0)
		return 0;

	/* Nothing changed under its element since the previous layout, and nothing asks for everything. */
	document = box->node->document;
	if (document->change_full_layout != 0U)
		return 0;
	if ((box->node->change & (DOM_CHANGED_LAYOUT | DOM_CHANGED_LAYOUT_BELOW)) != 0U)
		return 0;
	return 1;
}

/*
 * Takes a block's content from the previous tree when it can: when the
 * element's box there, and all it holds, is plain flow, nothing changed
 * under the element, the style is the same and the same sizes are given.
 * *grafted tells whether it did (the box has its size and its content, and
 * its layout is done); otherwise the box is as it was, to be laid out.
 * Returns 0 or ENOMEM.
 */
int
layout_reuse_graft(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width,
	int *grafted)
{
	struct reuse_copy copy;
	struct layout_box *first;
	struct layout_box *last;
	struct layout_box *child;
	const struct reuse_entry *entry;
	const struct layout_box *old;
	size_t index;
	int error;

	/* A box that may take its content, and whose element the previous tree has one box of, plain all the way down. */
	*grafted = 0;
	if (tree->reuse == NULL || !reuse_allowed(tree, box))
		return 0;
	entry = reuse_find(tree->reuse, box->node);
	if (entry == NULL || entry->ambiguous || !entry->plain)
		return 0;
	old = entry->box;
	tree->reuse_roots++;

	/* The same box laid out with the same things given. */
	if (!old->reuse_recorded || old->kind != box->kind || old->parent == NULL)
		return 0;
	if (old->reuse_width != containing_width || old->reuse_height != tree->containing_height ||
	    old->reuse_inputs != reuse_inputs(tree, box))
		return 0;
	if (old->children_inline != box->children_inline || old->atomic != box->atomic || old->floating != box->floating ||
	    old->marker_length != box->marker_length || memcmp(old->marker, box->marker, sizeof(box->marker)) != 0)
		return 0;
	if (memcmp(&old->style, &box->style, sizeof(box->style)) != 0)
		return 0;

	/* The copy of everything below it, apart from the box until it has all succeeded. */
	memset(&copy, 0, sizeof(copy));
	copy.tree = tree;
	first = box->first_child;
	last = box->last_child;
	box->first_child = NULL;
	box->last_child = NULL;
	error = reuse_copy_descend(&copy, old, box, old->x + old->border[CSS_LEFT] + old->padding[CSS_LEFT],
	    old->y + old->border[CSS_TOP] + old->padding[CSS_TOP], 0);
	if (error == 0)
		error = reuse_copy_pair(&copy, old, box);
	if (error == 0)
		error = reuse_copy_index(&copy);
	for (index = 0; error == 0 && index < copy.count; index++)
		error = reuse_copy_lines(&copy, copy.pairs[index].old, copy.pairs[index].copy);

	/* A failure leaves the box with the children it was built with. */
	if (error != 0) {
		box->first_child = first;
		box->last_child = last;
		free(copy.pairs);
		free(copy.table);
		return error == ENOMEM ? ENOMEM : 0;
	}

	/* The box's own result, and what is below it, are the copies. */
	reuse_box_results(old, box);
	tree->reuse_grafts++;
	tree->reuse_boxes += copy.count;
	for (child = box->first_child; child != NULL; child = child->next)
		child->parent = box;
	free(copy.pairs);
	free(copy.table);
	layout_reuse_record(tree, box, containing_width);
	*grafted = 1;
	return 0;
}

/* Gives a box the result of the layout of its box in the previous tree (the lines are copied with the pairs). */
static void
reuse_box_results(
	const struct layout_box *old,
	struct layout_box *fresh)
{
	fresh->width = old->width;
	fresh->height = old->height;
	memcpy(fresh->margin, old->margin, sizeof(fresh->margin));
	memcpy(fresh->border, old->border, sizeof(fresh->border));
	memcpy(fresh->padding, old->padding, sizeof(fresh->padding));
	fresh->collapsed_top = old->collapsed_top;
	fresh->collapsed_bottom = old->collapsed_bottom;
	fresh->line_count = old->line_count;
	fresh->vertical_inline = old->vertical_inline;
	fresh->has_baseline = old->has_baseline;
	fresh->control_baseline = old->control_baseline;
	fresh->max_content = old->max_content;
	fresh->max_content_known = old->max_content_known;
	fresh->min_content = old->min_content;
	fresh->min_content_known = old->min_content_known;
	memcpy(fresh->min_content_sizes, old->min_content_sizes, sizeof(fresh->min_content_sizes));
	fresh->min_content_sizing = old->min_content_sizing;
	fresh->min_content_ratio = old->min_content_ratio;
	fresh->min_content_containing = old->min_content_containing;
	fresh->min_content_definite = old->min_content_definite;
	fresh->min_content_indefinite = old->min_content_indefinite;
}

/* Registers the pair of the box taking its content and the box of the previous tree (its lines are copied too). */
static int
reuse_copy_pair(
	struct reuse_copy *copy,
	const struct layout_box *old,
	struct layout_box *fresh)
{
	if (copy->count == copy->capacity) {
		struct reuse_pair *grown;
		size_t capacity;

		capacity = copy->capacity == 0 ? 64 : copy->capacity * 2U;
		grown = realloc(copy->pairs, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		copy->pairs = grown;
		copy->capacity = capacity;
	}
	copy->pairs[copy->count].old = old;
	copy->pairs[copy->count].copy = fresh;
	copy->count++;
	return 0;
}

/* Makes the table to find a copy by the box it is of. */
static int
reuse_copy_index(
	struct reuse_copy *copy)
{
	size_t capacity;
	size_t index;
	size_t slot;

	capacity = 16;
	while (capacity < copy->count * 2U + 2U)
		capacity *= 2U;
	copy->table = calloc(capacity, sizeof(*copy->table));
	if (copy->table == NULL)
		return ENOMEM;
	copy->table_capacity = capacity;
	for (index = 0; index < copy->count; index++) {
		slot = reuse_hash(copy->pairs[index].old) & (capacity - 1U);
		while (copy->table[slot] != 0)
			slot = (slot + 1U) & (capacity - 1U);
		copy->table[slot] = index + 1U;
	}
	return 0;
}

/* Finds the copy of a box of the previous tree; NULL when it was not copied. */
static struct layout_box *
reuse_copy_of(
	const struct reuse_copy *copy,
	const struct layout_box *old)
{
	size_t slot;

	slot = reuse_hash(old) & (copy->table_capacity - 1U);
	while (copy->table[slot] != 0) {
		if (copy->pairs[copy->table[slot] - 1U].old == old)
			return copy->pairs[copy->table[slot] - 1U].copy;
		slot = (slot + 1U) & (copy->table_capacity - 1U);
	}
	return NULL;
}

/*
 * Makes the copy of a box under a parent, at a position (relative to its
 * parent's content box), as the box was before the final pass.  Its lines
 * are made after all the copies are.
 */
static struct layout_box *
reuse_copy_one(
	struct reuse_copy *copy,
	const struct layout_box *old,
	struct layout_box *fresh,
	layout_unit x,
	layout_unit y)
{
	struct layout_box *made;

	made = wb_arena_alloc(&copy->tree->arena, sizeof(*made));
	if (made == NULL)
		return NULL;
	memcpy(made, old, sizeof(*made));
	made->parent = fresh;
	made->first_child = NULL;
	made->last_child = NULL;
	made->next = NULL;
	made->x = x;
	made->y = y;

	/* Last among its parent's children. */
	if (fresh->last_child != NULL)
		fresh->last_child->next = made;
	else
		fresh->first_child = made;
	fresh->last_child = made;
	if (reuse_copy_pair(copy, old, made) != 0)
		return NULL;
	return made;
}

/*
 * Copies what is below a box (old, whose copy is fresh): its content box's
 * top left is x, y in the previous tree's positions, the origin the final
 * pass added to what is in it.
 */
static int
reuse_copy_descend(
	struct reuse_copy *copy,
	const struct layout_box *old,
	struct layout_box *fresh,
	layout_unit x,
	layout_unit y,
	int depth)
{
	if (depth > LAYOUT_DEPTH_MAX)
		return E2BIG;
	if (old->children_inline)
		return reuse_copy_inlines(copy, old->first_child, fresh, x, y, depth);
	return reuse_copy_blocks(copy, old, fresh, x, y, depth);
}

/* Copies the children of a block that holds blocks: each one's position is relative to the block's content box. */
static int
reuse_copy_blocks(
	struct reuse_copy *copy,
	const struct layout_box *old,
	struct layout_box *fresh,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	struct layout_box *made;
	int error;

	for (child = old->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			return EINVAL;
		made = reuse_copy_one(copy, child, fresh, child->x - x, child->y - y);
		if (made == NULL)
			return ENOMEM;
		error = reuse_copy_descend(copy, child, made, child->x + child->border[CSS_LEFT] + child->padding[CSS_LEFT],
		    child->y + child->border[CSS_TOP] + child->padding[CSS_TOP], depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Copies the inline content of a block (x, y: its content box's top left):
 * the inline boxes and text keep the position they have, a float or an
 * inline block there is relative to the block's content box.
 */
static int
reuse_copy_inlines(
	struct reuse_copy *copy,
	const struct layout_box *first,
	struct layout_box *fresh,
	layout_unit x,
	layout_unit y,
	int depth)
{
	const struct layout_box *child;
	struct layout_box *made;
	int error;

	if (depth > LAYOUT_DEPTH_MAX)
		return E2BIG;
	for (child = first; child != NULL; child = child->next) {
		if (child->out_of_flow)
			return EINVAL;

		/* A float or an inline block, which the final pass moved with the content box, and has content of its own. */
		if (child->floating != CSS_FLOAT_NONE || child->atomic) {
			made = reuse_copy_one(copy, child, fresh, child->x - x, child->y - y);
			if (made == NULL)
				return ENOMEM;
			error = reuse_copy_descend(copy, child, made, child->x + child->border[CSS_LEFT] + child->padding[CSS_LEFT],
			    child->y + child->border[CSS_TOP] + child->padding[CSS_TOP], depth + 1);
			if (error != 0)
				return error;
			continue;
		}

		/* An inline box, text, a break: where it is stays, and an inline box's children are searched. */
		made = reuse_copy_one(copy, child, fresh, child->x, child->y);
		if (made == NULL)
			return ENOMEM;
		error = reuse_copy_inlines(copy, child->first_child, made, x, y, depth + 1);
		if (error != 0)
			return error;
	}
	return 0;
}

/* Copies the lines of a box, their fragments and decorations, with the boxes they name taken to their copies. */
static int
reuse_copy_lines(
	struct reuse_copy *copy,
	const struct layout_box *old,
	struct layout_box *fresh)
{
	const struct layout_line *from;
	struct layout_line *to;
	size_t index;
	size_t at;

	/* A box without lines has none. */
	if (old->lines == NULL) {
		fresh->lines = NULL;
		return 0;
	}
	fresh->lines = wb_arena_alloc(&copy->tree->arena, (old->line_count == 0 ? 1U : old->line_count) * sizeof(struct layout_line));
	if (fresh->lines == NULL)
		return ENOMEM;
	for (index = 0; index < old->line_count; index++) {
		from = &old->lines[index];
		to = &fresh->lines[index];
		*to = *from;
		to->fragments = NULL;
		to->decorations = NULL;
		if (from->fragments != NULL) {
			to->fragments = wb_arena_alloc(&copy->tree->arena, (from->fragment_count == 0 ? 1U : from->fragment_count) * sizeof(struct layout_fragment));
			if (to->fragments == NULL)
				return ENOMEM;
			for (at = 0; at < from->fragment_count; at++) {
				to->fragments[at] = from->fragments[at];

				/* A list marker's text is in the box that owns the lines. */
				if (from->fragments[at].text >= old->marker && from->fragments[at].text < old->marker + sizeof(old->marker) / sizeof(old->marker[0]))
					to->fragments[at].text = fresh->marker + (from->fragments[at].text - old->marker);
				if (from->fragments[at].box != NULL) {
					to->fragments[at].box = reuse_copy_of(copy, from->fragments[at].box);
					if (to->fragments[at].box == NULL)
						return EINVAL;
				}
			}
		}
		if (from->decorations != NULL) {
			to->decorations = wb_arena_alloc(&copy->tree->arena, (from->decoration_count == 0 ? 1U : from->decoration_count) * sizeof(struct layout_decoration));
			if (to->decorations == NULL)
				return ENOMEM;
			for (at = 0; at < from->decoration_count; at++) {
				to->decorations[at] = from->decorations[at];
				if (from->decorations[at].box != NULL) {
					to->decorations[at].box = reuse_copy_of(copy, from->decorations[at].box);
					if (to->decorations[at].box == NULL)
						return EINVAL;
				}
			}
		}
	}
	return 0;
}
