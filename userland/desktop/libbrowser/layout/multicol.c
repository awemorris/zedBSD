/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Multiple columns (https://drafts.csswg.org/css-multicol-1/): a block
 * container with column-count or column-width lays its content out one
 * column wide as a single flow, and the flow is then cut into columns.
 *
 * The cut works on leaves, the pieces the flow can be cut between: the
 * lines of a block with inline content, and the blocks that cannot be
 * split (replaced, avoid breaks, tables, flex and grid containers, empty
 * blocks).  The blocks around the leaves are not split: each is moved
 * with its first leaf and keeps the height of the part in its first
 * column.  A box with column-span: all breaks the flow into segments,
 * each cut into columns of its own, with the box across all of them.
 */

#include "layout/layout.h"

/* The deepest tree the walks follow. */
#define MULTICOL_DEPTH	64

/* The most layout units a column can be (no limit). */
#define MULTICOL_UNLIMITED	0x3fffffff

/* A piece of the flow: where it is in the flow, what is forced around it, and where the cut puts it. */
struct multicol_leaf {
	struct layout_box *box;
	struct layout_line *line;
	layout_unit top;
	layout_unit bottom;
	layout_unit gap;
	int forced_before;
	int forced_after;
	int splittable;
	int split;
	int segment;
	int column;
	layout_unit target;
	layout_unit dx;
	layout_unit dy;
	/* A leaf that was cut between columns: its pieces (from piece_first, piece_count of them) in the pieces' list. */
	size_t piece_first;
	size_t piece_count;
};

/* One part of a leaf cut between columns: its column and where it is in the columns, and its height. */
struct multicol_piece {
	int column;
	layout_unit top;
	layout_unit height;
	layout_unit dx;
	layout_unit abs_top;
};

/* One run of leaves between column-span: all boxes, and where it lies in the flow and in the result. */
struct multicol_segment {
	size_t first;
	size_t count;
	layout_unit flow_top;
	layout_unit target_top;
	layout_unit height;
};

static int multicol_leaf_block(const struct layout_box *box);
static int multicol_forced(int value);
static int multicol_splittable(const struct layout_box *box);
static int multicol_avoids(int value);
static int multicol_in_flow(const struct layout_box *box);
static layout_unit multicol_outer(const struct layout_box *box);
static int multicol_collect_child(struct layout_box *child, layout_unit origin, int segment, struct wb_vector *leaves, int depth);
static int multicol_collect(struct layout_box *box, layout_unit origin, int segment, struct wb_vector *leaves, int depth);
static int multicol_pack(struct multicol_leaf *leaves, size_t count, size_t first_index, layout_unit limit, layout_unit start_gap, struct wb_vector *pieces, int *columns, layout_unit *extent);
static layout_unit multicol_limit(struct multicol_leaf *leaves, size_t count, layout_unit start_gap, int wanted, layout_unit definite, int balance);
static void multicol_apply(struct layout_tree *tree, struct layout_box *box, const struct multicol_leaf *leaves, const struct multicol_piece *pieces, size_t count, size_t *next, layout_unit parent_dx, layout_unit parent_dy, layout_unit origin, int container, int depth);
static void multicol_cut_block(struct layout_tree *tree, struct layout_box *box, const struct multicol_leaf *leaf, const struct multicol_piece *pieces, layout_unit parent_dx, layout_unit parent_dy);
static void multicol_move_line(struct layout_line *line, layout_unit dx, layout_unit dy);

/* Tells whether a block is a multicol container: columns asked for, and a block that lays its content out as a flow. */
int
layout_multicol_wanted(
	const struct layout_box *box)
{
	if (box->kind != LAYOUT_BLOCK || box->replaced || box->parent == NULL)
		return 0;
	if (box->style.multicol_count <= 0 && box->style.multicol_width <= 0.0f)
		return 0;
	return box->style.display == CSS_DISPLAY_BLOCK || box->style.display == CSS_DISPLAY_INLINE_BLOCK ||
	    box->style.display == CSS_DISPLAY_LIST_ITEM;
}

/*
 * Works out the columns of a container whose content is width wide: how many,
 * how wide, and the gap between them (column-gap's normal is 1em).
 */
void
layout_multicol_geometry(
	const struct layout_box *box,
	layout_unit width,
	int *count,
	layout_unit *column,
	layout_unit *gap)
{
	layout_unit room;
	layout_unit span;
	int wanted;
	int fit;

	if (box->style.column_gap.unit == CSS_UNIT_PX)
		*gap = layout_from_px(box->style.column_gap.value);
	else if (box->style.column_gap.unit == CSS_UNIT_PERCENT)
		*gap = (layout_unit)((float)width * box->style.column_gap.value / 100.0f);
	else
		*gap = layout_from_px(box->style.font_size);
	if (*gap < 0)
		*gap = 0;

	/* column-width asks for as many columns that wide as fit, up to column-count when it is given. */
	wanted = box->style.multicol_count;
	if (box->style.multicol_width > 0.0f) {
		span = layout_from_px(box->style.multicol_width);
		if (span < 1)
			span = 1;
		fit = (int)((width + *gap) / (span + *gap));
		if (fit < 1)
			fit = 1;
		if (wanted <= 0 || fit < wanted)
			wanted = fit;
	}
	if (wanted < 1)
		wanted = 1;
	room = width - (layout_unit)(wanted - 1) * *gap;
	if (room < 0)
		room = 0;
	*count = wanted;
	*column = room / wanted;
}

/* Tells whether a block is a piece that is not cut inside. */
static int
multicol_leaf_block(
	const struct layout_box *box)
{
	const struct layout_box *child;

	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return 1;
	if (box->replaced || box->atomic || multicol_avoids(box->style.break_inside))
		return 1;
	switch (box->style.display) {
	case CSS_DISPLAY_TABLE:
	case CSS_DISPLAY_INLINE_TABLE:
	case CSS_DISPLAY_FLEX:
	case CSS_DISPLAY_INLINE_FLEX:
	case CSS_DISPLAY_GRID:
	case CSS_DISPLAY_INLINE_GRID:
	case CSS_DISPLAY_INLINE_BLOCK:
		return 1;
	default:
		break;
	}
	if (box->control != 0)
		return 1;
	if (box->children_inline)
		return box->line_count == 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (multicol_in_flow(child))
			return 0;
	}
	return 1;
}

