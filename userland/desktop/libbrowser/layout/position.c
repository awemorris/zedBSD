/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Positioned boxes: the placing of absolutely positioned and fixed boxes
 * in their containing blocks (CSS 2 §10.3.7 and §10.6.4, simplified), and
 * the painting order of every positioned box.
 *
 * A box out of the flow is placed after the normal flow, in tree order, so
 * its containing block (the padding box of its nearest positioned
 * ancestor, or the viewport's rectangle at the top of the document) is
 * already placed.  An auto width with left or right auto shrinks to fit:
 * the box is laid out once very wide to measure its content, then at the
 * room it has, bounded below by content that cannot break.  A fixed box
 * is placed like an absolute one against the viewport's rectangle and
 * scrolls with the page in this pass.
 *
 * The painting order lists every positioned box as a layer.  A box under
 * an ancestor with an explicit z-index stays in that ancestor's outer
 * stacking level; this keeps the whole subtree above or below the
 * ancestor's siblings.  Within a level, tree order keeps an ancestor
 * before its positioned descendants.  The negative levels are painted
 * first, then the normal flow, then level zero and the positive levels.
 */

#include "layout/layout.h"

#include <errno.h>
#include <string.h>

/* The width a box is laid out in to measure its content's widest line (262144 pixels). */
#define POSITION_MEASURE_WIDTH	((layout_unit)1 << 24)

/*
 * A containing block: the rectangle of a positioned box's padding box, or
 * of the viewport, in absolute layout units.
 */
struct position_block {
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
	/* The block's writing mode and direction (what start and end mean to a box placed in it). */
	int mode;
	int direction;
};

/*
 * One positioned box with its z-index, while the painting order is sorted.
 */
struct position_entry {
	const struct layout_box *box;
	int z;
};

static int position_walk(struct layout_tree *tree, struct layout_box *box, const struct position_block *containing, const struct position_block *viewport, int depth);
static int position_place(struct layout_tree *tree, struct layout_box *box, const struct position_block *containing);
static int position_place_once(struct layout_tree *tree, struct layout_box *box, const struct position_block *containing);
static int position_place_fallbacks(struct layout_tree *tree, struct layout_box *box, const struct position_block *containing);
static int position_fits(const struct layout_box *box, const struct position_block *containing);
static int position_aligned(int alignment);
static int position_physical(int alignment, int rtl);
static layout_unit position_align(int alignment, layout_unit start, layout_unit end, layout_unit margin_start, layout_unit margin_end, layout_unit size);
static uint32_t position_try_key(const struct css_try_list *list);
static uint32_t position_style_key(const struct css_style *style);
static int position_in_scroller(const struct layout_box *box);
static void position_padding_box(const struct layout_box *box, struct position_block *block);
static int position_offset(const struct css_length *length, layout_unit size, layout_unit *value);
static layout_unit position_frame(const struct layout_box *box);
static layout_unit position_outer_height(const struct layout_box *box);
static int position_collect(const struct layout_box *box, struct wb_vector *entries, int depth, int constrained, int outer_z);
static layout_unit position_inline_floats(const struct layout_box *box, int depth);

/*
 * Places every box out of the flow of a laid out tree (whose normal flow
 * has absolute positions), and makes the document tall enough for those
 * that scroll with it.
 */
int
layout_position(
	struct layout_tree *tree)
{
	struct position_block viewport;
	int error;

	/* The initial containing block is the viewport's rectangle at the document's top left. */
	if (tree->root == NULL)
		return 0;
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = tree->viewport_width;
	viewport.height = tree->viewport_height;
	viewport.mode = CSS_WRITING_HORIZONTAL_TB;
	viewport.direction = CSS_DIRECTION_LTR;

	/* Walks from the root, whose own containing block is the viewport (a new pass: no box is placed yet). */
	tree->position_pass++;
	error = position_walk(tree, tree->root, &viewport, &viewport, 0);
	if (error != 0)
		return error;

	/* Succeeded: every box is placed. */
	return 0;
}

/*
 * Places one box out of the flow now (an anchor that another box needs
 * before the walk reaches it), in the containing block its ancestors give
 * it.
 */
int
layout_position_box(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct position_block containing;
	const struct layout_box *ancestor;

	/* Once a pass. */
	if (box->placed_pass == tree->position_pass)
		return 0;

	/* The nearest ancestor that contains it, else the viewport's rectangle. */
	containing.x = 0;
	containing.y = 0;
	containing.width = tree->viewport_width;
	containing.height = tree->viewport_height;
	containing.mode = CSS_WRITING_HORIZONTAL_TB;
	containing.direction = CSS_DIRECTION_LTR;
	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (box->style.position == CSS_POSITION_FIXED ? layout_contains_fixed(ancestor) : layout_contains_absolute(ancestor)) {
			position_padding_box(ancestor, &containing);
			break;
		}
	}

	/* Placed there. */
	return position_place(tree, box, &containing);
}

/*
 * Lists the positioned boxes of a tree in painting order, and reports at
 * which index of the list the normal flow is painted (after the boxes
 * with a negative z-index).  boxes holds const struct layout_box pointers.
 */
int
layout_stacking_order(
	const struct layout_tree *tree,
	struct wb_vector *boxes,
	size_t *flow_index)
{
	struct wb_vector entries;
	struct position_entry *entry;
	struct position_entry moved;
	size_t index;
	size_t place;
	int error;

	/* The positioned boxes in tree order. */
	*flow_index = 0;
	wb_vector_init(&entries, sizeof(struct position_entry));
	error = 0;
	if (tree->root != NULL)
		error = position_collect(tree->root, &entries, 0, 0, 0);
	if (error != 0) {
		wb_vector_release(&entries);
		return error;
	}

	/* A stable insertion sort by z-index keeps tree order among equals. */
	for (index = 1; index < entries.count; index++) {
		entry = wb_vector_at(&entries, index);
		moved = *entry;
		for (place = index; place > 0; place--) {
			entry = wb_vector_at(&entries, place - 1U);
			if (entry->z <= moved.z)
				break;
			*(struct position_entry *)wb_vector_at(&entries, place) = *entry;
		}

		/* The entry goes into the gap. */
		*(struct position_entry *)wb_vector_at(&entries, place) = moved;
	}

	/* The boxes in that order; the normal flow goes after the negative ones. */
	for (index = 0; index < entries.count; index++) {
		entry = wb_vector_at(&entries, index);
		if (entry->z < 0)
			*flow_index = index + 1U;
		error = wb_vector_push(boxes, &entry->box);
		if (error != 0) {
			wb_vector_release(&entries);
			return error;
		}
	}

	/* Succeeded: the order is listed. */
	wb_vector_release(&entries);
	return 0;
}

/*
 * Lays out a box whose width is auto so it shrinks to its content: at the
 * room (minus margins, borders and padding), bounded below by its narrowest
 * broken content and above by its width without a limit
 * (layout_max_content).  A box with a width is laid out in the room as it
 * is.  The style keeps its auto width, so another room shrinks it again.
 */
