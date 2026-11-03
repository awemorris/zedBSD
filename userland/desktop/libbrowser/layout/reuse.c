/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Using the boxes of the previous tree again.
 *
 * When a page changes a little and is laid out again, most of its boxes come
 * out as they did.  A block whose element and everything below it did not
 * change, whose style the engine still holds as it was computed, and that
 * stands in the flow of a block, is not built again: the box of the previous
 * tree, with all it holds, is put under its new parent as it is.  The new tree
 * then owns the memory of the previous one (its arena), as the boxes lie in it.
 *
 * Such a block is laid out again where its place or the sizes it is given
 * changed.  A block that starts a formatting context of its own is laid out
 * from its containing block's width and height alone: given the same ones
 * again it keeps what it has, and the final pass, which makes the positions
 * absolute, moves its content to where the block is now.
 *
 * Only blocks whose whole content is plain flow are shared: blocks, inline
 * boxes, text, line breaks, images, floats and inline blocks, none of them
 * positioned, transformed, scrolled, in columns, with generated content or a
 * first line of its own.  Anything else is built and laid out as it always was.
 * What a block was laid out with, and whether all below it is plain, is
 * recorded with the block (layout_reuse_record) for the next tree.
 *
 * The previous tree is not read as a whole: the boxes of the elements whose
 * boxes the new tree builds are searched for the ones they hold.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * How many trees the arena of a tree may hold besides its own, how many bytes it may hold beyond twice what the
 * last tree built whole took (what dead boxes keep), and how many bytes in all, before the next tree is built whole.
 */
#define REUSE_AGE_MAX		512U
#define REUSE_BYTES_SLACK	((size_t)32 << 20)
#define REUSE_BYTES_MAX		((size_t)1 << 30)

/* One element's box in the previous tree, and whether the element has more than one box there. */
struct reuse_entry {
	const struct dom_node *node;
	struct layout_box *box;
	int ambiguous;
};

/* The boxes of the previous tree found so far, by element. */
struct layout_reuse {
	struct reuse_entry *table;
	size_t capacity;
	size_t used;
};

static int reuse_note(struct layout_reuse *reuse, struct layout_box *box);
static void reuse_note_children(struct layout_reuse *reuse, const struct layout_box *box, int depth);
static int reuse_local_ok(const struct layout_box *box);
static int reuse_plain_below(const struct layout_box *box, int depth);
static size_t reuse_slot(const struct reuse_entry *table, size_t capacity, const struct dom_node *node);
static struct reuse_entry *reuse_find(const struct layout_reuse *reuse, const struct dom_node *node);
static unsigned reuse_inputs(const struct layout_tree *tree, const struct layout_box *box, int own_context);
static int reuse_allowed(const struct layout_tree *tree, const struct layout_box *box);
static void reuse_shift_blocks(struct layout_box *box, layout_unit x, layout_unit y, int depth);
static void reuse_shift_inlines(struct layout_box *first, layout_unit x, layout_unit y, int depth);
static size_t reuse_hash(const void *pointer);

/*
 * Starts the search of the previous tree: its root's box is known.  Without
 * memory, or when the previous tree holds too much, the tree is built whole
 * (the tree has no index).
 */