/* Tells whether a leaf block is one a real fragmentation cuts (an empty block with a height), whole as it is here. */
static int
multicol_splittable(
	const struct layout_box *box)
{
	return (box->kind == LAYOUT_BLOCK || box->kind == LAYOUT_ANONYMOUS_BLOCK) && !box->replaced && !box->atomic &&
	    !multicol_avoids(box->style.break_inside) && box->control == 0 && box->style.display == CSS_DISPLAY_BLOCK &&
	    box->first_child == NULL && box->line_count == 0;
}

/* Tells whether a break value forces a break in a column (always, all and column). */
static int
multicol_forced(
	int value)
{
	return value == 2 || value == 3 || value == 10;
}

/* Tells whether a break-inside value avoids a break inside the box. */
static int
multicol_avoids(
	int value)
{
	return value == 1 || value == 4 || value == 11 || value == 13;
}

/* Reports a box's border-box height. */
static layout_unit
multicol_outer(
	const struct layout_box *box)
{
	return box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
}

/* Tells whether a box is in the flow that the columns cut (not a float, not out of the flow). */
static int
multicol_in_flow(
	const struct layout_box *box)
{
	return !box->out_of_flow && box->floating == CSS_FLOAT_NONE && !box->continuation;
}

/* Adds the leaves of one child block of a container (flow coordinates are from the container's content box). */
static int
multicol_collect_child(
	struct layout_box *child,
	layout_unit origin,
	int segment,
	struct wb_vector *leaves,
	int depth)
{
	struct multicol_leaf leaf;
	size_t first;
	size_t count;
	layout_unit top;
	int error;

	top = origin + child->y;
	first = leaves->count;
	if (multicol_leaf_block(child)) {
		memset(&leaf, 0, sizeof(leaf));
		leaf.box = child;
		leaf.top = top;
		leaf.bottom = top + multicol_outer(child);
		leaf.segment = segment;
		leaf.splittable = multicol_splittable(child);
		error = wb_vector_push(leaves, &leaf);
	} else {
		error = multicol_collect(child, top + child->border[CSS_TOP] + child->padding[CSS_TOP], segment, leaves, depth + 1);
	}
	if (error != 0)
		return error;
	count = leaves->count - first;
	if (count == 0)
		return 0;

	/* A break asked for before the box is before its first piece, one after it after its last. */
	if (multicol_forced(child->style.break_before))
		((struct multicol_leaf *)wb_vector_at(leaves, first))->forced_before = 1;
	if (multicol_forced(child->style.break_after))
		((struct multicol_leaf *)wb_vector_at(leaves, first + count - 1U))->forced_after = 1;
	return 0;
}