int
layout_shrink_to_fit(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit room)
{
	struct css_length width;
	struct css_length maximum;
	layout_unit content;
	layout_unit minimum;
	layout_unit outside;
	layout_unit available;
	int indefinite_percentage;
	int error;

	/* A box of the other orientation fits its content along its own inline axis when it is laid out. */
	if (layout_logical_wanted(tree, box) &&
	    (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) != (tree->logical_mode == CSS_WRITING_HORIZONTAL_TB)) {
		error = layout_block(tree, box, room);
		return error;
	}

	/* A percentage is indefinite while an ancestor's intrinsic width is being measured. */
	width = box->style.width;
	indefinite_percentage = tree->measuring != 0 && width.unit == CSS_UNIT_PERCENT;
	if (width.unit != CSS_UNIT_AUTO && !indefinite_percentage) {
		error = layout_block(tree, box, room);
		return error;
	}

	/* An aspect-ratio and a pixel height give an auto width (the height is the box-sizing box's, as the width would be). */
	if (width.unit == CSS_UNIT_AUTO && !box->replaced && box->style.aspect_ratio > 0.0f && box->style.height.unit == CSS_UNIT_PX) {
		layout_unit target;

		/* The width the ratio gives, though a box that shows its overflow is no narrower than its content's least width. */
		target = layout_from_px(box->style.height.value * box->style.aspect_ratio);
		if (box->style.overflow_x == CSS_OVERFLOW_VISIBLE && box->style.overflow_y == CSS_OVERFLOW_VISIBLE) {
			layout_unit frame;

			frame = 0;
			if (box->style.box_sizing == CSS_BOX_SIZING_BORDER) {
				layout_box_model(box, room);
				frame = position_frame(box);
			}
			error = layout_min_content(tree, box, &minimum);
			if (error != 0)
				return error;
			if (target - frame < minimum)
				target = minimum + frame;
		}
		box->style.width.unit = CSS_UNIT_PX;
		box->style.width.value = layout_to_px(target);
		box->style.width.offset = 0;
		error = layout_block(tree, box, room);
		box->style.width = width;
		return error;
	}

	/* An indefinite percentage behaves as auto only for this measurement. */
	if (indefinite_percentage)
		box->style.width.unit = CSS_UNIT_AUTO;

	/* The content's width without a limit (the box's own max-width limits the result, not the content). */
	maximum = box->style.max_width;
	if (maximum.unit == CSS_UNIT_PX) {
		box->style.max_width.unit = CSS_UNIT_NONE;
		box->max_width_hidden = 1;
	}
	error = layout_max_content(tree, box, &content);
	box->max_width_hidden = 0;
	box->style.max_width = maximum;
	if (error != 0) {
		box->style.width = width;
		return error;
	}

	/* The room available to the content after its margins and frame. */
	layout_box_model(box, room);
	outside = position_frame(box) + box->margin[CSS_LEFT] + box->margin[CSS_RIGHT];
	available = room - outside;
	if (available < 0)
		available = 0;

	/* CSS shrink-to-fit keeps the widest unbreakable content even when the room is empty. */
	if (content > available) {
		error = layout_min_content(tree, box, &minimum);
		if (error != 0) {
			box->style.width = width;
			return error;
		}
		content = available;
		if (content < minimum)
			content = minimum;
		if (content > box->max_content)
			content = box->max_content;
	}

	/* An aspect-ratio carries a pixel minimum (or maximum) height over to the width. */
	if (!box->replaced && box->style.aspect_ratio > 0.0f && box->style.height.unit == CSS_UNIT_AUTO) {
		layout_unit transferred;
		layout_unit frame_y;

		frame_y = 0;
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			frame_y = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (box->style.min_height.unit == CSS_UNIT_PX) {
			transferred = (layout_unit)((float)(layout_from_px(box->style.min_height.value) - frame_y) * box->style.aspect_ratio);
			if (content < transferred)
				content = transferred;
		}
		if (box->style.max_height.unit == CSS_UNIT_PX) {
			transferred = (layout_unit)((float)(layout_from_px(box->style.max_height.value) - frame_y) * box->style.aspect_ratio);
			if (content > transferred)
				content = transferred;
		}
	}

	/* Under box-sizing: border-box the width given is the border box's. */
	if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
		content += position_frame(box);

	/* The box is laid out at that width, given for this layout only. */
	box->style.width.unit = CSS_UNIT_PX;
	box->style.width.value = layout_to_px(content);
	box->style.width.offset = 0;
	error = layout_block(tree, box, room);
	box->style.width = width;
	if (error != 0)
		return error;

	/* Succeeded: the box has shrunk to fit. */
	return 0;
}

/*
 * Measures the width an orthogonal grid item takes: its block size, laid out
 * along the inline room its row gives (the row's height when the row has a
 * fixed one, otherwise the viewport's, as a fit-content size).
 */
static int
position_orthogonal_width(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit *width)
{
	struct layout_context context;
	struct wb_vector floats;
	layout_unit saved_height;
	int saved_definite;
	int error;

	/* The room the row gives along the item's inline axis. */
	saved_height = tree->containing_height;
	saved_definite = tree->containing_height_definite;
	tree->containing_height_definite = box->grid_room >= 0;
	tree->containing_height = box->grid_room >= 0 ? box->grid_room : 0;

	/* The item laid out in a context of its own. */
	tree->measuring++;
	layout_context_begin(tree, &context, &floats);
	error = layout_block(tree, box, 0);
	layout_context_end(tree, &context);
	tree->measuring--;
	tree->containing_height = saved_height;
	tree->containing_height_definite = saved_definite;
	if (error != 0)
		return error;

	/* Succeeded: its physical width is its block size. */
	*width = box->width;
	return 0;
}

/*
 * Measures the width a box's content takes laid out without a limit (its
 * max-content width): the box laid out very wide, while the count of
 * measurements in progress makes the percentages inside indefinite.  The
 * width does not depend on the room the box is later laid out in, so it
 * is measured once and kept in the box.
 */
int
layout_max_content(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit *width)
{
	struct layout_context context;
	struct wb_vector floats;
	int error;

	/* An orthogonal grid item's width is its block size. */
	if (box->grid_orthogonal && !box->out_of_flow && !box->logical_root && box->physical_style == NULL &&
	    layout_logical_wanted(tree, box))
		return position_orthogonal_width(tree, box, width);

	/* A box measured before has its width already. */
	if (box->max_content_known) {
		*width = box->max_content;
		return 0;
	}

	/*
	 * The content, laid out very wide in a formatting context of its own, so
	 * that the floats of the measurement do not stay in the context the box
	 * is in (ws074-p074: a box that starts no context of its own is measured
	 * too, for an intrinsic width).
	 */
	tree->measuring++;
	layout_context_begin(tree, &context, &floats);
	error = layout_block(tree, box, POSITION_MEASURE_WIDTH);
	layout_context_end(tree, &context);
	if (error != 0) {
		tree->measuring--;
		return error;
	}

	/* The content's own width, with the measurement over. */
	box->max_content = layout_content_width(box, 0);
	box->max_content_known = 1;
	tree->measuring--;

	/* Succeeded: the width is measured. */
	*width = box->max_content;
	return 0;
}