int
layout_reuse_open(
	struct layout_tree *tree,
	const struct layout_tree *previous)
{
	struct layout_reuse *reuse;
	size_t bytes;

	/* Nothing to take from a tree without boxes, or from one that carries too much. */
	tree->reuse = NULL;
	if (previous->root == NULL)
		return 0;
	bytes = wb_arena_bytes(&previous->arena);
	if (previous->reuse_age >= REUSE_AGE_MAX || bytes > REUSE_BYTES_MAX)
		return 0;
	if (previous->reuse_age != 0 && bytes > previous->reuse_base * 2U + REUSE_BYTES_SLACK)
		return 0;

	/* The index, with the root's box in it. */
	reuse = calloc(1, sizeof(*reuse));
	if (reuse == NULL)
		return ENOMEM;
	reuse->capacity = 256;
	reuse->table = calloc(reuse->capacity, sizeof(*reuse->table));
	if (reuse->table == NULL) {
		free(reuse);
		return ENOMEM;
	}
	if (reuse_note(reuse, previous->root) != 0) {
		free(reuse->table);
		free(reuse);
		return ENOMEM;
	}
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

/*
 * Makes the new tree the owner of the boxes it took from the previous one:
 * their memory is the previous tree's, which has none of its own afterwards.
 * A tree that took nothing leaves the previous one to its owner.
 */
int
layout_reuse_adopt(
	struct layout_tree *tree,
	struct layout_tree *previous)
{
	if (tree->reuse_boxes == 0)
		return 0;
	tree->reuse_base = previous->reuse_age == 0 ? wb_arena_bytes(&previous->arena) : previous->reuse_base;
	wb_arena_adopt(&tree->arena, &previous->arena);
	tree->reuse_age = previous->reuse_age + 1U;
	previous->root = NULL;
	return 1;
}

/*
 * Tells whether the passes that follow the layout have nothing to do in a box's
 * subtree: it is a box of the previous tree whose content is plain flow all the
 * way down (nothing positioned, scrolled, stuck or scaled in it), laid out as it
 * was recorded.
 */
int
layout_reuse_frozen(
	const struct layout_tree *tree,
	const struct layout_box *box)
{
	return tree->reuse_boxes != 0 && box->born != tree->serial && box->reuse_recorded && box->reuse_plain;
}

/* Enters a box of the previous tree in the index (the box of an element; a second one of the same element makes it ambiguous). */
static int
reuse_note(
	struct layout_reuse *reuse,
	struct layout_box *box)
{
	struct reuse_entry *grown;
	struct reuse_entry *entry;
	size_t capacity;
	size_t index;
	size_t slot;

	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;

	/* Grows the table when adding would fill more than half of it. */
	if ((reuse->used + 1U) * 2U > reuse->capacity) {
		capacity = reuse->capacity * 2U;
		grown = calloc(capacity, sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		for (index = 0; index < reuse->capacity; index++) {
			if (reuse->table[index].node == NULL)
				continue;
			slot = reuse_slot(grown, capacity, reuse->table[index].node);
			grown[slot] = reuse->table[index];
		}
		free(reuse->table);
		reuse->table = grown;
		reuse->capacity = capacity;
	}

	/* Its entry. */
	slot = reuse_slot(reuse->table, reuse->capacity, box->node);
	entry = &reuse->table[slot];
	if (entry->node != NULL) {
		entry->ambiguous = 1;
		return 0;
	}
	entry->node = box->node;
	entry->box = box;
	reuse->used++;
	return 0;
}

/* Enters the boxes of the elements under a box (those held by anonymous boxes too) in the index. */
static void
reuse_note_children(
	struct layout_reuse *reuse,
	const struct layout_box *box,
	int depth)
{
	struct layout_box *child;

	if (depth > 8)
		return;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->node == NULL)
			reuse_note_children(reuse, child, depth + 1);
		else if (reuse_note(reuse, child) != 0)
			return;
	}
}

/*
 * Tells the search that the new tree built a box for an element: the boxes of
 * the element's children in the previous tree become the ones that may be
 * taken.
 */
void
layout_reuse_expand(
	struct layout_tree *tree,
	const struct dom_node *node)
{
	const struct reuse_entry *entry;

	if (tree->reuse == NULL)
		return;
	entry = reuse_find(tree->reuse, node);
	if (entry == NULL || entry->ambiguous)
		return;
	reuse_note_children(tree->reuse, entry->box, 0);
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

/* Finds an element's entry, NULL when no box of it was found in the previous tree. */
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

/* The flags a box's layout depends on besides its containing block's size: what the tree and the box say about heights, measuring, the writing mode and (for a block that shares its parent's context) floats. */
static unsigned
reuse_inputs(
	const struct layout_tree *tree,
	const struct layout_box *box,
	int own_context)
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

	/* A block in its parent's formatting context lays out around the floats there are. */
	if (!own_context && tree->floats != NULL && tree->floats->count != 0)
		inputs |= 16U;
	return inputs;
}