/* Adds the leaves of a block's content, in flow order. */
static int
multicol_collect(
	struct layout_box *box,
	layout_unit origin,
	int segment,
	struct wb_vector *leaves,
	int depth)
{
	struct multicol_leaf leaf;
	struct layout_box *child;
	size_t index;
	int error;

	if (depth > MULTICOL_DEPTH)
		return 0;
	if (box->children_inline) {
		for (index = 0; index < box->line_count; index++) {
			memset(&leaf, 0, sizeof(leaf));
			leaf.box = box;
			leaf.line = &box->lines[index];
			leaf.top = origin + box->lines[index].y;
			leaf.bottom = leaf.top + box->lines[index].height;
			leaf.segment = segment;
			error = wb_vector_push(leaves, &leaf);
			if (error != 0)
				return error;
		}
		return 0;
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!multicol_in_flow(child))
			continue;
		error = multicol_collect_child(child, origin, segment, leaves, depth);
		if (error != 0)
			return error;
	}
	return 0;
}

/*
 * Puts leaves into columns of a height, the way the content flows: a leaf
 * that does not fit below the one before it starts the next column (unless
 * it is the first in its column), and a forced break starts one too.  A
 * leaf that is empty (splittable) is cut where the column ends instead.
 * With pieces set the leaves get their column and their place in it (from
 * the top of the columns), and the parts of a cut leaf are added to pieces
 * (first_index is the index of the first leaf in the whole list); the
 * number of columns used and how far down the lowest leaf reaches are
 * returned.  Returns 0, or an errno value.
 */
static int
multicol_pack(
	struct multicol_leaf *leaves,
	size_t count,
	size_t first_index,
	layout_unit limit,
	layout_unit start_gap,
	struct wb_vector *pieces,
	int *columns,
	layout_unit *extent)
{
	struct multicol_piece piece;
	layout_unit bottom;
	layout_unit top;
	layout_unit height;
	layout_unit deepest;
	layout_unit room;
	layout_unit part;
	size_t index;
	int column;
	int first;
	int forced;
	int error;

	column = 0;
	bottom = 0;
	deepest = 0;
	first = 1;
	(void)first_index;
	for (index = 0; index < count; index++) {
		height = leaves[index].bottom - leaves[index].top;
		forced = index > 0 && (leaves[index].forced_before || leaves[index - 1U].forced_after);
		if (forced && !first) {
			column++;
			first = 1;
		}
		if (first)
			top = index == 0 ? start_gap : (forced ? leaves[index].gap : 0);
		else
			top = bottom + leaves[index].gap;

		/* An empty block is cut where the column ends (or at the column's height when it is the first and too tall). */
		if (leaves[index].splittable && limit < MULTICOL_UNLIMITED && limit > 0 && top + height > limit &&
		    (first ? height > limit : limit - top > 0)) {
			leaves[index].piece_first = pieces != NULL ? pieces->count : 0;
			leaves[index].piece_count = 0;
			room = height;
			if (pieces == NULL) {
				/* Only the count of columns matters: the whole columns between are worked out, not walked. */
				part = limit - top;
				room -= part;
				if (top + part > deepest)
					deepest = top + part;
				column += 1 + (int)((room - 1) / limit);
				bottom = room - ((room - 1) / limit) * limit;
				if (room > limit && limit > deepest)
					deepest = limit;
				if (bottom > deepest)
					deepest = bottom;
				first = 0;
				continue;
			}
			for (;;) {
				part = limit - top;
				if (part > room)
					part = room;
				if (pieces != NULL) {
					piece.column = column;
					piece.top = top;
					piece.height = part;
					piece.dx = 0;
					piece.abs_top = 0;
					error = wb_vector_push(pieces, &piece);
					if (error != 0)
						return error;
					leaves[index].piece_count++;
				}
				room -= part;
				if (top + part > deepest)
					deepest = top + part;
				if (room <= 0) {
					bottom = top + part;
					break;
				}
				column++;
				top = 0;
				first = 1;
			}
			if (pieces != NULL) {
				leaves[index].column = column;
				leaves[index].target = ((struct multicol_piece *)wb_vector_at(pieces, leaves[index].piece_first))->top;
				leaves[index].column = ((struct multicol_piece *)wb_vector_at(pieces, leaves[index].piece_first))->column;
			}
			first = 0;
			continue;
		}
		if (!first && top + height > limit) {
			column++;
			first = 1;
			top = 0;
		}
		bottom = top + height;
		if (bottom > deepest)
			deepest = bottom;
		if (pieces != NULL) {
			leaves[index].column = column;
			leaves[index].target = top;
			leaves[index].piece_count = 0;
		}
		first = 0;
	}
	*columns = column + 1;
	*extent = deepest;
	return 0;
}