/* Measures the narrowest content width with every permitted line break taken. */
int
layout_min_content(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit *width)
{
	struct layout_context context;
	struct wb_vector floats;
	struct css_length given;
	int error;

	/* A replaced item cannot break inside its own width. */
	if (box->replaced) {
		*width = box->width;
		return 0;
	}

	/* An orthogonal grid item's width is its block size. */
	if (box->grid_orthogonal && !box->out_of_flow && !box->logical_root && box->physical_style == NULL &&
	    layout_logical_wanted(tree, box))
		return position_orthogonal_width(tree, box, width);

	/* Lay it out in zero room to take every allowed line break. */
	given = box->style.width;
	box->style.width.unit = CSS_UNIT_AUTO;
	tree->measuring++;
	layout_context_begin(tree, &context, &floats);
	error = layout_block(tree, box, 0);
	layout_context_end(tree, &context);
	tree->measuring--;
	box->style.width = given;
	if (error != 0)
		return error;

	/* The widest remaining line or child is the min-content width. */
	*width = layout_content_width(box, 0);

	/* A single-line row of flex items is as narrow as its items' min-content contributions side by side, whatever they would shrink to. */
	if ((box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX) &&
	    box->style.flex_wrap == CSS_FLEX_NOWRAP && box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    (box->style.flex_direction == CSS_FLEX_ROW || box->style.flex_direction == CSS_FLEX_ROW_REVERSE)) {
		struct layout_box *item;
		layout_unit sum;
		layout_unit least;
		layout_unit frame;
		int items;

		sum = 0;
		items = 0;
		for (item = box->first_child; item != NULL; item = item->next) {
			if (item->out_of_flow)
				continue;
			if (item->style.writing_mode != CSS_WRITING_HORIZONTAL_TB || item->grid_orthogonal)
				return 0;
			frame = item->border[CSS_LEFT] + item->padding[CSS_LEFT] + item->padding[CSS_RIGHT] + item->border[CSS_RIGHT];
			if (item->style.width.unit == CSS_UNIT_PX) {
				least = layout_from_px(item->style.width.value);
				if (item->style.box_sizing == CSS_BOX_SIZING_BORDER)
					least -= frame;
			} else if (item->style.width.unit == CSS_UNIT_AUTO) {
				error = layout_min_content(tree, item, &least);
				if (error != 0)
					return error;
			} else {
				return 0;
			}
			sum += least + frame + item->margin[CSS_LEFT] + item->margin[CSS_RIGHT];
			items++;
		}
		if (items > 1 && box->style.column_gap.unit == CSS_UNIT_PX)
			sum += (layout_unit)(items - 1) * layout_from_px(box->style.column_gap.value);
		if (sum < *width)
			*width = sum;
	}
	return 0;
}

/*
 * Measures the width a laid out box's content needs: its longest line (or
 * its widest float among its inline content), or its widest child's margin
 * box.
 */
layout_unit
layout_content_width(
	const struct layout_box *box,
	int depth)
{
	const struct layout_line *line;
	const struct layout_fragment *last;
	const struct layout_box *child;
	layout_unit widest;
	layout_unit width;
	layout_unit floats_left;
	layout_unit floats_right;
	layout_unit edge;
	size_t index;

	/* Stops at the depth the layout stops at. */
	widest = 0;
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* Size containment (or inline-size containment) gives the box no intrinsic width. */
	if (!box->replaced && (layout_containment(box) & 17U) != 0)
		return 0;

	/*
	 * A replaced box's content is as wide as it was sized; one sized by a
	 * percentage of the width being measured is as wide as its image (a
	 * control as its natural width), since that width is indefinite.
	 */
	if (box->replaced && box->style.width.unit == CSS_UNIT_PERCENT) {
		if (box->image != NULL)
			return (layout_unit)box->image->width * LAYOUT_UNIT;
		return box->natural_width;
	}

	/* Any other replaced box. */
	if (box->replaced)
		return box->width;

	/* A grid or a table is as wide as its columns at their content's sizes (ws074-p072, ws074-p037). */
	if (box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID)
		return box->grid_content;
	if (box->style.display == CSS_DISPLAY_TABLE || box->style.display == CSS_DISPLAY_INLINE_TABLE)
		return box->grid_content;

	/* A physical horizontal flex main axis is as wide as its items before they flexed. */
	if ((box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX) &&
	    ((box->style.flex_direction == CSS_FLEX_ROW || box->style.flex_direction == CSS_FLEX_ROW_REVERSE) !=
	    (box->style.writing_mode == CSS_WRITING_VERTICAL_LR))) {
		width = layout_flex_content_width(box);
		return width;
	}

	/* Lines: the end of each line's last piece. */
	if (box->children_inline) {
		/* Vertical lines stack in physical x, so their block-axis widths add. */
		if (box->vertical_inline) {
			for (index = 0; index < box->line_count; index++)
				widest += box->lines[index].height;
			return widest;
		}

		for (index = 0; index < box->line_count; index++) {
			line = &box->lines[index];
			if (line->fragment_count == 0)
				continue;
			last = &line->fragments[line->fragment_count - 1U];

			/* A hanging marker comes after the line's pieces and ends before them. */
			if (box->marker_length != 0 && last->text == box->marker && line->fragment_count > 1U)
				last = &line->fragments[line->fragment_count - 2U];
			width = last->x + last->width;
			if (width > widest)
				widest = width;
		}

		/* A float among the content needs its margin box at least. */
		width = position_inline_floats(box, depth);
		if (width > widest)
			widest = width;

		/* The longest line. */
		return widest;
	}

	/*
	 * Blocks: each child's margin box, its content measured when its width
	 * is auto or a percentage (a percentage of the width being measured is
	 * indefinite, so it contributes its content, as in Chromium).
	 */
	floats_left = 0;
	floats_right = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		width = child->width;
		if (child->style.width.unit == CSS_UNIT_AUTO) {
			width = layout_content_width(child, depth + 1);
		} else if (child->style.width.unit == CSS_UNIT_PERCENT) {
			width = layout_content_width(child, depth + 1);
		}

		/*
		 * The child's margins, borders and paddings go around it.  An auto
		 * margin contributes zero to an intrinsic size; its resolved value
		 * only places the child in the deliberately very wide measuring box.
		 */
		if (child->style.margin[CSS_LEFT].unit != CSS_UNIT_AUTO) {
			/* In a right-to-left box the left margin took the room the box left over: its own value counts. */
			if (box->style.direction == CSS_DIRECTION_RTL && child->floating == CSS_FLOAT_NONE && !child->atomic &&
			    box->style.display != CSS_DISPLAY_FLEX && box->style.display != CSS_DISPLAY_INLINE_FLEX &&
			    box->style.display != CSS_DISPLAY_GRID && box->style.display != CSS_DISPLAY_INLINE_GRID)
				width += child->style.margin[CSS_LEFT].unit == CSS_UNIT_PX ? layout_from_px(child->style.margin[CSS_LEFT].value) : 0;
			else
				width += child->margin[CSS_LEFT];
		}
		width += position_frame(child);
		if (child->style.margin[CSS_RIGHT].unit != CSS_UNIT_AUTO)
			width += child->margin[CSS_RIGHT];

		/*
		 * Floats stand side by side (ws074-p074): a right one adds its margin
		 * box, a left one reaches its right margin edge (placed relative to
		 * the content box).
		 */
		if (child->floating == CSS_FLOAT_RIGHT) {
			floats_right += width;
			continue;
		}

		/* A left float. */
		if (child->floating != CSS_FLOAT_NONE) {
			edge = child->x + position_frame(child) + child->width + child->margin[CSS_RIGHT];
			if (edge > floats_left)
				floats_left = edge;
			continue;
		}

		/* A block in the flow. */
		if (width > widest)
			widest = width;
	}

	/* The widest block, or the floats side by side. */
	if (floats_left + floats_right > widest)
		widest = floats_left + floats_right;
	return widest;
}