/*
 * Records what a block's layout was made with, for a later tree to compare:
 * the width of its containing block, the height the tree held (and whether
 * it was definite) and the flags of reuse_inputs; and whether the box and all
 * below it is plain flow, which a later tree may then use as it is.
 */
void
layout_reuse_record(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width,
	int own_context)
{
	if (!tree->cached)
		return;
	box->reuse_width = containing_width;
	box->reuse_height = tree->containing_height;
	box->reuse_inputs = reuse_inputs(tree, box, own_context);
	box->reuse_recorded = 1;
	box->reuse_plain = reuse_local_ok(box) && reuse_plain_below(box, 0);
}

/* Tells whether everything below a box is plain flow: the blocks by what they recorded, the inline boxes by looking. */
static int
reuse_plain_below(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;

	if (depth > LAYOUT_DEPTH_MAX)
		return 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->kind == LAYOUT_BLOCK || child->kind == LAYOUT_ANONYMOUS_BLOCK) {
			if (!child->reuse_recorded || !child->reuse_plain)
				return 0;
			continue;
		}
		if (!reuse_local_ok(child))
			return 0;
		if (child->kind == LAYOUT_INLINE && !reuse_plain_below(child, depth + 1))
			return 0;
	}
	return 1;
}

/* Tells whether a box that was built for the tree may be a block that keeps what it has: a block in the flow of a block, laid out for real. */
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
 * Takes the box of an element from the previous tree when it can: when the
 * box and all it holds is plain flow, nothing changed under the element, the
 * style engine still has the style of the element as it computed it, and the
 * box goes under a block.  Returns the box (free of its old siblings; the
 * caller puts it under its parent), or NULL when the element is to be built.
 */
struct layout_box *
layout_reuse_take(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_element *element,
	struct layout_box *parent)
{
	const struct reuse_entry *entry;
	const struct dom_document *document;
	struct layout_box *box;

	/* Only under a block of the flow, outside any measuring. */
	if (tree->reuse == NULL || parent == NULL || tree->measuring != 0)
		return NULL;
	if (parent->kind != LAYOUT_BLOCK || parent->replaced)
		return NULL;
	if (parent->style.display != CSS_DISPLAY_BLOCK && parent->style.display != CSS_DISPLAY_LIST_ITEM &&
	    parent->style.display != CSS_DISPLAY_INLINE_BLOCK)
		return NULL;

	/* Nothing changed under the element, and nothing asks for everything. */
	document = element->node.document;
	if (document->change_full_layout != 0U || (element->node.change & (DOM_CHANGED_LAYOUT | DOM_CHANGED_LAYOUT_BELOW)) != 0U)
		return NULL;

	/* The element's one box in the previous tree, a plain block that was laid out, without an animation. */
	entry = reuse_find(tree->reuse, &element->node);
	if (entry == NULL || entry->ambiguous)
		return NULL;
	box = entry->box;
	if (!box->reuse_recorded || !box->reuse_plain || box->kind != LAYOUT_BLOCK || box->out_of_flow || box->replaced)
		return NULL;
	if (box->style.animation_name != NULL)
		return NULL;

	/* The style is as it was when the box was made. */
	tree->reuse_roots++;
	if (!css_engine_style_current(css, element))
		return NULL;

	/* Free of the siblings it had. */
	box->next = NULL;
	tree->reuse_boxes++;
	return box;
}