/*
 * Works out the column height for a run of leaves: the definite height
 * given, or none (the flow is as tall as it is, cut by forced breaks only)
 * with column-fill: auto; when balanced, the least height that needs no
 * more columns than there are (or the most there is when none will do).
 */
static layout_unit
multicol_limit(
	struct multicol_leaf *leaves,
	size_t count,
	layout_unit start_gap,
	int wanted,
	layout_unit definite,
	int balance)
{
	layout_unit low;
	layout_unit high;
	layout_unit middle;
	layout_unit extent;
	int columns;

	if (!balance)
		return definite >= 0 ? definite : MULTICOL_UNLIMITED;
	multicol_pack(leaves, count, 0, MULTICOL_UNLIMITED, start_gap, NULL, &columns, &extent);
	high = extent;
	if (definite >= 0 && high > definite)
		high = definite;
	multicol_pack(leaves, count, 0, high, start_gap, NULL, &columns, &extent);
	if (columns > wanted)
		return high;
	low = 0;
	while (low < high) {
		middle = low + (high - low) / 2;
		multicol_pack(leaves, count, 0, middle, start_gap, NULL, &columns, &extent);
		if (columns <= wanted)
			high = middle;
		else
			low = middle + 1;
	}
	return high;
}

/* Moves one line of a block with the inline blocks standing in it. */
static void
multicol_move_line(
	struct layout_line *line,
	layout_unit dx,
	layout_unit dy)
{
	size_t index;

	line->y += dy;
	line->left += dx;
	for (index = 0; index < line->fragment_count; index++) {
		if (line->fragments[index].box != NULL && line->fragments[index].box->atomic &&
		    line->fragments[index].box->kind == LAYOUT_BLOCK) {
			line->fragments[index].box->x += dx;
			line->fragments[index].box->y += dy;
		}
	}
}

/*
 * Cuts an empty block between the columns its pieces are in: the box stays
 * as the first part and a copy of it (a continuation, with nothing inside)
 * follows for each other part.  The borders and the padding are where the
 * block starts and ends, not at each cut.
 */
static void
multicol_cut_block(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct multicol_leaf *leaf,
	const struct multicol_piece *pieces,
	layout_unit parent_dx,
	layout_unit parent_dy)
{
	const struct multicol_piece *piece;
	struct layout_box *clone;
	struct layout_box *after;
	layout_unit old_x;
	layout_unit old_y;
	layout_unit top_edge;
	layout_unit bottom_edge;
	size_t index;

	old_x = box->x;
	old_y = box->y;
	top_edge = box->border[CSS_TOP] + box->padding[CSS_TOP];
	bottom_edge = box->border[CSS_BOTTOM] + box->padding[CSS_BOTTOM];
	after = box;
	for (index = 0; index < leaf->piece_count; index++) {
		piece = &pieces[leaf->piece_first + index];
		if (index == 0) {
			clone = box;
		} else {
			clone = wb_arena_alloc(&tree->arena, sizeof(*clone));
			if (clone == NULL)
				return;
			*clone = *box;
			clone->continuation = 1;
			clone->first_child = NULL;
			clone->last_child = NULL;
			clone->lines = NULL;
			clone->line_count = 0;
			clone->next = after->next;
			after->next = clone;
			if (box->parent->last_child == after)
				box->parent->last_child = clone;
			after = clone;
			clone->border[CSS_TOP] = 0;
			clone->padding[CSS_TOP] = 0;
			clone->margin[CSS_TOP] = 0;
			clone->border[CSS_BOTTOM] = box->border[CSS_BOTTOM];
			clone->padding[CSS_BOTTOM] = box->padding[CSS_BOTTOM];
		}
		clone->x = old_x + piece->dx - parent_dx;
		clone->y = old_y + (piece->abs_top - leaf->top) - parent_dy;
		clone->fragmented = 1;

		/* Where the block goes on in a later part, it has no bottom edge here, and where it comes from, no top edge. */
		if (index + 1U < leaf->piece_count) {
			clone->border[CSS_BOTTOM] = 0;
			clone->padding[CSS_BOTTOM] = 0;
			clone->margin[CSS_BOTTOM] = 0;
		}
		clone->height = piece->height - (index == 0 ? top_edge : 0) - (index + 1U == leaf->piece_count ? bottom_edge : 0);
		if (clone->height < 0)
			clone->height = 0;
	}
}