/* Places the boxes out of the flow under a box, in tree order, with the containing block its descendants have. */
static int
position_walk(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct position_block *containing,
	const struct position_block *viewport,
	int depth)
{
	struct position_block own;
	struct layout_box *child;
	const struct position_block *inner;
	int positioned;
	int error;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* A positioned (or transformed) block is the containing block of the boxes out of the flow inside it (a transformed one of the fixed ones too). */
	inner = containing;
	positioned = layout_contains_absolute(box);
	if (positioned) {
		position_padding_box(box, &own);
		inner = &own;
	}
	if (layout_contains_fixed(box))
		viewport = &own;

	/* Each child: one out of the flow is placed first (a fixed one against the viewport), then searched. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow && child->placed_pass == tree->position_pass) {
			/* An anchor of a box before it was placed for that box. */
			error = 0;
		} else if (child->out_of_flow && child->style.position == CSS_POSITION_FIXED) {
			error = position_place(tree, child, viewport);
		} else if (child->out_of_flow) {
			error = position_place(tree, child, inner);
		} else {
			error = 0;
		}

		/* A box that could not be laid out stops the walk. */
		if (error != 0)
			return error;

		/* Its descendants. */
		error = position_walk(tree, child, inner, viewport, depth + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the boxes under this one are placed. */
	return 0;
}

/*
 * Lays out and places one box out of the flow in its containing block,
 * trying its position-try-fallbacks in order when it overflows: the first
 * that fits is kept, else the box as it was.
 * https://drafts.csswg.org/css-anchor-position-1/#fallback-apply
 */
static int
position_place(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct position_block *containing)
{
	int error;

	/* The box, then whether position-visibility hides it. */
	error = position_place_fallbacks(tree, box, containing);
	if (error != 0)
		return error;
	box->anchor_hidden = layout_anchor_hides(tree, box) ||
	    ((box->style.position_visibility & 4) != 0 && !position_fits(box, containing));
	return 0;
}

/* Lays out and places one box, trying its position-try-fallbacks when it overflows. */
static int
position_place_fallbacks(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct position_block *containing)
{
	struct layout_box *saved;
	struct dom_element *element;
	uint32_t key;
	layout_unit metric[CSS_TRY_ENTRIES];
	layout_unit wide;
	layout_unit high;
	int order[CSS_TRY_ENTRIES];
	int entry;
	int index;
	int item;
	int other;
	int error;

	/* The box as it is, placed in this pass. */
	box->placed_pass = tree->position_pass;
	box->anchor_flip = 0;
	if (box->style.position_try == NULL)
		return position_place_once(tree, box, containing);

	/* Each pass starts from the style the box had before a fallback changed it. */
	if (box->try_base == NULL) {
		box->try_base = wb_arena_alloc(&tree->arena, sizeof(*box->try_base));
		if (box->try_base == NULL)
			return ENOMEM;
		*box->try_base = box->style;
	} else {
		box->style = *box->try_base;
	}
	saved = wb_arena_alloc(&tree->arena, sizeof(*saved));
	if (saved == NULL)
		return ENOMEM;
	*saved = *box;

	/* The position option that last fit is kept while it still fits (even when the base position fits again). */
	element = NULL;
	if (box->node != NULL && box->node->type == DOM_ELEMENT)
		element = (struct dom_element *)box->node;
	key = position_try_key(saved->style.position_try) ^ position_style_key(&saved->style);
	if (element != NULL && element->anchor_last > 0 && element->anchor_last_key == key &&
	    element->anchor_last <= saved->style.position_try->count && layout_anchor_try(tree, box, element->anchor_last - 1)) {
		error = position_place_once(tree, box, containing);
		if (error != 0 || position_fits(box, containing))
			return error;
	}
	*box = *saved;
	error = position_place_once(tree, box, containing);
	if (error != 0 || position_fits(box, containing)) {
		/* The base position fits: nothing is remembered. */
		if (error == 0 && box->node != NULL && box->node->type == DOM_ELEMENT)
			((struct dom_element *)box->node)->anchor_last = 0;
		return error;
	}

	/* Each fallback, from the box as it was. */

	/* position-try-order sorts the fallbacks by the size of the inset-modified containing block each gives (the largest first). */
	for (entry = 0; entry < CSS_TRY_ENTRIES; entry++) {
		order[entry] = entry;
		metric[entry] = 0;
	}
	if (saved->style.position_try_order != 0) {
		for (entry = 0; entry < saved->style.position_try->count; entry++) {
			*box = *saved;
			metric[entry] = -1;
			if (!layout_anchor_try(tree, box, entry))
				continue;
			error = position_place_once(tree, box, containing);
			if (error != 0)
				return error;
			wide = box->imcb[1] - box->imcb[0];
			high = box->imcb[3] - box->imcb[2];
			switch (saved->style.position_try_order) {
			case 1:
				metric[entry] = wide;
				break;
			case 2:
				metric[entry] = high;
				break;
			case 3:
				metric[entry] = containing->mode == CSS_WRITING_HORIZONTAL_TB ? high : wide;
				break;
			default:
				metric[entry] = containing->mode == CSS_WRITING_HORIZONTAL_TB ? wide : high;
				break;
			}
		}

		/* A stable insertion sort, the largest first. */
		for (entry = 1; entry < saved->style.position_try->count; entry++) {
			item = order[entry];
			for (other = entry; other > 0 && metric[order[other - 1]] < metric[item]; other--)
				order[other] = order[other - 1];
			order[other] = item;
		}
	}
	for (index = 0; index < saved->style.position_try->count; index++) {
		entry = order[index];
		*box = *saved;
		if (!layout_anchor_try(tree, box, entry))
			continue;
		error = position_place_once(tree, box, containing);
		if (error != 0 || position_fits(box, containing)) {
			/* The first that fits is remembered. */
			if (element != NULL) {
				element->anchor_last = entry + 1;
				element->anchor_last_key = key;
			}
			return error;
		}
	}

	/* None fits: the last that did when the list is the same, else the box as it was. */
	*box = *saved;
	if (element != NULL && element->anchor_last > 0 && element->anchor_last_key == key &&
	    element->anchor_last <= saved->style.position_try->count && layout_anchor_try(tree, box, element->anchor_last - 1))
		return position_place_once(tree, box, containing);
	*box = *saved;
	box->anchor_flip = 0;
	return position_place_once(tree, box, containing);
}