/*
 * Lets a block that was taken from the previous tree keep what it has when
 * its layout would be the same: when the same sizes are given as when it was
 * laid out, and, for one in its parent's formatting context, there were no
 * floats in that context (none of its own either) and there are none.  *grafted tells
 * whether it does (the final pass moves its content); otherwise it is laid
 * out again, in the place it is.  Returns 0.
 */
int
layout_reuse_graft(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit containing_width,
	int own_context,
	int *grafted)
{
	/* A box this tree made has nothing to keep; one taken is laid out again unless it comes out as it did. */
	*grafted = 0;
	if (tree->reuse == NULL || box->born == tree->serial)
		return 0;
	box->reuse_skipped = 0;
	if (!reuse_allowed(tree, box))
		return 0;
	if (!box->reuse_recorded || !box->reuse_plain)
		return 0;
	/*
	 * A block in its parent's context has its lines and clearance made by the floats there are, not only by whether
	 * there are any, and floats of its own join that context: it keeps what it has only where there were none (the
	 * record is made after its own floats are in) and there are none.
	 */
	if (!own_context && ((box->reuse_inputs | reuse_inputs(tree, box, own_context)) & 16U) != 0U)
		return 0;

	/* The same box laid out with the same things given. */
	if (box->reuse_width != containing_width || box->reuse_height != tree->containing_height ||
	    box->reuse_inputs != reuse_inputs(tree, box, own_context))
		return 0;
	box->reuse_skipped = 1;
	tree->reuse_grafts++;
	*grafted = 1;
	return 0;
}

/*
 * Moves what is below a block that kept its layout to where the block is:
 * the block's border box is at x, y (what the final pass added to its position),
 * and its content, which is as the previous final pass left it, moves by what the
 * block's content box moved.
 */
void
layout_reuse_shift(
	struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	layout_unit content_x;
	layout_unit content_y;
	layout_unit dx;
	layout_unit dy;

	/* The block's position, as the final pass makes any. */
	box->reuse_skipped = 0;
	box->x += x;
	box->y += y;
	content_x = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	content_y = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];

	/* Its content by what the content box moved. */
	dx = content_x - box->abs_x;
	dy = content_y - box->abs_y;
	box->abs_x = content_x;
	box->abs_y = content_y;
	if (dx == 0 && dy == 0)
		return;
	if (box->children_inline)
		reuse_shift_inlines(box->first_child, dx, dy, 0);
	else
		reuse_shift_blocks(box, dx, dy, 0);
}

/* Moves the children of a block that holds blocks (all of them in the flow). */
static void
reuse_shift_blocks(
	struct layout_box *box,
	layout_unit x,
	layout_unit y,
	int depth)
{
	struct layout_box *child;

	if (depth > LAYOUT_DEPTH_MAX)
		return;
	for (child = box->first_child; child != NULL; child = child->next) {
		child->x += x;
		child->y += y;
		child->abs_x += x;
		child->abs_y += y;
		if (child->children_inline)
			reuse_shift_inlines(child->first_child, x, y, depth + 1);
		else
			reuse_shift_blocks(child, x, y, depth + 1);
	}
}

/* Moves the floats and inline blocks among the inline content of a block (the inline boxes and the text stay where they are). */
static void
reuse_shift_inlines(
	struct layout_box *first,
	layout_unit x,
	layout_unit y,
	int depth)
{
	struct layout_box *child;

	if (depth > LAYOUT_DEPTH_MAX)
		return;
	for (child = first; child != NULL; child = child->next) {
		if (child->floating != CSS_FLOAT_NONE || child->atomic) {
			child->x += x;
			child->y += y;
			child->abs_x += x;
			child->abs_y += y;
			if (child->children_inline)
				reuse_shift_inlines(child->first_child, x, y, depth + 1);
			else
				reuse_shift_blocks(child, x, y, depth + 1);
			continue;
		}
		reuse_shift_inlines(child->first_child, x, y, depth + 1);
	}
}