/*
 * Moves the boxes of a block's content to where the cut put their leaves
 * (the walk visits them in the order they were collected): a leaf by what
 * its place needs; a block that was not a leaf with its first leaf, and its
 * height becomes what its first column holds of it.  parent_dx and
 * parent_dy are how far the block's parent has been moved already, and
 * origin is where the block's content box is in the flow.
 */
static void
multicol_apply(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct multicol_leaf *leaves,
	const struct multicol_piece *pieces,
	size_t count,
	size_t *next,
	layout_unit parent_dx,
	layout_unit parent_dy,
	layout_unit origin,
	int container,
	int depth)
{
	struct layout_box *child;
	const struct multicol_leaf *leaf;
	layout_unit content;
	layout_unit reach;
	size_t index;
	size_t first;
	size_t last;
	int split;

	if (depth > MULTICOL_DEPTH)
		return;
	if (box->children_inline) {
		for (index = 0; index < box->line_count && *next < count; index++) {
			leaf = &leaves[*next];
			(*next)++;
			multicol_move_line(&box->lines[index], leaf->dx - parent_dx, leaf->dy - parent_dy);
		}
		return;
	}
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!multicol_in_flow(child))
			continue;
		if (container && child->style.multicol_span == 1)
			continue;
		first = *next;
		if (first >= count)
			return;
		leaf = &leaves[first];
		content = origin + child->y + child->border[CSS_TOP] + child->padding[CSS_TOP];
		if (multicol_leaf_block(child)) {
			(*next)++;
			if (leaf->piece_count > 1U) {
				multicol_cut_block(tree, child, leaf, pieces, parent_dx, parent_dy);
				continue;
			}
			child->x += leaf->dx - parent_dx;
			child->y += leaf->dy - parent_dy;
			if (leaf->split)
				child->fragmented = 1;
			continue;
		}

		/* The block moves with its first piece; what is inside it follows, its own pieces by what they need. */
		child->x += leaf->dx - parent_dx;
		child->y += leaf->dy - parent_dy;
		multicol_apply(tree, child, leaves, pieces, count, next, leaf->dx, leaf->dy, content, 0, depth + 1);
		if (*next == first)
			continue;
		last = *next - 1U;
		split = 0;
		for (index = first; index <= last; index++) {
			if (leaves[index].column != leaves[first].column || leaves[index].segment != leaves[first].segment) {
				split = 1;
				break;
			}
		}
		if (!split) {
			child->height += leaves[last].dy - leaves[first].dy;
			continue;
		}
		child->fragmented = 1;

		/* A block cut between columns keeps what lies in its first one. */
		reach = 0;
		for (index = first; index <= last; index++) {
			if (leaves[index].column != leaves[first].column || leaves[index].segment != leaves[first].segment)
				break;
			reach = leaves[index].bottom + leaves[index].dy;
		}
		child->height = reach - (content + leaf->dy);
		if (child->height < 0)
			child->height = 0;
	}
}

/*
 * Cuts the flow of a container, laid out one column wide, into its columns
 * (width is the container's content width, columns how many there are and
 * column how wide each is).  The container's height becomes the columns'
 * when it has none of its own.
 *
 * Returns 0, or an errno value.
 */