/* Tells whether a scroll container is among a box's ancestors (its containing block then reaches its scrollable overflow). */
static int
position_in_scroller(
	const struct layout_box *box)
{
	const struct layout_box *ancestor;

	for (ancestor = box->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor->parent == NULL)
			break;
		if (ancestor->style.overflow_x != CSS_OVERFLOW_VISIBLE || ancestor->style.overflow_y != CSS_OVERFLOW_VISIBLE)
			return 1;
	}
	return 0;
}

/* Mixes a length into a hash. */
static uint32_t
position_hash_length(
	uint32_t key,
	const struct css_length *length)
{
	union {
		float number;
		uint32_t bits;
	} value;

	value.number = length->value;
	key = key * 16777619u ^ value.bits;
	key = key * 16777619u ^ (uint32_t)length->unit;
	value.number = length->offset;
	return key * 16777619u ^ value.bits;
}

/* A number that stays the same while the style that the base position and a fallback list depend on does. */
static uint32_t
position_style_key(
	const struct css_style *style)
{
	uint32_t key;
	int side;

	key = 2166136261u;
	for (side = 0; side < 4; side++) {
		key = position_hash_length(key, &style->offset[side]);
		key = position_hash_length(key, &style->margin[side]);
	}
	key = position_hash_length(key, &style->width);
	key = position_hash_length(key, &style->height);
	key = position_hash_length(key, &style->min_width);
	key = position_hash_length(key, &style->min_height);
	key = position_hash_length(key, &style->max_width);
	key = position_hash_length(key, &style->max_height);
	key = key * 16777619u ^ (uint32_t)style->position_area;
	key = key * 16777619u ^ (uint32_t)style->position_try_order;
	key = key * 16777619u ^ (uint32_t)style->justify_self;
	key = key * 16777619u ^ (uint32_t)style->align_self;
	return key;
}

/* A number that stays the same while a position-try-fallbacks list does. */
static uint32_t
position_try_key(
	const struct css_try_list *list)
{
	uint32_t key;
	int entry;
	int tactic;

	/* Each entry's name, tactics and area. */
	key = 2166136261u ^ (uint32_t)list->count;
	for (entry = 0; entry < list->count; entry++) {
		if (list->entries[entry].name != NULL)
			key = key * 16777619u ^ vm_string_hash(list->entries[entry].name);
		for (tactic = 0; tactic < list->entries[entry].tactic_count; tactic++)
			key = key * 16777619u ^ (uint32_t)list->entries[entry].tactics[tactic];
		key = key * 16777619u ^ (uint32_t)list->entries[entry].area;
	}
	return key;
}

/* Tells whether a placed box's margin box is inside its containing block. */
static int
position_fits(
	const struct layout_box *box,
	const struct position_block *containing)
{
	layout_unit left;
	layout_unit top;
	layout_unit right;
	layout_unit bottom;

	/* The inset-modified containing block of the placing is what it fits in. */
	(void)containing;

	/* A box a position-area had to move back inside the block did not fit. */
	if (box->area_overflow)
		return 0;

	/* The margin box against the block. */
	left = box->x - box->margin[CSS_LEFT];
	top = box->y - box->margin[CSS_TOP];
	right = box->x + position_frame(box) + box->width + box->margin[CSS_RIGHT];
	bottom = box->y + position_outer_height(box) + box->margin[CSS_BOTTOM];
	return left >= box->imcb[0] && top >= box->imcb[2] && right <= box->imcb[1] && bottom <= box->imcb[3];
}