int
layout_multicol_cut(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit width,
	int columns,
	layout_unit column,
	layout_unit gap)
{
	struct wb_vector leaves;
	struct wb_vector segments;
	struct wb_vector pieces;
	size_t pieces_before;
	struct multicol_segment segment;
	struct multicol_segment *made;
	struct multicol_leaf *list;
	struct layout_box *child;
	layout_unit definite;
	layout_unit limit;
	layout_unit cursor;
	layout_unit flow_bottom;
	layout_unit original_y;
	layout_unit original_outer;
	layout_unit pitch;
	layout_unit start_gap;
	layout_unit saved_x;
	layout_unit saved_y;
	size_t index;
	size_t at;
	size_t next;
	int used;
	int balance;
	int mirror;
	int spanned;
	int error;

	wb_vector_init(&leaves, sizeof(struct multicol_leaf));
	wb_vector_init(&segments, sizeof(struct multicol_segment));
	wb_vector_init(&pieces, sizeof(struct multicol_piece));
	pitch = column + gap;
	mirror = box->style.direction == CSS_DIRECTION_RTL;
	definite = tree->containing_height_definite ? tree->containing_height : -1;
	error = 0;

	/* The leaves, in runs: a box that spans all the columns ends one and starts the next. */
	memset(&segment, 0, sizeof(segment));
	if (box->children_inline) {
		error = multicol_collect(box, 0, 0, &leaves, 0);
		segment.count = leaves.count;
		if (error == 0)
			error = wb_vector_push(&segments, &segment);
	} else {
		for (child = box->first_child; child != NULL && error == 0; child = child->next) {
			if (!multicol_in_flow(child))
				continue;
			if (child->style.multicol_span == 1) {
				error = wb_vector_push(&segments, &segment);
				segment.first = leaves.count;
				segment.count = 0;
				segment.flow_top = child->y + multicol_outer(child);
				continue;
			}
			error = multicol_collect_child(child, 0, (int)segments.count, &leaves, 0);
			segment.count = leaves.count - segment.first;
		}
		if (error == 0)
			error = wb_vector_push(&segments, &segment);
	}
	if (error != 0)
		goto done;
	list = leaves.count > 0 ? wb_vector_at(&leaves, 0) : NULL;

	/* How far each leaf is from the one before it in the flow. */
	for (index = 0; index < segments.count; index++) {
		made = wb_vector_at(&segments, index);
		for (at = 1; at < made->count; at++)
			list[made->first + at].gap = list[made->first + at].top - list[made->first + at - 1U].bottom;
	}

	/* Each run is cut, then what spans the columns after it is laid out across them. */
	cursor = 0;
	spanned = 0;
	for (index = 0; index < segments.count; index++) {
		made = wb_vector_at(&segments, index);
		made->target_top = cursor;
		pieces_before = pieces.count;
		start_gap = made->count > 0 ? list[made->first].top - made->flow_top : 0;
		if (start_gap < 0)
			start_gap = 0;
		balance = box->style.multicol_fill != 1 || index + 1U < segments.count;
		limit = multicol_limit(list + made->first, made->count, start_gap, columns,
		    index + 1U == segments.count && definite >= 0 ? definite - cursor : -1, balance);
		error = multicol_pack(list + made->first, made->count, made->first, limit, start_gap, &pieces, &used, &made->height);
		if (error != 0)
			goto done;
		for (at = 0; at < made->count; at++) {
			struct multicol_leaf *leaf = &list[made->first + at];
			size_t piece_at;

			leaf->dx = (mirror ? (width - column) - (layout_unit)leaf->column * pitch : (layout_unit)leaf->column * pitch);
			leaf->dy = cursor + leaf->target - leaf->top;
			for (piece_at = 0; piece_at < leaf->piece_count; piece_at++) {
				struct multicol_piece *piece = wb_vector_at(&pieces, leaf->piece_first + piece_at);

				piece->dx = (mirror ? (width - column) - (layout_unit)piece->column * pitch : (layout_unit)piece->column * pitch);
				piece->abs_top = cursor + piece->top;
			}
		}
		(void)pieces_before;
		cursor += made->height;
		flow_bottom = made->count > 0 ? list[made->first + made->count - 1U].bottom : made->flow_top;

		/* The spanning box that ends the run (the next run starts after it). */
		if (index + 1U < segments.count) {
			next = 0;
			for (child = box->first_child; child != NULL; child = child->next) {
				if (!multicol_in_flow(child) || child->style.multicol_span != 1)
					continue;
				if (next++ != (size_t)spanned)
					continue;
				original_y = child->y;
				original_outer = multicol_outer(child);
				saved_x = tree->origin_x;
				saved_y = tree->origin_y;
				tree->origin_y += cursor;
				error = layout_block(tree, child, width);
				tree->origin_x = saved_x;
				tree->origin_y = saved_y;
				if (error != 0)
					goto done;
				child->x = child->margin[CSS_LEFT];
				child->y = cursor + (original_y - flow_bottom);
				cursor = child->y + multicol_outer(child);
				((struct multicol_segment *)wb_vector_at(&segments, index + 1U))->flow_top = original_y + original_outer;
				break;
			}
			spanned++;
		}
	}

	/* The boxes go where the cut put them. */
	next = 0;
	multicol_apply(tree, box, list, (const struct multicol_piece *)(pieces.count > 0 ? wb_vector_at(&pieces, 0) : NULL), leaves.count, &next, 0, 0, 0, 1, 0);
	if (definite < 0)
		box->height = cursor;

done:
	wb_vector_release(&pieces);
	wb_vector_release(&segments);
	wb_vector_release(&leaves);
	return error;
}