/* Lays out and places one box out of the flow in its containing block. */
static int
position_place_once(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct position_block *containing)
{
	const struct css_length *offset;
	layout_unit left;
	layout_unit right;
	layout_unit top;
	layout_unit bottom;
	layout_unit room;
	layout_unit frame;
	layout_unit height;
	layout_unit content;
	layout_unit x;
	layout_unit y;
	layout_unit bottom_edge;
	layout_unit area_left;
	layout_unit area_top;
	layout_unit area_width;
	layout_unit area_height;
	layout_unit saved_height;
	struct css_length saved_style_height;
	struct css_length saved_box_width;
	struct css_length saved_box_min_width;
	struct css_length saved_box_min_height;
	layout_unit frame_width;
	layout_unit frame_height;
	int width_converted;
	int saved_sizing;
	int saved_width_sizing;
	struct css_length saved_width;
	struct css_length saved_orthogonal_height;
	int saved_definite;
	int has_left;
	int has_right;
	int has_top;
	int has_bottom;
	int error;

	/* The position-area clamp is set again by the area. */
	box->area_clamp_x = 0;
	box->area_clamp_y = 0;
	box->area_inset_x = 0;
	box->area_inset_y = 0;
	box->area_overflow = 0;
	box->area_scrolls = position_in_scroller(box);

	/* Insets and sizes with anchor functions are finished against the containing block. */
	if (layout_anchor_wanted(box))
		layout_anchor_resolve(tree, box, containing->x, containing->y, containing->width, containing->height);

	/* A position-area makes an area of the grid around the anchor the containing block; anchor-center centers on the anchor. */
	if (box->style.position_area >= 0)
		layout_anchor_area(tree, box, containing->mode, containing->direction,
		    containing->x, containing->y, containing->width, containing->height);
	layout_anchor_center(tree, box, containing->x, containing->y, containing->width, containing->height);

	/* A definite Grid area, when supplied, is the inset containing block. */
	area_left = containing->x;
	area_top = containing->y;
	area_width = containing->width;
	area_height = containing->height;
	if (box->cb_set) {
		area_left = box->cb_left_auto ? containing->x : box->cb_left;
		area_top = box->cb_top_auto ? containing->y : box->cb_top;
		area_width = (box->cb_right_auto ? containing->x + containing->width : box->cb_right) - area_left;
		area_height = (box->cb_bottom_auto ? containing->y + containing->height : box->cb_bottom) - area_top;
		if (area_width < 0)
			area_width = 0;
		if (area_height < 0)
			area_height = 0;
	} else if (box->static_grid_area) {
		area_left = box->static_x -
		    (box->static_x_from_right ? box->static_x_area_width : 0);
		area_top = box->static_y;
		area_width = box->static_x_area_width;
		area_height = box->static_y_area_height;
	}

	/* The offsets that are not auto, against their containing block's size. */
	offset = box->style.offset;
	has_left = position_offset(&offset[CSS_LEFT], area_width, &left);
	has_right = position_offset(&offset[CSS_RIGHT], area_width, &right);
	has_top = position_offset(&offset[CSS_TOP], area_height, &top);
	has_bottom = position_offset(&offset[CSS_BOTTOM], area_height, &bottom);

	/* Under a position-area an inset that is auto is zero when the size is auto: the box fills the area's edges. */
	if (box->style.position_area >= 0 && box->cb_set) {
		if (box->style.width.unit == CSS_UNIT_AUTO) {
			if (!has_left) {
				has_left = 1;
				left = 0;
			}
			if (!has_right) {
				has_right = 1;
				right = 0;
			}
		}
		if (box->style.height.unit == CSS_UNIT_AUTO) {
			if (!has_top) {
				has_top = 1;
				top = 0;
			}
			if (!has_bottom) {
				has_bottom = 1;
				bottom = 0;
			}
		}
	}

	/* The containing block the insets leave: what a fallback has to fit in. */
	box->imcb[0] = area_left + (has_left ? left : 0);
	box->imcb[1] = area_left + area_width - (has_right ? right : 0);
	box->imcb[2] = area_top + (has_top ? top : 0);
	box->imcb[3] = area_top + area_height - (has_bottom ? bottom : 0);

	/* The room between the offsets that are set. */
	room = area_width;
	if (has_left)
		room -= left;
	if (has_right)
		room -= right;
	if (room < 0)
		room = 0;

	/* Auto margins are zero unless both sides are set (they then center a box of a set width). */
	if (!has_left || !has_right) {
		if (box->style.margin[CSS_LEFT].unit == CSS_UNIT_AUTO) {
			box->style.margin[CSS_LEFT].unit = CSS_UNIT_PX;
			box->style.margin[CSS_LEFT].value = 0.0f;
		}

		/* The same on the right. */
		if (box->style.margin[CSS_RIGHT].unit == CSS_UNIT_AUTO) {
			box->style.margin[CSS_RIGHT].unit = CSS_UNIT_PX;
			box->style.margin[CSS_RIGHT].value = 0.0f;
		}
	}

	/* A percentage height of the box is of its containing block's padding box. */
	saved_height = tree->containing_height;
	saved_definite = tree->containing_height_definite;
	tree->containing_height = area_height;
	tree->containing_height_definite = 1;

	/* A flex or grid container of an auto height between a top and a bottom lays its items out in that definite height. */
	saved_style_height = box->style.height;
	saved_sizing = box->style.box_sizing;
	width_converted = 0;
	if (box->style.height.unit == CSS_UNIT_AUTO && has_top && has_bottom &&
	    (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID)) {
		layout_box_model(box, area_width);
		height = area_height - top - bottom - box->margin[CSS_TOP] - box->margin[CSS_BOTTOM] -
		    box->border[CSS_TOP] - box->padding[CSS_TOP] - box->padding[CSS_BOTTOM] - box->border[CSS_BOTTOM];
		if (height < 0)
			height = 0;
		box->style.height.unit = CSS_UNIT_PX;
		box->style.height.value = layout_to_px(height);
		box->style.height.offset = 0;

		/* The box now sizes its content: a border-box width is the content width less the frame. */
		if (saved_sizing == CSS_BOX_SIZING_BORDER) {
			frame_width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
			frame_height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
			saved_box_width = box->style.width;
			saved_box_min_width = box->style.min_width;
			saved_box_min_height = box->style.min_height;
			width_converted = 1;
			if (box->style.width.unit == CSS_UNIT_PX)
				box->style.width.value -= layout_to_px(frame_width);
			if (box->style.width.unit == CSS_UNIT_PX && box->style.width.value < 0)
				box->style.width.value = 0;
			if (box->style.min_width.unit == CSS_UNIT_PX)
				box->style.min_width.value -= layout_to_px(frame_width);
			if (box->style.min_height.unit == CSS_UNIT_PX)
				box->style.min_height.value -= layout_to_px(frame_height);
		}
		box->style.box_sizing = CSS_BOX_SIZING_CONTENT;
	}

	/* An orthogonal box between a left and a right (or a top and a bottom) fills the room with its block (inline) size. */
	saved_width = width_converted ? saved_box_width : box->style.width;
	saved_orthogonal_height = box->style.height;
	saved_width_sizing = -1;
	if (!box->replaced && (box->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) != (tree->logical_mode == CSS_WRITING_HORIZONTAL_TB) &&
	    ((box->style.width.unit == CSS_UNIT_AUTO && has_left && has_right) ||
	    (box->style.height.unit == CSS_UNIT_AUTO && has_top && has_bottom)) &&
	    layout_logical_wanted(tree, box)) {
		layout_box_model(box, area_width);
		saved_width_sizing = box->style.box_sizing;
		box->style.box_sizing = CSS_BOX_SIZING_CONTENT;
		if (box->style.width.unit == CSS_UNIT_AUTO && has_left && has_right) {
			content = room - box->margin[CSS_LEFT] - box->margin[CSS_RIGHT] - box->border[CSS_LEFT] - box->padding[CSS_LEFT] -
			    box->padding[CSS_RIGHT] - box->border[CSS_RIGHT];
			if (content < 0)
				content = 0;
			box->style.width.unit = CSS_UNIT_PX;
			box->style.width.value = layout_to_px(content);
			box->style.width.offset = 0;
		}
		if (box->style.height.unit == CSS_UNIT_AUTO && has_top && has_bottom) {
			content = area_height - top - bottom - box->margin[CSS_TOP] - box->margin[CSS_BOTTOM] - box->border[CSS_TOP] -
			    box->padding[CSS_TOP] - box->padding[CSS_BOTTOM] - box->border[CSS_BOTTOM];
			if (content < 0)
				content = 0;
			box->style.height.unit = CSS_UNIT_PX;
			box->style.height.value = layout_to_px(content);
			box->style.height.offset = 0;
		}
	}

	/* A width set, or both sides set, lays the box out in that room; otherwise it shrinks to fit. */
	if (box->style.width.unit != CSS_UNIT_AUTO || (has_left && has_right)) {
		/* A percentage width is of the containing block, not of the room between the offsets. */
		error = layout_block(tree, box,
		    box->style.width.unit == CSS_UNIT_PERCENT ? area_width : room);
	} else {
		error = layout_shrink_to_fit(tree, box, room);
	}
	box->style.height = saved_style_height;
	box->style.box_sizing = saved_sizing;
	box->style.width = saved_width;
	if (width_converted) {
		box->style.min_width = saved_box_min_width;
		box->style.min_height = saved_box_min_height;
	}
	box->style.height = saved_orthogonal_height;
	if (saved_width_sizing >= 0)
		box->style.box_sizing = saved_width_sizing;
	tree->containing_height = saved_height;
	tree->containing_height_definite = saved_definite;

	/* A box that could not be laid out stops the placing. */
	if (error != 0)
		return error;

	/* A height left auto between a top and a bottom fills the room between them. */
	if (box->style.height.unit == CSS_UNIT_AUTO && has_top && has_bottom) {
		height = area_height - top - bottom - box->margin[CSS_TOP] - box->margin[CSS_BOTTOM] -
		    box->border[CSS_TOP] - box->padding[CSS_TOP] - box->padding[CSS_BOTTOM] - box->border[CSS_BOTTOM];
		if (height < 0)
			height = 0;
		content = box->height;
		box->height = height;
		layout_center_button(box, content);
	}

	/* Horizontally: from the left, from the right, or where it would have been. */
	frame = position_frame(box);
	if (has_left && has_right && box->style.width.unit != CSS_UNIT_AUTO &&
	    position_aligned(position_physical(box->style.justify_self, box->style.direction == CSS_DIRECTION_RTL))) {
		/* Both insets set and a width that does not stretch: justify-self aligns the box between them. */
		x = position_align(position_physical(box->style.justify_self, box->style.direction == CSS_DIRECTION_RTL),
		    area_left + left, area_left + area_width - right,
		    box->margin[CSS_LEFT], box->margin[CSS_RIGHT], box->width + frame);
	} else if (has_left && has_right && box->style.width.unit != CSS_UNIT_AUTO && containing->direction == CSS_DIRECTION_RTL &&
	    !box->area_inset_x) {
		/* Over-constrained in a right-to-left containing block: the left inset is ignored. */
		x = area_left + area_width - right - box->margin[CSS_RIGHT] - (box->width + frame);
	} else if (has_left && !box->area_inset_x) {
		x = area_left + left + box->margin[CSS_LEFT];
	} else if (has_right && !box->area_inset_x) {
		x = area_left + area_width - right - box->margin[CSS_RIGHT] - (box->width + frame);
	} else if (box->static_x_align == CSS_ALIGN_END ||
	    box->static_x_align == CSS_ALIGN_SELF_END ||
	    box->static_x_align == CSS_ALIGN_SELF_START ||
	    box->static_x_align == CSS_ALIGN_LEFT ||
	    box->static_x_align == CSS_ALIGN_RIGHT) {
		if (box->static_x_safe && box->static_x_area_width < box->margin[CSS_LEFT] + box->width + frame + box->margin[CSS_RIGHT]) {
			x = box->static_x + box->margin[CSS_LEFT];
		} else if (box->static_x_align_from_right) {
			x = box->static_x +
			    (box->static_x_from_right ? 0 : box->static_x_area_width) -
			    box->margin[CSS_RIGHT] - (box->width + frame);
		} else {
			x = box->static_x -
			    (box->static_x_from_right ? box->static_x_area_width : 0) +
			    box->margin[CSS_LEFT];
		}
	} else if (box->static_x_align == CSS_ALIGN_CENTER) {
		layout_unit free_width;

		free_width = box->static_x_area_width - box->margin[CSS_LEFT] -
		    (box->width + frame) - box->margin[CSS_RIGHT];
		if (free_width < 0 && box->static_x_safe == 2) {
			/* A safe overflow starts at the right edge. */
			x = box->static_x - (box->static_x_from_right ? box->static_x_area_width : 0) +
			    box->margin[CSS_LEFT] + free_width;
			goto placed_x;
		}
		if (free_width < 0 && (!box->static_unsafe || box->static_x_safe))
			free_width = 0;
		x = box->static_x -
		    (box->static_x_from_right ? box->static_x_area_width : 0) +
		    box->margin[CSS_LEFT] + free_width / 2;
	} else if (box->static_x_from_right) {
		/* A Grid inline-start edge needs the width that was just measured. */
		x = box->static_x - box->margin[CSS_RIGHT] - (box->width + frame);
	} else {
		x = box->static_x + box->margin[CSS_LEFT];
	}

placed_x:
	/* A position-area's normal alignment keeps an overflowing box inside the containing block. */
	if (box->area_clamp_x && !box->area_scrolls) {
		if (x + box->width + frame + box->margin[CSS_RIGHT] > box->area_clamp[1]) {
			box->area_overflow = 1;
			x = box->area_clamp[1] - box->margin[CSS_RIGHT] - frame - box->width;
		}
		if (x - box->margin[CSS_LEFT] < box->area_clamp[0]) {
			box->area_overflow = 1;
			x = box->area_clamp[0] + box->margin[CSS_LEFT];
		}
	}

	/* anchor-center keeps the box inside its containing block across. */
	if (box->anchor_clamp_x) {
		if (x + box->width + frame + box->margin[CSS_RIGHT] > box->anchor_clamp[1])
			x = box->anchor_clamp[1] - box->margin[CSS_RIGHT] - frame - box->width;
		if (x - box->margin[CSS_LEFT] < box->anchor_clamp[0])
			x = box->anchor_clamp[0] + box->margin[CSS_LEFT];
	}

	/* Vertically: from the top, from the bottom, or where it would have been. */
	if (has_top && has_bottom && box->style.height.unit != CSS_UNIT_AUTO && position_aligned(box->style.align_self)) {
		y = position_align(box->style.align_self, area_top + top, area_top + area_height - bottom,
		    box->margin[CSS_TOP], box->margin[CSS_BOTTOM], position_outer_height(box));
	} else if (has_top && !box->area_inset_y) {
		y = area_top + top + box->margin[CSS_TOP];
	} else if (has_bottom && !box->area_inset_y) {
		y = area_top + area_height - bottom - box->margin[CSS_BOTTOM] - position_outer_height(box);
	} else if ((box->static_y_align == CSS_ALIGN_END ||
	    box->static_y_align == CSS_ALIGN_SELF_END) && box->static_y_safe &&
	    box->static_y_area_height < box->margin[CSS_TOP] + position_outer_height(box) + box->margin[CSS_BOTTOM]) {
		y = box->static_y + box->margin[CSS_TOP];
	} else if (box->static_y_align == CSS_ALIGN_END ||
	    box->static_y_align == CSS_ALIGN_SELF_END) {
		y = box->static_y + box->static_y_area_height -
		    box->margin[CSS_BOTTOM] - position_outer_height(box);
	} else if (box->static_y_align == CSS_ALIGN_CENTER) {
		layout_unit free_height;

		free_height = box->static_y_area_height - box->margin[CSS_TOP] -
		    position_outer_height(box) - box->margin[CSS_BOTTOM];
		if (free_height < 0 && (!box->static_unsafe || box->static_y_safe))
			free_height = 0;
		y = box->static_y + box->margin[CSS_TOP] + free_height / 2;
	} else {
		y = box->static_y + box->margin[CSS_TOP];
	}

	/* The same down. */
	if (box->area_clamp_y && !box->area_scrolls) {
		if (y + position_outer_height(box) + box->margin[CSS_BOTTOM] > box->area_clamp[3]) {
			box->area_overflow = 1;
			y = box->area_clamp[3] - box->margin[CSS_BOTTOM] - position_outer_height(box);
		}
		if (y - box->margin[CSS_TOP] < box->area_clamp[2]) {
			box->area_overflow = 1;
			y = box->area_clamp[2] + box->margin[CSS_TOP];
		}
	}

	/* anchor-center keeps the box inside its containing block down. */
	if (box->anchor_clamp_y) {
		if (y + position_outer_height(box) + box->margin[CSS_BOTTOM] > box->anchor_clamp[3])
			y = box->anchor_clamp[3] - box->margin[CSS_BOTTOM] - position_outer_height(box);
		if (y - box->margin[CSS_TOP] < box->anchor_clamp[2])
			y = box->anchor_clamp[2] + box->margin[CSS_TOP];
	}

	/* The box and its descendants take absolute positions from there. */
	box->x = 0;
	box->y = 0;
	layout_absolute(box, x, y);

	/* Its used insets from the area it was placed in. */
	box->used_inset[CSS_LEFT] = x - box->margin[CSS_LEFT] - area_left;
	box->used_inset[CSS_TOP] = y - box->margin[CSS_TOP] - area_top;
	box->used_inset[CSS_RIGHT] = area_left + area_width - (x + box->width + frame + box->margin[CSS_RIGHT]);
	box->used_inset[CSS_BOTTOM] = area_top + area_height - (y + position_outer_height(box) + box->margin[CSS_BOTTOM]);
	box->used_inset_known = 1;

	/* A box that scrolls with the document makes it tall enough to hold it. */
	bottom_edge = box->y + position_outer_height(box) + box->margin[CSS_BOTTOM];
	if (box->style.position != CSS_POSITION_FIXED && bottom_edge > tree->document_height)
		tree->document_height = bottom_edge;

	/* Succeeded: the box is placed. */
	return 0;
}

/* A self-alignment along the horizontal axis as left, right, center or (the start side) anything else: start and end follow the direction. */
static int
position_physical(
	int alignment,
	int rtl)
{
	if (alignment == CSS_ALIGN_START || alignment == CSS_ALIGN_SELF_START || alignment == CSS_ALIGN_FLEX_START)
		return rtl ? CSS_ALIGN_RIGHT : CSS_ALIGN_LEFT;
	if (alignment == CSS_ALIGN_END || alignment == CSS_ALIGN_SELF_END || alignment == CSS_ALIGN_FLEX_END)
		return rtl ? CSS_ALIGN_LEFT : CSS_ALIGN_RIGHT;
	return alignment;
}

/* Tells whether a self-alignment moves a box that has both insets away from the start (end, center and their relatives). */
static int
position_aligned(
	int alignment)
{
	return alignment == CSS_ALIGN_END || alignment == CSS_ALIGN_CENTER || alignment == CSS_ALIGN_FLEX_END ||
	    alignment == CSS_ALIGN_RIGHT || alignment == CSS_ALIGN_SELF_END;
}

/* Puts a margin box of a size between two edges as an alignment says (the start when it does not fit); returns the border box's start. */
static layout_unit
position_align(
	int alignment,
	layout_unit start,
	layout_unit end,
	layout_unit margin_start,
	layout_unit margin_end,
	layout_unit size)
{
	layout_unit room;

	room = end - start - margin_start - margin_end - size;
	if (room < 0)
		room = 0;
	if (alignment == CSS_ALIGN_CENTER)
		room /= 2;
	return start + margin_start + room;
}

/* Reports a box's padding box as a containing block. */
static void
position_padding_box(
	const struct layout_box *box,
	struct position_block *block)
{
	const struct css_style *style;

	/* Inside the borders, around the paddings. */
	block->x = box->x + box->border[CSS_LEFT];
	block->y = box->y + box->border[CSS_TOP];
	block->width = box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT];
	block->height = box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM];
	style = box->physical_style != NULL ? box->physical_style : &box->style;
	block->mode = position_area_mode(style);
	block->direction = style->direction;
}

/* Resolves an offset (top, right, bottom or left) against a size; zero when it is auto. */
static int
position_offset(
	const struct css_length *length,
	layout_unit size,
	layout_unit *value)
{
	/* Pixels. */
	*value = 0;
	if (length->unit == CSS_UNIT_PX) {
		*value = layout_from_px(length->value);
		return 1;
	}

	/* A percentage of the containing block. */
	if (length->unit == CSS_UNIT_PERCENT) {
		*value = (layout_unit)((float)size * length->value / 100.0f) + layout_from_px(length->offset);
		return 1;
	}

	/* auto. */
	return 0;
}


/* Reports the horizontal borders and paddings of a box. */
static layout_unit
position_frame(
	const struct layout_box *box)
{
	/* The four widths. */
	return box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
}

/* Reports a box's border-box height. */
static layout_unit
position_outer_height(
	const struct layout_box *box)
{
	/* The content, paddings and borders. */
	return box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
}

/*
 * Adds the positioned boxes under a box (not the root) to the entries, in
 * tree order.  An explicit z-index starts a stacking level: positioned
 * descendants keep its outer z-index instead of escaping above or below
 * the level's siblings.
 */
static int
position_collect(
	const struct layout_box *box,
	struct wb_vector *entries,
	int depth,
	int constrained,
	int outer_z)
{
	struct position_entry entry;
	const struct layout_box *child;
	int child_constrained;
	int child_outer_z;
	int child_z;
	int positioned;
	int error;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* Each child: a positioned one is listed at its outer stacking level, then searched. */
	for (child = box->first_child; child != NULL; child = child->next) {
		/* What position-visibility hides stacks nowhere. */
		if (child->anchor_hidden)
			continue;
		positioned = layout_is_layered(child);
		child_z = child->style.z_index;
		if (child->style.z_index_auto)
			child_z = 0;
		child_constrained = constrained;
		child_outer_z = outer_z;
		if (positioned) {
			entry.box = child;
			entry.z = child_z;
			if (constrained)
				entry.z = outer_z;
			error = wb_vector_push(entries, &entry);
			if (error != 0)
				return error;

			/* The first explicit z-index contains all the levels below it. */
			if (!constrained && !child->style.z_index_auto) {
				child_constrained = 1;
				child_outer_z = child_z;
			}
		}

		/* Its descendants. */
		error = position_collect(child, entries, depth + 1, child_constrained, child_outer_z);
		if (error != 0)
			return error;
	}

	/* Succeeded: the boxes are listed. */
	return 0;
}

/*
 * Reports the width the floats among a block's inline content take side by
 * side: the right margin edge of the furthest left float (they were placed
 * relative to the content box), and the margin boxes of the right floats
 * added to it.
 */
static layout_unit
position_inline_floats(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	layout_unit left;
	layout_unit right;
	layout_unit width;
	layout_unit edge;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* A float is measured; an inline box is searched; a box out of the flow or an inline block (its line holds it) is not. */
	left = 0;
	right = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->atomic)
			continue;
		if (child->floating == CSS_FLOAT_NONE) {
			width = position_inline_floats(child, depth + 1);
			if (width > left)
				left = width;
			continue;
		}

		/* A right float adds its margin box; a left one reaches its right margin edge. */
		width = child->margin[CSS_LEFT] + position_frame(child) + child->width + child->margin[CSS_RIGHT];
		if (child->floating == CSS_FLOAT_RIGHT) {
			right += width;
			continue;
		}

		/* A left float, at least as wide as its margin box. */
		edge = child->x + position_frame(child) + child->width + child->margin[CSS_RIGHT];
		if (edge < width)
			edge = width;
		if (edge > left)
			left = edge;
	}

	/* Reports the two sides together. */
	return left + right;
}
