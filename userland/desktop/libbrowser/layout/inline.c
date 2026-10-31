/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Inline layout: a block's inline content (its text boxes, through the
 * inline boxes that hold them) cut into pieces at the break opportunities,
 * whitespace collapsed as white-space asks, and packed greedily into line
 * boxes that are aligned and stacked.
 *
 * An inline replaced box (an <img>) is a piece of its own, as wide as its
 * margin box, standing on the baseline.  So is an inline block
 * (ws074-p060): laid out on its own, shrunk to fit, it stands on its last
 * line's baseline (on its bottom margin edge when it has no line or clips
 * its overflow).  vertical-align then moves each piece: to the line's top
 * or bottom, to the middle, the parent's text top or bottom, the sub- or
 * superscript position, or by a length; the line grows to hold them.
 *
 * The first pass sets every piece on the baseline; inline boxes contribute
 * their style (through the text they hold) but no borders or padding yet.
 * The floats among the content are placed first, at the content's top
 * (not at the line they are on, in this pass), and each line is as wide
 * as the floats beside it leave.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* How many spaces a tab stands for in preserved whitespace. */
#define INLINE_TAB_SPACES	8

/*
 * One piece of inline content: a word (or a run that cannot break), a
 * collapsed space, a forced line break, or a replaced box.  A collapsed
 * space is drawn as one space however much whitespace it stands for.
 */
struct inline_piece {
	struct layout_box *box;
	const uint16_t *text;
	size_t length;
	layout_unit width;
	int space;
	int forced_break;
	int replaced;
	int uses_fallback;
	int edge;
	struct text_font font;
};

/*
 * The state of cutting a block's content into pieces: the block's width
 * (which replaced boxes are sized in), the pieces so far, the word being
 * gathered and whether the last character was collapsed whitespace.
 */
struct inline_cutter {
	struct layout_tree *tree;
	layout_unit width;
	struct wb_vector pieces;
	struct layout_box *box;
	const uint16_t *word;
	size_t word_length;
	layout_unit word_width;
	int word_fallback;
	uint32_t previous;
	int after_space;
	struct text_font font;
	int error;
};

/*
 * How far a piece of a line reaches above and below its own baseline: an
 * atomic piece's margin box, or a text piece's line height.
 */
struct inline_reach {
	layout_unit above;
	layout_unit below;
};

/* The one space a collapsed run of whitespace is drawn as. */
static const uint16_t inline_space[1] = { 0x20U };

static void inline_collect(struct inline_cutter *cutter, struct layout_box *box);
static void inline_add_edge(struct inline_cutter *cutter, struct layout_box *box, int edge);
static int inline_decorations(struct layout_tree *tree, struct layout_line *line, size_t count);
static void inline_cut_text(struct inline_cutter *cutter, struct layout_box *box);
static void inline_add_replaced(struct inline_cutter *cutter, struct layout_box *box);
static void inline_add_atomic(struct inline_cutter *cutter, struct layout_box *box);
static int inline_first_baseline(const struct layout_box *box, layout_unit *baseline, int depth);
static int inline_items_baseline(const struct layout_box *box, layout_unit *baseline, int last, int vertical);
static int inline_is_legend(const struct layout_box *box, const struct layout_box *child);
static int inline_last_baseline(const struct layout_box *box, layout_unit *baseline, int depth);
static void inline_align(struct layout_tree *tree, struct layout_box *box, struct layout_line *line, struct inline_reach *reach, size_t count, layout_unit *above, layout_unit *below);
static layout_unit inline_shift(struct layout_tree *tree, const struct layout_box *box, const struct css_style *style, const struct inline_reach *reach);
static layout_unit inline_x_height(struct layout_tree *tree, const struct css_style *style);
static const struct css_style *inline_align_style(const struct layout_fragment *fragment, const struct layout_box *block);
static void inline_place_atomics(const struct layout_line *line);
static void inline_replaced_extent(const struct layout_box *box, layout_unit *above, layout_unit *below);
static layout_unit inline_margin_height(const struct layout_box *box);
static void inline_flush_word(struct inline_cutter *cutter);
static void inline_add_piece(struct inline_cutter *cutter, const uint16_t *text, size_t length, layout_unit width, int space, int forced_break);
static layout_unit inline_advance(struct inline_cutter *cutter, uint32_t code_point);
static int inline_build_lines(struct layout_tree *tree, struct layout_box *box, const struct inline_piece *pieces, size_t count);
static int inline_finish_line(struct layout_tree *tree, struct layout_box *box, const struct inline_piece *pieces, size_t start, size_t end, layout_unit *cursor, struct wb_vector *lines, int first_line, layout_unit line_left, layout_unit line_width);
static int inline_place_floats(struct layout_tree *tree, struct layout_box *content, struct layout_box *box, int depth, layout_unit *floor);
static void inline_room(struct layout_tree *tree, struct layout_box *box, layout_unit cursor, layout_unit *left, layout_unit *width);
static int inline_wraps(const struct layout_box *box);
static void inline_line_height(struct layout_tree *tree, const struct css_style *style, layout_unit *above, layout_unit *below);
static void inline_font_extent(struct layout_tree *tree, const struct text_font *font, const struct css_style *style, layout_unit *above, layout_unit *below);
static void inline_piece_extent(struct layout_tree *tree, const struct inline_piece *piece, layout_unit *above, layout_unit *below);
static int inline_vertical_absolute_grid(const struct layout_box *box);
static int inline_vertical_simple(const struct layout_box *box, int depth, int mode, int direction);
static layout_unit inline_vertical_room(const struct layout_box *box);

/*
 * Lays out a block's inline content into lines; the block's content
 * height becomes the lines' total height.
 */
int
layout_inline(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct inline_cutter cutter;
	struct layout_box *child;
	const struct inline_piece *pieces;
	layout_unit room;
	layout_unit advance;
	layout_unit floor;
	size_t index;
	int rl_non_grid;
	int rl_fixed_grid;
	int rl_auto_grid;
	int rl_orthogonal;
	int vertical;
	int error;

	/* Bounded text-only vertical blocks map logical lines to physical axes. */
	rl_non_grid = box->style.width.unit == CSS_UNIT_PX &&
	    box->style.height.unit == CSS_UNIT_PX &&
	    box->style.box_sizing == CSS_BOX_SIZING_CONTENT &&
	    (box->parent == NULL ||
	    (box->parent->style.display != CSS_DISPLAY_GRID &&
	    box->parent->style.display != CSS_DISPLAY_INLINE_GRID));
	rl_fixed_grid = box->parent != NULL &&
	    box->parent->style.display == CSS_DISPLAY_INLINE_GRID &&
	    (box->parent->style.writing_mode == CSS_WRITING_VERTICAL_RL ||
	    ((box->parent->style.writing_mode == CSS_WRITING_VERTICAL_LR ||
	    box->parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) &&
	    box->out_of_flow && inline_vertical_absolute_grid(box))) &&
	    box->parent->style.column_count != 0 &&
	    box->parent->style.row_count != 0;
	rl_orthogonal = box->grid_orthogonal && !box->out_of_flow;
	rl_auto_grid = box->parent != NULL &&
	    box->parent->style.display == CSS_DISPLAY_INLINE_GRID &&
	    box->parent->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    box->parent->style.column_count == 0 &&
	    box->parent->style.row_count == 0 &&
	    box->parent->style.width.unit == CSS_UNIT_PX &&
	    box->parent->style.height.unit == CSS_UNIT_PX &&
	    box->style.height.unit == CSS_UNIT_PX;
	vertical = (box->style.writing_mode == CSS_WRITING_VERTICAL_LR ||
	    (box->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    (rl_non_grid || rl_fixed_grid || rl_auto_grid || rl_orthogonal))) &&
	    (box->style.direction == CSS_DIRECTION_LTR ||
	    (box->style.direction == CSS_DIRECTION_RTL && box->out_of_flow &&
	    inline_vertical_absolute_grid(box))) &&
	    (box->style.text_align == CSS_TEXT_ALIGN_START || box->style.text_align == CSS_TEXT_ALIGN_LEFT) &&
	    inline_vertical_simple(box, 0, box->style.writing_mode, box->style.direction);

	/* The floats among the content go to their sides first. */
	floor = 0;
	error = inline_place_floats(tree, box, box, 0, &floor);
	if (error != 0)
		return error;

	/* Cuts the content into pieces. */
	memset(&cutter, 0, sizeof(cutter));
	cutter.tree = tree;
	cutter.width = box->width;
	cutter.after_space = 1;
	wb_vector_init(&cutter.pieces, sizeof(struct inline_piece));
	for (child = box->first_child; child != NULL; child = child->next)
		inline_collect(&cutter, child);
	inline_flush_word(&cutter);
	if (cutter.error != 0) {
		wb_vector_release(&cutter.pieces);
		return cutter.error;
	}

	/* Only lines that fit a definite physical inline room use this unwrapped mapping. */
	if (vertical) {
		room = inline_vertical_room(box);
		advance = 0;
		pieces = cutter.pieces.items;
		for (index = 0; index < cutter.pieces.count; index++) {
			/* An explicit break starts a new line's inline advance. */
			if (pieces[index].forced_break) {
				advance = 0;
				continue;
			}

			/* A run beyond the room needs the established wrapping path. */
			advance += pieces[index].width;
			if (advance > room) {
				vertical = 0;
				break;
			}
		}
	}
	box->vertical_inline = vertical;

	/* Packs the pieces into lines. */
	error = inline_build_lines(tree, box, cutter.pieces.items, cutter.pieces.count);
	wb_vector_release(&cutter.pieces);
	if (error != 0)
		return error;
	if (vertical && box->style.writing_mode == CSS_WRITING_VERTICAL_RL) {
		/* The first block-axis line starts at the physical right edge. */
		for (index = 0; index < box->line_count; index++)
			box->lines[index].y = box->width - box->lines[index].y - box->lines[index].height;
	}

	/* Succeeded: the block has its lines and its height. */
	return 0;
}

/* A direct absolute child can borrow a definite Grid track's inline room. */
static int
inline_vertical_absolute_grid(
	const struct layout_box *box)
{
	const struct layout_box *parent;
	int index;
	int horizontal_lr;
	int horizontal_rl;
	int horizontal_rtl;
	int horizontal_auto;
	int horizontal_auto_rl;
	int framed_child;
	int framed_parent;

	/* Auto or temporarily resolved pixel widths enter positioned fixed-track Grids. */
	parent = box->parent;
	horizontal_lr = 0;
	horizontal_rl = 0;
	horizontal_rtl = 0;
	horizontal_auto = 0;
	horizontal_auto_rl = 0;
	framed_child = 0;
	framed_parent = 0;
	if (parent == NULL || box->style.position != CSS_POSITION_ABSOLUTE ||
	    (box->style.width.unit != CSS_UNIT_AUTO &&
	    box->style.width.unit != CSS_UNIT_PX) ||
	    box->style.height.unit != CSS_UNIT_AUTO ||
	    box->style.box_sizing != CSS_BOX_SIZING_CONTENT ||
	    parent->style.position != CSS_POSITION_RELATIVE ||
	    parent->style.display != CSS_DISPLAY_INLINE_GRID ||
	    (parent->style.direction != CSS_DIRECTION_LTR &&
	    !(parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    parent->style.direction == CSS_DIRECTION_RTL)) ||
	    parent->style.column_count == 0 || parent->style.row_count == 0 ||
	    parent->style.column_gap.value != 0 || parent->style.row_gap.value != 0)
		return 0;
	if (parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) {
		/* A definite horizontal Grid row can bound one vertical absolute run. */
		horizontal_lr = box->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
		    box->style.grid_column.start == 1 &&
		    box->style.grid_column.end == 2 &&
		    box->style.offset[CSS_LEFT].unit == CSS_UNIT_PX &&
		    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO;
		horizontal_rl = box->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
		    box->style.grid_column.start == 2 &&
		    box->style.grid_column.end == 3 &&
		    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_PX &&
		    box->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO;
		horizontal_rtl = box->style.writing_mode ==
		    (parent->style.direction == CSS_DIRECTION_RTL ?
		    CSS_WRITING_VERTICAL_RL : CSS_WRITING_VERTICAL_LR) &&
		    box->style.direction == CSS_DIRECTION_RTL &&
		    box->style.grid_column.start == 2 &&
		    box->style.grid_column.end == 3 &&
		    box->style.grid_row.start == 2 && box->style.grid_row.end == 3 &&
		    box->style.offset[CSS_LEFT].unit == CSS_UNIT_PX &&
		    box->style.offset[CSS_TOP].unit == CSS_UNIT_PX &&
		    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO;
		horizontal_auto = box->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
		    box->style.grid_column.start == 1 &&
		    box->style.grid_column.end == 2 &&
		    box->style.grid_row.start == 1 &&
		    box->style.grid_row.end == 2 &&
		    box->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO;
		horizontal_auto_rl = box->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
		    box->style.grid_column.start == 2 &&
		    box->style.grid_column.end == 3 &&
		    box->style.grid_row.start == 1 &&
		    box->style.grid_row.end == 2 &&
		    box->style.offset[CSS_LEFT].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_RIGHT].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_TOP].unit == CSS_UNIT_AUTO &&
		    box->style.offset[CSS_BOTTOM].unit == CSS_UNIT_AUTO;
		if ((!horizontal_lr && !horizontal_rl && !horizontal_rtl &&
		    !horizontal_auto && !horizontal_auto_rl) ||
		    parent->style.column_count != 2 || parent->style.row_count != 2 ||
		    (!horizontal_rtl && (box->style.direction != parent->style.direction ||
		    box->style.grid_row.start != 1 || box->style.grid_row.end != 2 ||
		    box->style.offset[CSS_TOP].unit != CSS_UNIT_AUTO)) ||
		    box->style.offset[CSS_BOTTOM].unit != CSS_UNIT_AUTO)
			return 0;

		/* A framed vertical child can still use the row's inline room. */
		for (index = 0; index < 4; index++) {
			if (parent->border[index] != 0 || parent->padding[index] != 0)
				framed_parent = 1;
			if (framed_parent && !horizontal_auto && !horizontal_auto_rl)
				return 0;
			if (box->border[index] != 0 || box->padding[index] != 0)
				framed_child = 1;
		}
		if ((horizontal_auto || horizontal_auto_rl) &&
		    (!framed_parent || !framed_child))
			return 0;
		if (framed_child) {
			if (!horizontal_lr && !horizontal_rl && !horizontal_rtl &&
			    !horizontal_auto && !horizontal_auto_rl)
				return 0;
		}
	} else if (parent->style.writing_mode != box->style.writing_mode &&
	    !((parent->style.writing_mode == CSS_WRITING_VERTICAL_LR &&
	    box->style.writing_mode == CSS_WRITING_VERTICAL_RL) ||
	    (parent->style.writing_mode == CSS_WRITING_VERTICAL_RL &&
	    box->style.writing_mode == CSS_WRITING_VERTICAL_LR))) {
		return 0;
	}
	if (parent->style.writing_mode != CSS_WRITING_HORIZONTAL_TB &&
	    parent->style.writing_mode != box->style.writing_mode &&
	    (parent->style.column_count != 2 || parent->style.row_count != 2 ||
	    box->style.direction != CSS_DIRECTION_LTR))
		return 0;
	if (box->style.direction == CSS_DIRECTION_RTL &&
	    !(parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    (horizontal_rtl || (parent->style.direction == CSS_DIRECTION_RTL &&
	    (horizontal_lr || horizontal_rl || horizontal_auto ||
	    horizontal_auto_rl)))) &&
	    (parent->style.writing_mode != box->style.writing_mode ||
	    parent->style.column_count != 2 || parent->style.row_count != 2))
		return 0;

	/* No percentage or flexible track can provide a definite inline room. */
	for (index = 0; index < parent->style.column_count; index++) {
		if (parent->style.columns[index].kind != CSS_TRACK_LENGTH ||
		    parent->style.columns[index].size.unit != CSS_UNIT_PX)
			return 0;
	}
	for (index = 0; index < parent->style.row_count; index++) {
		if (parent->style.rows[index].kind != CSS_TRACK_LENGTH ||
		    parent->style.rows[index].size.unit != CSS_UNIT_PX)
			return 0;
	}

	/* The positioned child can use the bounded sideways text path. */
	return 1;
}

/* Checks whether a vertical line has only text and explicit breaks. */
static int
inline_vertical_simple(
	const struct layout_box *box,
	int depth,
	int mode,
	int direction)
{
	const struct layout_box *child;
	size_t index;

	/* Other formatting contexts and markers retain their established layout. */
	if (depth > LAYOUT_DEPTH_MAX ||
	    (box->out_of_flow && (depth != 0 || !inline_vertical_absolute_grid(box))) ||
	    box->floating != CSS_FLOAT_NONE || box->atomic || box->replaced ||
	    box->marker_length != 0)
		return 0;

	/* Upright orientation and non-Latin shaping need a different glyph path. */
	if (box->style.text_orientation == CSS_TEXT_ORIENTATION_UPRIGHT)
		return 0;
	if (box->kind == LAYOUT_TEXT) {
		for (index = 0; index < box->text_length; index++) {
			if (box->text[index] > 0x7fU)
				return 0;
		}
		return 1;
	}

	/* A line break has no descendants to inspect. */
	if (box->kind == LAYOUT_LINE_BREAK)
		return 1;

	/* Only the root block and inline wrappers can contain these runs. */
	if (depth != 0 && box->kind != LAYOUT_INLINE)
		return 0;

	/* Every descendant must share the root's bounded vertical mapping. */
	if (box->style.writing_mode != mode ||
	    box->style.direction != direction)
		return 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (!inline_vertical_simple(child, depth + 1, mode, direction))
			return 0;
	}

	/* The subtree can use one logical inline axis. */
	return 1;
}

/* Finds definite physical inline room for the bounded vertical-lr line. */
static layout_unit
inline_vertical_room(
	const struct layout_box *box)
{
	const struct layout_box *parent;
	layout_unit room;
	layout_unit track;
	int column;

	/* A grid item's inline room is its row's height; an auto row leaves it unbounded. */
	if (box->grid_orthogonal && box->style.height.unit != CSS_UNIT_PX) {
		if (box->grid_room < 0)
			return LAYOUT_UNIT * 100000;
		room = box->grid_room - box->margin[CSS_TOP] - box->margin[CSS_BOTTOM] -
		    box->border[CSS_TOP] - box->padding[CSS_TOP] -
		    box->padding[CSS_BOTTOM] - box->border[CSS_BOTTOM];
		return room < 0 ? 0 : room;
	}

	/* A physical pixel height gives a non-Grid block its inline room. */
	room = 0;
	if (box->style.height.unit == CSS_UNIT_PX) {
		room = layout_from_px(box->style.height.value);

		/* Border sizing leaves less content room than its declared height. */
		if (box->style.box_sizing == CSS_BOX_SIZING_BORDER)
			room -= box->border[CSS_TOP] + box->padding[CSS_TOP] +
			    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

		/* A negative physical length supplies no inline room. */
		if (room < 0)
			room = 0;
		return room;
	}

	/* A fixed Grid track bounds the child's logical inline axis. */
	parent = box->parent;
	if (parent != NULL && parent->style.writing_mode == CSS_WRITING_HORIZONTAL_TB &&
	    inline_vertical_absolute_grid(box)) {
		/* The child's logical inline axis is the row's physical height. */
		room = layout_from_px(parent->style.rows[box->style.grid_row.start - 1].size.value);
	} else {
		if (parent == NULL || parent->style.display != CSS_DISPLAY_INLINE_GRID ||
		    (parent->style.writing_mode != CSS_WRITING_VERTICAL_LR &&
		    parent->style.writing_mode != CSS_WRITING_VERTICAL_RL) ||
		    parent->style.direction != CSS_DIRECTION_LTR ||
		    parent->style.column_count == 0 || parent->style.column_gap.value != 0)
			return 0;
		for (column = 0; column < parent->style.column_count; column++) {
			/* A non-fixed column cannot guarantee that every child fits. */
			if (parent->style.columns[column].kind != CSS_TRACK_LENGTH ||
			    parent->style.columns[column].size.unit != CSS_UNIT_PX)
				return 0;

			/* The shortest fixed column bounds every simple grid item. */
			track = layout_from_px(parent->style.columns[column].size.value);
			if (column == 0 || track < room)
				room = track;
		}
	}

	/* The item's physical frame and margins reduce that room. */
	room -= box->margin[CSS_TOP] + box->margin[CSS_BOTTOM] +
	    box->border[CSS_TOP] + box->padding[CSS_TOP] +
	    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];

	/* A negative content room cannot hold even an empty run. */
	if (room < 0)
		room = 0;
	return room;
}

/* Cuts an inline-level box's content: text, a line break, or an inline box's children. */
static void
inline_collect(
	struct inline_cutter *cutter,
	struct layout_box *box)
{
	struct layout_box *child;

	/* A box out of the flow, or a float, is no part of the lines. */
	if (box->out_of_flow || box->floating != CSS_FLOAT_NONE)
		return;

	/* Text is cut into words and spaces. */
	if (box->kind == LAYOUT_TEXT) {
		inline_cut_text(cutter, box);
		return;
	}

	/* A replaced box is a piece of its own. */
	if (box->kind == LAYOUT_REPLACED) {
		inline_add_replaced(cutter, box);
		return;
	}

	/* So is an inline block, laid out on its own first. */
	if (box->atomic) {
		inline_add_atomic(cutter, box);
		return;
	}

	/* A line break ends the line. */
	if (box->kind == LAYOUT_LINE_BREAK) {
		inline_flush_word(cutter);
		cutter->box = box;
		inline_add_piece(cutter, NULL, 0, 0, 0, 1);
		cutter->after_space = 1;
		return;
	}

	/* An inline box's children, in order, between the spacers of its horizontal margin, border and padding. */
	if (box->kind == LAYOUT_INLINE)
		inline_add_edge(cutter, box, 1);
	for (child = box->first_child; child != NULL; child = child->next)
		inline_collect(cutter, child);
	if (box->kind == LAYOUT_INLINE)
		inline_add_edge(cutter, box, 2);
}

/* Adds the spacer of an inline box's left (1) or right (2) margin, border and padding: a piece of no text. */
static void
inline_add_edge(
	struct inline_cutter *cutter,
	struct layout_box *box,
	int edge)
{
	struct inline_piece piece;
	layout_unit width;
	int left;
	int error;

	/* The sides are resolved against the block's width. */
	layout_box_model(box, cutter->width);
	left = edge == 1;
	width = left ? box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT] :
	    box->margin[CSS_RIGHT] + box->border[CSS_RIGHT] + box->padding[CSS_RIGHT];
	if (width == 0)
		return;

	/* The word before belongs to the box before. */
	inline_flush_word(cutter);
	memset(&piece, 0, sizeof(piece));
	piece.box = box;
	piece.width = width;
	piece.edge = edge;
	layout_font_of(cutter->tree, &box->style, &piece.font);
	error = wb_vector_push(&cutter->pieces, &piece);
	if (error != 0)
		cutter->error = error;
}

/* Cuts a text box's text into words, collapsed spaces and forced breaks. */
static void
inline_cut_text(
	struct inline_cutter *cutter,
	struct layout_box *box)
{
	struct text_font font;
	uint32_t code_point;
	size_t offset;
	size_t used;
	int font_changed;
	int preserve;
	int wrap_lines;
	int space;
	int opportunity;
	int tab;

	/* The font and the white-space behaviour of the text. */
	layout_font_of(cutter->tree, &box->style, &font);
	preserve = 0;
	if (box->style.white_space == CSS_WHITE_SPACE_PRE || box->style.white_space == CSS_WHITE_SPACE_PRE_WRAP)
		preserve = 1;
	wrap_lines = 1;
	if (box->style.white_space == CSS_WHITE_SPACE_PRE || box->style.white_space == CSS_WHITE_SPACE_NOWRAP)
		wrap_lines = 0;

	/* A new box or font ends the word before it (pieces have one font). */
	font_changed = memcmp(&font, &cutter->font, sizeof(font));
	if (cutter->word != NULL) {
		if (cutter->box != box) {
			/* The word so far belongs to the box before. */
			inline_flush_word(cutter);
		} else if (font_changed != 0) {
			/* The word so far is in another font. */
			inline_flush_word(cutter);
		}
	}

	/* The characters that follow belong to this box and font. */
	cutter->box = box;
	cutter->font = font;

	/* Walks the code points. */
	offset = 0;
	while (offset < box->text_length && cutter->error == 0) {
		used = wb_utf16_decode(box->text + offset, box->text_length - offset, &code_point);
		space = text_is_space(code_point);

		/* A line feed is a forced break where line breaks are kept. */
		if (code_point == 0x0aU && (preserve || box->style.white_space == CSS_WHITE_SPACE_PRE_LINE)) {
			inline_flush_word(cutter);
			inline_add_piece(cutter, NULL, 0, 0, 0, 1);
			cutter->after_space = 1;
			cutter->previous = 0;
			offset += used;
			continue;
		}

		/* Collapsible whitespace becomes one space, and none after another. */
		if (space && !preserve) {
			inline_flush_word(cutter);
			if (!cutter->after_space)
				inline_add_piece(cutter, inline_space, 1, inline_advance(cutter, 0x20U), 1, 0);
			cutter->after_space = 1;
			cutter->previous = 0x20U;
			offset += used;
			continue;
		}

		/* A break opportunity before this character ends the word (only where lines wrap). */
		opportunity = text_break_between(cutter->previous, code_point);
		if (cutter->word != NULL && opportunity && wrap_lines)
			inline_flush_word(cutter);

		/* The character joins the word; a preserved tab is as wide as several spaces. */
		if (cutter->word == NULL) {
			cutter->word = box->text + offset;
			cutter->word_length = 0;
			cutter->word_width = 0;
			cutter->word_fallback = 0;
		}

		/* The character's advance widens the word. */
		cutter->word_length += used;
		tab = 0;
		if (code_point == 0x09U)
			tab = 1;
		if (tab) {
			cutter->word_width += inline_advance(cutter, 0x20U) * INLINE_TAB_SPACES;
		} else {
			cutter->word_width += inline_advance(cutter, code_point);
		}

		/* The next character follows a character that is not collapsed whitespace. */
		cutter->after_space = 0;
		cutter->previous = code_point;
		offset += used;
	}
}

/*
 * Sizes an inline replaced box in the block's width and adds it as a
 * piece as wide as its margin box; the lines may break before and after
 * it.
 */
static void
inline_add_replaced(
	struct inline_cutter *cutter,
	struct layout_box *box)
{
	struct inline_piece *piece;
	layout_unit width;

	/* Its margins, borders, paddings and size. */
	layout_box_model(box, cutter->width);
	layout_replaced_size(box, cutter->width, cutter->tree->containing_height, cutter->tree->containing_height_definite);
	width = box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width +
	    box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] + box->margin[CSS_RIGHT];

	/* The word before it ends, and the box is a piece of its own. */
	inline_flush_word(cutter);
	cutter->box = box;
	inline_add_piece(cutter, NULL, 0, width, 0, 0);
	if (cutter->error != 0)
		return;
	piece = wb_vector_at(&cutter->pieces, cutter->pieces.count - 1U);
	piece->replaced = 1;

	/* What follows is not after a space, and starts a new word. */
	cutter->after_space = 0;
	cutter->previous = 0;
}

/*
 * Lays an inline block out, shrunk to fit the block's width in a
 * formatting context of its own, finds its baseline, and adds it as a
 * piece as wide as its margin box; the lines may break before and after it.
 */
static void
inline_add_atomic(
	struct inline_cutter *cutter,
	struct layout_box *box)
{
	struct inline_piece *piece;
	layout_unit origin_x;
	layout_unit origin_y;
	layout_unit baseline;
	layout_unit width;
	int found;
	int clips;
	int error;

	/* The inline block's own layout, which leaves the origin of the block around it as it was. */
	origin_x = cutter->tree->origin_x;
	origin_y = cutter->tree->origin_y;
	error = layout_shrink_to_fit(cutter->tree, box, cutter->width);
	cutter->tree->origin_x = origin_x;
	cutter->tree->origin_y = origin_y;
	if (error != 0) {
		cutter->error = error;
		return;
	}

	/* Its baseline is its last line's, unless it clips its overflow (then its bottom margin edge stands on the line's). */
	box->has_baseline = 0;
	box->control_baseline = 0;
	clips = layout_clips(box);
	/* An orthogonal inline block has no baseline along the line: its bottom margin edge stands on it. */
	if (box->style.writing_mode != CSS_WRITING_HORIZONTAL_TB && cutter->tree->logical_mode == CSS_WRITING_HORIZONTAL_TB)
		clips = 1;
	if (!clips) {
		/* An inline flex or grid container stands on its first baseline instead. */
		if (box->style.display == CSS_DISPLAY_INLINE_FLEX || box->style.display == CSS_DISPLAY_INLINE_GRID)
			found = inline_first_baseline(box, &baseline, 0);
		else
			found = inline_last_baseline(box, &baseline, 0);
		if (found) {
			box->has_baseline = 1;
			box->control_baseline = baseline;
		}
	}

	/* Its margin box's width. */
	width = box->margin[CSS_LEFT] + box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width +
	    box->padding[CSS_RIGHT] + box->border[CSS_RIGHT] + box->margin[CSS_RIGHT];

	/* The word before it ends, and the box is a piece of its own. */
	inline_flush_word(cutter);
	cutter->box = box;
	inline_add_piece(cutter, NULL, 0, width, 0, 0);
	if (cutter->error != 0)
		return;
	piece = wb_vector_at(&cutter->pieces, cutter->pieces.count - 1U);
	piece->replaced = 1;

	/* What follows is not after a space, and starts a new word. */
	cutter->after_space = 0;
	cutter->previous = 0;
}

/* Tells whether a child is the legend of a fieldset (its baselines are not the fieldset's), or a caption of a table. */
static int
inline_is_legend(
	const struct layout_box *box,
	const struct layout_box *child)
{
	const struct dom_element *element;

	/* A table's captions are outside its rows: its baselines are its rows'. */
	if ((box->style.display == CSS_DISPLAY_TABLE || box->style.display == CSS_DISPLAY_INLINE_TABLE) &&
	    child->style.display == CSS_DISPLAY_TABLE_CAPTION)
		return 1;
	if (box->node == NULL || box->node->type != DOM_ELEMENT || child->node == NULL || child->node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)box->node;
	if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_FIELDSET)
		return 0;
	element = (const struct dom_element *)child->node;
	return element->ns == DOM_NS_HTML && element->tag == DOM_TAG_LEGEND;
}

/*
 * Finds the baseline of a laid out block's last line in the normal flow,
 * searching its last blocks that have one, as a distance below the top of
 * its content box.  Reports whether there is one.
 */
static int
inline_last_baseline(
	const struct layout_box *box,
	layout_unit *baseline,
	int depth)
{
	const struct layout_box *child;
	const struct layout_line *line;
	layout_unit inner;
	int found;
	int any;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* A flex or grid container's: its last row's or line's (when its items know where they were put). */
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID) {
		found = inline_items_baseline(box, baseline, 1, 0);
		if (found >= 0)
			return found;
	}

	/* A block of lines: its last line's baseline. */
	if (box->children_inline) {
		if (box->line_count == 0)
			return 0;
		line = &box->lines[box->line_count - 1U];
		*baseline = line->y + line->baseline;
		return 1;
	}

	/* A block of blocks: the last child in the flow that has a baseline, below where that child's content starts. */
	any = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE)
			continue;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			continue;
		if (inline_is_legend(box, child))
			continue;

		/* The child's own last baseline, moved into this box's content box. */
		found = inline_last_baseline(child, &inner, depth + 1);
		if (found) {
			*baseline = child->y + child->border[CSS_TOP] + child->padding[CSS_TOP] + inner;
			any = 1;
		}
	}

	/* Reports whether any child had one. */
	return any;
}

/*
 * Measures how far an inline replaced box reaches above and below the
 * baseline: an image's whole margin box stands on it; a form control's
 * text's baseline is on it, the rest of its margin box below.
 */
static void
inline_replaced_extent(
	const struct layout_box *box,
	layout_unit *above,
	layout_unit *below)
{
	layout_unit height;

	/* The margin box. */
	height = inline_margin_height(box);

	/* An image, or a control without text, is all above the baseline. */
	if (!box->has_baseline) {
		*above = height;
		*below = 0;
		return;
	}

	/* A control's text baseline is its margin, border and padding above its content, and its text's baseline in it. */
	*above = box->margin[CSS_TOP] + box->border[CSS_TOP] + box->padding[CSS_TOP] + box->control_baseline;
	*below = height - *above;
	if (*below < 0)
		*below = 0;
}

/* Measures a box's margin box height. */
static layout_unit
inline_margin_height(
	const struct layout_box *box)
{
	layout_unit height;

	/* The margins, borders and paddings above and below the content. */
	height = box->margin[CSS_TOP] + box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height +
	    box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM] + box->margin[CSS_BOTTOM];
	return height;
}

/* Ends the word being gathered and adds it as a piece. */
static void
inline_flush_word(
	struct inline_cutter *cutter)
{
	/* Nothing gathered is nothing to add. */
	if (cutter->word == NULL)
		return;

	/* Adds the word. */
	inline_add_piece(cutter, cutter->word, cutter->word_length, cutter->word_width, 0, 0);
	cutter->word = NULL;
	cutter->word_length = 0;
	cutter->word_width = 0;
	cutter->word_fallback = 0;
}

/* Adds a piece of the current box and font. */
static void
inline_add_piece(
	struct inline_cutter *cutter,
	const uint16_t *text,
	size_t length,
	layout_unit width,
	int space,
	int forced_break)
{
	struct inline_piece piece;
	int error;

	/* Fills the piece. */
	memset(&piece, 0, sizeof(piece));
	piece.box = cutter->box;
	piece.text = text;
	piece.length = length;
	piece.width = width;
	piece.space = space;
	piece.forced_break = forced_break;
	piece.font = cutter->font;

	/* A word drawn partly in the fallback face remembers it for the line's height. */
	if (text != NULL && !space)
		piece.uses_fallback = cutter->word_fallback;

	/* Appends it. */
	error = wb_vector_push(&cutter->pieces, &piece);
	if (error != 0)
		cutter->error = error;
}

/* Measures a code point's advance in the current font, in layout units. */
static layout_unit
inline_advance(
	struct inline_cutter *cutter,
	uint32_t code_point)
{
	struct text_glyph glyph;
	int error;

	/* Asks the text system for the glyph. */
	error = text_glyph(cutter->tree->text, &cutter->font, code_point, 0, &glyph);
	if (error != 0) {
		cutter->error = error;
		return 0;
	}

	/* A glyph the font's own face lacks came from the fallback face. */
	if (glyph.face != cutter->font.face)
		cutter->word_fallback = 1;

	/* Reports the advance in layout units. */
	return (layout_unit)glyph.advance_units;
}

/* Tells whether a box's white-space lets lines wrap. */
static int
inline_wraps(
	const struct layout_box *box)
{
	/* pre and nowrap keep a line whole. */
	if (box->style.white_space == CSS_WHITE_SPACE_PRE || box->style.white_space == CSS_WHITE_SPACE_NOWRAP)
		return 0;

	/* The others wrap. */
	return 1;
}

/* Packs pieces into lines greedily and stores the lines in the block. */
static int
inline_build_lines(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct inline_piece *pieces,
	size_t count)
{
	struct wb_vector lines;
	const struct layout_line *line;
	const struct layout_fragment *last;
	layout_unit cursor;
	layout_unit x;
	layout_unit line_left;
	layout_unit line_width;
	layout_unit below;
	layout_unit advance;
	size_t start;
	size_t index;
	int wrap_lines;
	int wraps;
	int error;

	/* Lines wrap unless the block's white-space forbids it. */
	wrap_lines = !box->vertical_inline && inline_wraps(box);

	/* Walks the pieces, ending a line at a forced break or when the next word does not fit. */
	wb_vector_init(&lines, sizeof(struct layout_line));
	cursor = 0;
	start = 0;
	x = 0;
	index = 0;
	inline_room(tree, box, cursor, &line_left, &line_width);
	while (index < count) {
		/* Spaces at the start of a line are dropped. */
		if (pieces[index].space && index == start) {
			start++;
			index++;
			continue;
		}

		/* A word that does not fit beside the floats at a line's start moves the line below the next one to end. */
		if (wrap_lines && index == start && pieces[index].width > line_width && line_width < box->width) {
			below = layout_below_float(tree, cursor);
			if (below > cursor) {
				cursor = below;
				inline_room(tree, box, cursor, &line_left, &line_width);
				continue;
			}
		}

		/* A forced break ends the line with what came before it. */
		if (pieces[index].forced_break) {
			error = inline_finish_line(tree, box, pieces, start, index, &cursor, &lines, lines.count == 0, line_left, line_width);
			if (error != 0) {
				wb_vector_release(&lines);
				return error;
			}

			/* A break that clears floats starts the next line below them. */
			if (pieces[index].box != NULL && pieces[index].box->kind == LAYOUT_LINE_BREAK &&
			    pieces[index].box->style.clear != CSS_FLOAT_NONE) {
				below = layout_clearance(tree, pieces[index].box->style.clear);
				if (below > cursor)
					cursor = below;
			}

			/* The next line starts after the break. */
			index++;
			start = index;
			x = 0;
			inline_room(tree, box, cursor, &line_left, &line_width);
			continue;
		}

		/*
		 * A word that overflows a line that has something ends the line
		 * before it, where the white-space of the piece before (the space
		 * or the text the break follows) lets lines wrap (ws074-p037: a
		 * link with white-space: normal in a nowrap item wraps).
		 */
		wraps = 0;
		if (!box->vertical_inline && index > start)
			wraps = inline_wraps(pieces[index - 1U].box);
		if (wraps && !pieces[index].space && x + pieces[index].width > line_width) {
			error = inline_finish_line(tree, box, pieces, start, index, &cursor, &lines, lines.count == 0, line_left, line_width);
			if (error != 0) {
				wb_vector_release(&lines);
				return error;
			}

			/* The next line starts with the word. */
			start = index;
			x = 0;
			inline_room(tree, box, cursor, &line_left, &line_width);
			continue;
		}

		/* The piece goes on the line. */
		x += pieces[index].width;
		index++;
	}

	/* The last line. */
	if (start < count) {
		error = inline_finish_line(tree, box, pieces, start, count, &cursor, &lines, lines.count == 0, line_left, line_width);
		if (error != 0) {
			wb_vector_release(&lines);
			return error;
		}
	}

	/* Moves the lines into the arena. */
	box->line_count = lines.count;
	box->lines = NULL;
	if (lines.count != 0) {
		box->lines = wb_arena_alloc(&tree->arena, lines.count * sizeof(struct layout_line));
		if (box->lines == NULL) {
			wb_vector_release(&lines);
			return ENOMEM;
		}

		/* Copies the lines out of the growing vector. */
		memcpy(box->lines, lines.items, lines.count * sizeof(struct layout_line));
	}

	/* The lines now live in the arena. */
	wb_vector_release(&lines);

	/* Horizontal lines stack downwards; vertical lines take the longest inline advance. */
	box->height = cursor;
	if (box->vertical_inline) {
		box->height = 0;
		for (index = 0; index < box->line_count; index++) {
			line = &box->lines[index];
			if (line->fragment_count == 0)
				continue;
			last = &line->fragments[line->fragment_count - 1U];
			advance = last->x + last->width;
			if (advance > box->height)
				box->height = advance;
		}
	}

	/* Succeeded: the lines are stored. */
	return 0;
}

/*
 * Makes one line of pieces [start, end): its fragments, their vertical
 * alignment, the line's height, baseline and horizontal alignment, and
 * the places of the inline blocks on it.
 */
static int
inline_finish_line(
	struct layout_tree *tree,
	struct layout_box *box,
	const struct inline_piece *pieces,
	size_t start,
	size_t end,
	layout_unit *cursor,
	struct wb_vector *lines,
	int first_line,
	layout_unit line_left,
	layout_unit line_width)
{
	struct layout_line line;
	struct layout_fragment *fragment;
	struct inline_reach *reach;
	struct text_metrics metrics;
	struct text_font font;
	struct text_glyph glyph;
	struct css_length saved_line_height;
	layout_unit above;
	layout_unit below;
	layout_unit x;
	layout_unit room;
	size_t count;
	size_t piece_count;
	size_t index;
	int align;
	int error;

	/* Trailing spaces hang past the line and are dropped. */
	while (end > start && pieces[end - 1U].space)
		end--;

	/* The strut: the block's own font and line height (the first line's are the ::first-line's). */
	inline_line_height(tree, first_line && box->first_line_style != NULL ? box->first_line_style : &box->style, &above, &below);

	/* Counts the fragments: every piece but the forced breaks. */
	memset(&line, 0, sizeof(line));
	count = 0;
	for (index = start; index < end; index++) {
		if (!pieces[index].forced_break)
			count++;
	}

	/* A list item's first line holds its marker too. */
	if (first_line)
		count++;

	/* Allocates the fragments of a line that has any. */
	reach = NULL;
	if (count != 0) {
		line.fragments = wb_arena_zalloc(&tree->arena, count * sizeof(struct layout_fragment));
		if (line.fragments == NULL)
			return ENOMEM;

		/* And how far each reaches, which the alignment reads (the marker reaches nowhere). */
		reach = calloc(count, sizeof(*reach));
		if (reach == NULL)
			return ENOMEM;
	}

	/* Sets one fragment per piece along the line, and how far it reaches above and below its baseline. */
	x = 0;
	for (index = start; index < end; index++) {
		/* A forced break has no fragment. */
		if (pieces[index].forced_break)
			continue;

		/* The fragment's text, font and place. */
		fragment = &line.fragments[line.fragment_count];
		line.fragment_count++;
		fragment->box = pieces[index].box;
		fragment->text = pieces[index].text;
		fragment->length = pieces[index].length;
		fragment->font = pieces[index].font;
		fragment->color = pieces[index].box->style.paint_color;
		if (first_line && box->first_line_style != NULL && pieces[index].box->style.paint_color == box->style.paint_color)
			fragment->color = box->first_line_style->paint_color;
		fragment->underline = pieces[index].box->style.underline && pieces[index].edge == 0;
		fragment->edge = pieces[index].edge;
		fragment->x = x;
		fragment->width = pieces[index].width;

		/*
		 * A replaced box stands on the baseline: all of its margin box is above
		 * it, unless it is a control with text, whose text's baseline is the
		 * line's.
		 */
		if (pieces[index].replaced) {
			inline_replaced_extent(pieces[index].box, &fragment->ascent, &fragment->descent);

			/*
			 * In a vertical line of upright-or-mixed text the central baseline is
			 * the one that aligns: a replaced box is centered on the strut's middle.
			 */
			if (box->physical_style != NULL && box->physical_style->writing_mode != CSS_WRITING_HORIZONTAL_TB &&
			    box->physical_style->writing_sideways == 0 && !pieces[index].box->has_baseline &&
			    pieces[index].box->kind == LAYOUT_REPLACED && pieces[index].box->control == DOM_CONTROL_NONE) {
				layout_unit whole;

				whole = fragment->ascent + fragment->descent;
				fragment->ascent = whole / 2 + (above - below) / 2;
				fragment->descent = whole - fragment->ascent;
			}
			reach[line.fragment_count - 1U].above = fragment->ascent;
			reach[line.fragment_count - 1U].below = fragment->descent;
			x += pieces[index].width;
			continue;
		}

		/* The font's ascent and descent, which the painting places the glyphs by. */
		error = text_font_metrics(tree->text, &pieces[index].font, &metrics);
		if (error != 0) {
			free(reach);
			return error;
		}

		/* In layout units. */
		fragment->ascent = (layout_unit)metrics.ascent * LAYOUT_UNIT;
		fragment->descent = (layout_unit)metrics.descent * LAYOUT_UNIT;

		/* The piece's line reaches above and below its baseline (the first line's inherited line height is the ::first-line's). */
		if (first_line && box->first_line_style != NULL &&
		    pieces[index].box->style.line_height.unit == box->style.line_height.unit &&
		    pieces[index].box->style.line_height.value == box->style.line_height.value) {
			saved_line_height = pieces[index].box->style.line_height;
			pieces[index].box->style.line_height = box->first_line_style->line_height;
			inline_piece_extent(tree, &pieces[index], &reach[line.fragment_count - 1U].above, &reach[line.fragment_count - 1U].below);
			pieces[index].box->style.line_height = saved_line_height;
		} else {
			inline_piece_extent(tree, &pieces[index], &reach[line.fragment_count - 1U].above, &reach[line.fragment_count - 1U].below);
		}

		/* The next piece starts where this one ends. */
		x += pieces[index].width;
	}

	/* The pieces' fragments come before the marker's. */
	piece_count = line.fragment_count;

	/* A list item's marker hangs to the left of its first line. */
	if (first_line && box->marker_length != 0) {
		fragment = &line.fragments[line.fragment_count];
		line.fragment_count++;
		layout_font_of(tree, &box->style, &font);
		fragment->box = box;
		fragment->text = box->marker;
		fragment->length = box->marker_length;
		fragment->font = font;
		fragment->color = box->style.paint_color;
		fragment->width = 0;

		/* The marker is as wide as its characters. */
		for (index = 0; index < box->marker_length; index++) {
			error = text_glyph(tree->text, &font, box->marker[index], 0, &glyph);
			if (error != 0) {
				free(reach);
				return error;
			}

			/* The character widens the marker. */
			fragment->width += glyph.advance_units;
		}

		/* It ends a space's width before the content's left edge. */
		error = text_glyph(tree->text, &font, 0x20U, 0, &glyph);
		if (error != 0) {
			free(reach);
			return error;
		}

		/* Its right edge is that space before the content. */
		fragment->x = -(fragment->width + glyph.advance_units);

		/* An inside marker starts the line, and the content follows it. */
		if (box->style.list_style_inside) {
			layout_unit inside_shift;

			inside_shift = fragment->width + glyph.advance_units;
			for (index = 0; index < piece_count; index++)
				line.fragments[index].x += inside_shift;
			fragment->x = 0;
			x += inside_shift;
		}

		/* The font's ascent and descent, which the painting places the glyphs by. */
		error = text_font_metrics(tree->text, &font, &metrics);
		if (error != 0) {
			free(reach);
			return error;
		}

		/* In layout units. */
		fragment->ascent = (layout_unit)metrics.ascent * LAYOUT_UNIT;
		fragment->descent = (layout_unit)metrics.descent * LAYOUT_UNIT;
	}

	/* vertical-align places the pieces, and the line grows to hold them over the strut. */
	inline_align(tree, box, &line, reach, piece_count, &above, &below);
	free(reach);

	/* The inline boxes with a background, borders or padding take their share of the line. */
	error = inline_decorations(tree, &line, piece_count);
	if (error != 0)
		return error;

	/* Aligns the line in the room the floats leave it: start and end are the right and the left in a right-to-left block. */
	room = line_width - x;
	line.left = line_left;
	align = box->style.text_align;
	if (align == CSS_TEXT_ALIGN_START || align == CSS_TEXT_ALIGN_JUSTIFY) {
		align = CSS_TEXT_ALIGN_LEFT;
		if (box->style.direction == CSS_DIRECTION_RTL)
			align = CSS_TEXT_ALIGN_RIGHT;
	} else if (align == CSS_TEXT_ALIGN_END) {
		align = CSS_TEXT_ALIGN_RIGHT;
		if (box->style.direction == CSS_DIRECTION_RTL)
			align = CSS_TEXT_ALIGN_LEFT;
	}

	/* The room goes half to each side, or all to the left. */
	if (room > 0 && align == CSS_TEXT_ALIGN_CENTER)
		line.left = line_left + room / 2;
	if (room > 0 && align == CSS_TEXT_ALIGN_RIGHT)
		line.left = line_left + room;

	/* Stacks the line under the ones before. */
	line.y = *cursor;
	line.height = above + below;
	line.baseline = above;
	*cursor += line.height;

	/* The inline blocks on the line now know where they are. */
	inline_place_atomics(&line);

	/* Adds it. */
	error = wb_vector_push(lines, &line);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the line is made. */
	return 0;
}

/*
 * Aligns the pieces of a line vertically (vertical-align): each piece's
 * shift from the line's baseline, and how far the line reaches above and
 * below its baseline (above and below come in as the strut's).  The
 * pieces aligned with the baseline come first; a piece aligned with the
 * line's top or bottom then makes the line tall enough for it and is put
 * at that edge.
 */
static void
inline_align(
	struct layout_tree *tree,
	struct layout_box *box,
	struct layout_line *line,
	struct inline_reach *reach,
	size_t count,
	layout_unit *above,
	layout_unit *below)
{
	struct layout_fragment *fragment;
	const struct css_style *style;
	layout_unit top_height;
	layout_unit bottom_height;
	layout_unit height;
	size_t index;
	int align;

	/* The pieces placed from the baseline raise the line; those at its top or bottom wait. */
	top_height = 0;
	bottom_height = 0;
	for (index = 0; index < count; index++) {
		fragment = &line->fragments[index];
		height = reach[index].above + reach[index].below;

		/* The piece's alignment: the baseline, unless a style around it says otherwise. */
		align = CSS_VALIGN_BASELINE;
		style = inline_align_style(fragment, box);
		if (style != NULL)
			align = style->vertical_align;

		/* A piece at the line's top or bottom needs the line at least as tall as itself. */
		if (align == CSS_VALIGN_TOP) {
			if (height > top_height)
				top_height = height;
			continue;
		}

		/* Likewise a piece at its bottom. */
		if (align == CSS_VALIGN_BOTTOM) {
			if (height > bottom_height)
				bottom_height = height;
			continue;
		}

		/* The shift from the baseline, and how far the shifted piece reaches. */
		fragment->shift = 0;
		if (style != NULL)
			fragment->shift = inline_shift(tree, box, style, &reach[index]);
		if (reach[index].above - fragment->shift > *above)
			*above = reach[index].above - fragment->shift;
		if (reach[index].below + fragment->shift > *below)
			*below = reach[index].below + fragment->shift;
	}

	/* A piece at the top grows the line downwards, one at the bottom upwards. */
	if (top_height > *above + *below)
		*below = top_height - *above;
	if (bottom_height > *above + *below)
		*above = bottom_height - *below;

	/* The pieces at the top and the bottom go to those edges. */
	for (index = 0; index < count; index++) {
		fragment = &line->fragments[index];
		style = inline_align_style(fragment, box);
		if (style == NULL)
			continue;

		/* Its top on the line's top, or its bottom on the line's bottom. */
		if (style->vertical_align == CSS_VALIGN_TOP)
			fragment->shift = reach[index].above - *above;
		if (style->vertical_align == CSS_VALIGN_BOTTOM)
			fragment->shift = *below - reach[index].below;
	}
}

/*
 * Picks the style whose vertical-align moves a fragment, or NULL for the
 * baseline: an atomic piece's own; for text, that of the nearest inline
 * box around it that leaves the baseline (text straight in the block
 * stays on the baseline, since vertical-align does not apply to blocks).
 */
static const struct css_style *
inline_align_style(
	const struct layout_fragment *fragment,
	const struct layout_box *block)
{
	const struct layout_box *ancestor;

	/* A replaced box or an inline block is aligned by its own style. */
	if (fragment->box->kind != LAYOUT_TEXT) {
		if (fragment->box->style.vertical_align == CSS_VALIGN_BASELINE)
			return NULL;
		return &fragment->box->style;
	}

	/* Text: the inline boxes around it, up to the block. */
	for (ancestor = fragment->box->parent; ancestor != NULL && ancestor != block; ancestor = ancestor->parent) {
		if (ancestor->kind != LAYOUT_INLINE)
			break;
		if (ancestor->style.vertical_align != CSS_VALIGN_BASELINE)
			return &ancestor->style;
	}

	/* No inline box around the text leaves the baseline. */
	return NULL;
}

/*
 * Finds how far a piece's baseline sits below the line's (negative: above)
 * for vertical-align relative to the parent, which is the block holding
 * the line in this pass.
 */
static layout_unit
inline_shift(
	struct layout_tree *tree,
	const struct layout_box *box,
	const struct css_style *style,
	const struct inline_reach *reach)
{
	struct text_metrics metrics;
	struct text_font font;
	layout_unit x_height;
	layout_unit line_above;
	layout_unit line_below;
	layout_unit shift;
	int error;

	/* Each keyword moves the piece its own way. */
	shift = 0;
	switch (style->vertical_align) {
	case CSS_VALIGN_MIDDLE:
		/* The piece's middle goes to half the parent's x-height above the baseline. */
		x_height = inline_x_height(tree, &box->style);
		shift = (reach->above - reach->below) / 2 - x_height / 2;
		break;
	case CSS_VALIGN_TEXT_TOP:
	case CSS_VALIGN_TEXT_BOTTOM:
		/* The parent's font's ascent and descent are its text's top and bottom. */
		layout_font_of(tree, &box->style, &font);
		error = text_font_metrics(tree->text, &font, &metrics);
		if (error != 0)
			break;

		/* The piece's top goes to the text's top, or its bottom to the text's bottom. */
		if (style->vertical_align == CSS_VALIGN_TEXT_TOP) {
			shift = reach->above - (layout_unit)metrics.ascent * LAYOUT_UNIT;
		} else {
			shift = (layout_unit)metrics.descent * LAYOUT_UNIT - reach->below;
		}

		/* The shift is found. */
		break;
	case CSS_VALIGN_SUB:
		/* Lowered by a fifth of the parent's font size, as Chromium does. */
		shift = layout_from_px(box->style.font_size / 5.0f + 1.0f);
		break;
	case CSS_VALIGN_SUPER:
		/* Raised by a third of the parent's font size, as Chromium does. */
		shift = -layout_from_px(box->style.font_size / 3.0f + 1.0f);
		break;
	case CSS_VALIGN_LENGTH:
		/* Raised by a length, or by a percentage of the piece's own line height. */
		if (style->vertical_offset.unit == CSS_UNIT_PX) {
			shift = -layout_from_px(style->vertical_offset.value);
		} else if (style->vertical_offset.unit == CSS_UNIT_PERCENT) {
			inline_line_height(tree, style, &line_above, &line_below);
			shift = -(layout_unit)((float)(line_above + line_below) * style->vertical_offset.value / 100.0f);
		}

		/* The shift is found. */
		break;
	default:
		break;
	}

	/* Reports the shift. */
	return shift;
}

/*
 * Measures a style's font's x-height: how far the top of an "x" is above
 * the baseline, or half the font size when the glyph cannot be drawn.
 */
static layout_unit
inline_x_height(
	struct layout_tree *tree,
	const struct css_style *style)
{
	struct text_font font;
	struct text_glyph glyph;
	int error;

	/* The drawn "x" of the style's font. */
	layout_font_of(tree, style, &font);
	error = text_glyph(tree->text, &font, 'x', 1, &glyph);
	if (error != 0 || glyph.top <= 0)
		return layout_from_px(style->font_size / 2.0f);

	/* Its top above the baseline. */
	return (layout_unit)glyph.top * LAYOUT_UNIT;
}

/*
 * Places the inline blocks of a stacked line: each border box relative to
 * the block's content box, from the line's left and its piece's baseline.
 */
static void
inline_place_atomics(
	const struct layout_line *line)
{
	const struct layout_fragment *fragment;
	struct layout_box *atomic;
	size_t index;

	/* Each inline block's margin box starts at its piece, its top its reach above the piece's baseline. */
	for (index = 0; index < line->fragment_count; index++) {
		fragment = &line->fragments[index];
		atomic = fragment->box;
		if (!atomic->atomic)
			continue;
		atomic->x = line->left + fragment->x + atomic->margin[CSS_LEFT];
		atomic->y = line->y + line->baseline + fragment->shift - fragment->ascent + atomic->margin[CSS_TOP];
	}
}

/* Measures how far a style's line reaches above and below the baseline, half-leading included. */
static void
inline_line_height(
	struct layout_tree *tree,
	const struct css_style *style,
	layout_unit *above,
	layout_unit *below)
{
	struct text_font font;

	/* The style's own font under the style's line-height. */
	layout_font_of(tree, style, &font);
	inline_font_extent(tree, &font, style, above, below);
}

/*
 * Measures how far a piece's line reaches above and below the baseline.
 *
 * With line-height: normal, a word drawn partly in the fallback face also
 * reaches as far as that face's own line does, as in Chromium, where every
 * font a run uses counts toward a normal line's height.
 */
static void
inline_piece_extent(
	struct layout_tree *tree,
	const struct inline_piece *piece,
	layout_unit *above,
	layout_unit *below)
{
	struct text_font fallback;
	layout_unit fallback_above;
	layout_unit fallback_below;

	/* The line of the piece's own style. */
	inline_line_height(tree, &piece->box->style, above, below);

	/* A given line height is not raised by the fonts used. */
	if (piece->box->style.line_height.unit != CSS_UNIT_NORMAL)
		return;

	/* A piece in its own face alone needs nothing more. */
	if (!piece->uses_fallback)
		return;

	/* The fallback face at the piece's size raises the extent where it reaches further. */
	fallback = piece->font;
	fallback.face = TEXT_FACE_FALLBACK;
	inline_font_extent(tree, &fallback, &piece->box->style, &fallback_above, &fallback_below);
	if (fallback_above > *above)
		*above = fallback_above;
	if (fallback_below > *below)
		*below = fallback_below;
}

/* Measures how far a font's line reaches above and below the baseline under a style's line-height. */
static void
inline_font_extent(
	struct layout_tree *tree,
	const struct text_font *font,
	const struct css_style *style,
	layout_unit *above,
	layout_unit *below)
{
	struct text_metrics metrics;
	layout_unit content;
	layout_unit height;
	layout_unit leading;
	int error;

	/* A zero-sized font has no ascent or descent, even if rasterization uses a minimum glyph size. */
	if (style->font_size == 0.0f) {
		memset(&metrics, 0, sizeof(metrics));
		error = 0;
	} else {
		error = text_font_metrics(tree->text, font, &metrics);
	}
	if (error != 0) {
		metrics.ascent = (int)style->font_size;
		metrics.descent = 0;
		metrics.line_height = (int)style->font_size;
	}

	/* The glyphs' own height, from the ascent to the descent. */
	content = (layout_unit)(metrics.ascent + metrics.descent) * LAYOUT_UNIT;

	/* The used line height: normal, a multiple of the font size, or a length. */
	height = (layout_unit)metrics.line_height * LAYOUT_UNIT;
	if (style->line_height.unit == CSS_UNIT_NUMBER)
		height = layout_from_px(style->line_height.value * style->font_size);
	if (style->line_height.unit == CSS_UNIT_PX)
		height = layout_from_px(style->line_height.value);

	/* Half the leading goes above, half below. */
	leading = height - content;
	*above = (layout_unit)metrics.ascent * LAYOUT_UNIT + leading / 2;
	*below = height - *above;
}

/*
 * Lays out and places the floats among a block's inline content (not inside
 * boxes out of the flow, inline blocks or other floats); a line break that
 * clears floats keeps the floats after it below them (floor).
 */
static int
inline_place_floats(
	struct layout_tree *tree,
	struct layout_box *content,
	struct layout_box *box,
	int depth,
	layout_unit *floor)
{
	layout_unit clearance;

	struct layout_box *child;
	layout_unit origin_x;
	layout_unit origin_y;
	int error;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* Each float shrinks to fit and goes to its side at the content's top; inline boxes are searched through. */
	origin_x = tree->origin_x;
	origin_y = tree->origin_y;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->atomic)
			continue;
		if (child->kind == LAYOUT_LINE_BREAK && child->style.clear != CSS_FLOAT_NONE) {
			clearance = layout_clearance(tree, child->style.clear);
			if (clearance > *floor)
				*floor = clearance;
			continue;
		}
		if (child->floating == CSS_FLOAT_NONE) {
			error = inline_place_floats(tree, content, child, depth + 1, floor);
			if (error != 0)
				return error;
			continue;
		}

		/* The float's own layout, then its place. */
		error = layout_shrink_to_fit(tree, child, content->width);
		tree->origin_x = origin_x;
		tree->origin_y = origin_y;
		if (error == 0)
			error = layout_place_float(tree, child, *floor, content->width);
		if (error != 0)
			return error;
	}

	/* Succeeded: the floats are placed. */
	return 0;
}

/* Finds the room of a line starting at cursor: its left edge and width between the floats beside a line's height. */
static void
inline_room(
	struct layout_tree *tree,
	struct layout_box *box,
	layout_unit cursor,
	layout_unit *left,
	layout_unit *width)
{
	layout_unit above;
	layout_unit below;
	layout_unit right;

	/* The band of a line of the block's own font. */
	inline_line_height(tree, &box->style, &above, &below);
	layout_line_room(tree, cursor, cursor + above + below, box->width, left, &right);

	/* The width between the edges. */
	*width = right - *left;
	if (*width < 0)
		*width = 0;
}

/* Tells whether an inline box paints something of its own: a background, a border or padding. */
static int
inline_decorated(
	const struct layout_box *box)
{
	int side;

	if ((box->style.background_color >> 24) != 0 || box->background != NULL)
		return 1;
	for (side = 0; side < 4; side++) {
		if (box->border[side] != 0 || box->padding[side] != 0)
			return 1;
	}
	return 0;
}

/*
 * Finds, for a laid-out line, what each decorated inline box on it covers: the
 * span of its fragments (its left and right edges when the line holds them),
 * and how far they reach above and below the baseline.
 */
static int
inline_decorations(
	struct layout_tree *tree,
	struct layout_line *line,
	size_t count)
{
	struct layout_decoration *list;
	struct layout_decoration *found;
	struct layout_fragment *fragment;
	struct layout_box *node;
	layout_unit start;
	layout_unit end;
	layout_unit up;
	layout_unit down;
	size_t used;
	size_t capacity;
	size_t index;
	size_t at;
	int decorated;

	/* A line of no inline boxes with decoration has none. */
	list = NULL;
	used = 0;
	capacity = 0;
	for (index = 0; index < count; index++) {
		fragment = &line->fragments[index];
		node = fragment->box;
		if (node->kind != LAYOUT_INLINE)
			node = node->parent;
		for (; node != NULL && node->kind == LAYOUT_INLINE; node = node->parent) {
			decorated = inline_decorated(node);
			if (!decorated)
				continue;

			/* The box's record on this line, made at its first fragment. */
			found = NULL;
			for (at = 0; at < used; at++) {
				if (list[at].box == node)
					found = &list[at];
			}
			if (found == NULL) {
				if (used == capacity) {
					capacity = capacity == 0 ? 4U : capacity * 2U;
					found = realloc(list, capacity * sizeof(*list));
					if (found == NULL) {
						free(list);
						return ENOMEM;
					}
					list = found;
				}
				found = &list[used++];
				memset(found, 0, sizeof(*found));
				found->box = node;
				found->x = fragment->x;
				found->width = fragment->width;
				found->shift = fragment->shift;
			}

			/* Its span grows to hold the fragment, and its reach with it. */
			start = found->x;
			end = found->x + found->width;
			if (fragment->x < start)
				start = fragment->x;
			if (fragment->x + fragment->width > end)
				end = fragment->x + fragment->width;
			found->x = start;
			found->width = end - start;
			up = fragment->ascent;
			down = fragment->descent;
			if (fragment->edge == 0 || fragment->box != node) {
				if (up > found->ascent)
					found->ascent = up;
				if (down > found->descent)
					found->descent = down;
			} else if (found->ascent == 0 && found->descent == 0) {
				found->ascent = up;
				found->descent = down;
			}
			if (fragment->box == node && fragment->edge == 1)
				found->has_left = 1;
			if (fragment->box == node && fragment->edge == 2)
				found->has_right = 1;
		}
	}

	/* An outer box is painted before the boxes inside it: the records go by the depth of their boxes. */
	for (index = 1; index < used; index++) {
		struct layout_decoration moving;
		size_t depth_moving;
		size_t depth_before;
		const struct layout_box *walker;

		moving = list[index];
		depth_moving = 0;
		for (walker = moving.box->parent; walker != NULL; walker = walker->parent)
			depth_moving++;
		at = index;
		while (at > 0) {
			depth_before = 0;
			for (walker = list[at - 1U].box->parent; walker != NULL; walker = walker->parent)
				depth_before++;
			if (depth_before <= depth_moving)
				break;
			list[at] = list[at - 1U];
			at--;
		}
		list[at] = moving;
	}

	/* The records move into the arena with the line. */
	line->decorations = NULL;
	line->decoration_count = 0;
	if (used != 0) {
		line->decorations = wb_arena_alloc(&tree->arena, used * sizeof(*list));
		if (line->decorations == NULL) {
			free(list);
			return ENOMEM;
		}
		memcpy(line->decorations, list, used * sizeof(*list));
		line->decoration_count = used;
	}
	free(list);
	return 0;
}

/*
 * Finds a flex or grid container's first (or last) baseline below its content box's
 * top: the baseline its first (last) row's or line's baseline-aligned items
 * share, else the one of its startmost (endmost) item of that row, which a flex
 * container takes as the one at the physical start (a reversed direction or a right to left row
 * puts the start at the other end) (synthesized from its border
 * box's bottom when it has none).  Reports 1 when found, 0 when the
 * container has no item, and -1 when its items do not know their rows.
 */
static int
inline_items_baseline(
	const struct layout_box *box,
	layout_unit *baseline,
	int last,
	int vertical)
{
	const struct layout_box *child;
	const struct layout_box *best;
	layout_unit inner;
	int aligned;
	int best_aligned;
	int wanted_row;
	int flex;
	int row;
	int reversed;
	int reversed_lines;
	int last_line;
	int last_item;
	int better;
	int any;
	int align;
	int item_last;
	int best_last;

	/*
	 * Where a flex container's start is: its lines and its items run the other way when its direction (or a right to
	 * left row, or a wrapped column's lines) is reversed.  The first baseline is the one of the line at the physical
	 * start and the item at its start; the last baseline the one at the other ends.
	 */
	flex = box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX;
	reversed = 0;
	reversed_lines = 0;
	if (flex) {
		row = box->style.flex_direction == CSS_FLEX_ROW || box->style.flex_direction == CSS_FLEX_ROW_REVERSE;
		reversed = box->style.flex_direction == CSS_FLEX_ROW_REVERSE || box->style.flex_direction == CSS_FLEX_COLUMN_REVERSE;
		reversed_lines = box->style.flex_wrap == CSS_FLEX_WRAP_REVERSE;
		if (row && box->style.direction == CSS_DIRECTION_RTL)
			reversed = !reversed;
		if (!row && box->style.direction == CSS_DIRECTION_RTL)
			reversed_lines = !reversed_lines;
	}
	last_line = last != reversed_lines;
	last_item = last != reversed;

	/* The line that holds an item at that end. */
	wanted_row = 0;
	any = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow)
			continue;
		any = 1;
		if (child->item_row > 0 && (wanted_row == 0 || (last_line ? child->item_row > wanted_row : child->item_row < wanted_row)))
			wanted_row = child->item_row;
	}

	/* Its items, one aligned by the baseline asked for first, then one aligned by the other baseline (a grid's), then the one at that end. */
	best = NULL;
	best_aligned = 0;
	best_last = last != 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || wanted_row == 0 || child->item_row != wanted_row)
			continue;
		align = child->style.align_self;
		item_last = (child->style.last_baselines & 8u) != 0;
		if (align == CSS_ALIGN_AUTO) {
			align = box->style.align_items;
			item_last = (box->style.last_baselines & 4u) != 0;
		}
		/* A last baseline is stored as the end alignment with its flag. */
		aligned = 0;
		if (align == CSS_ALIGN_BASELINE || (item_last && align == CSS_ALIGN_END))
			aligned = item_last == (last != 0) ? 2 : (child->item_row_span > 1 ? 0 : 1);
		if (flex && last && item_last && align != CSS_ALIGN_BASELINE)
			aligned = 2;
		better = best == NULL || (last_item ? child->item_column > best->item_column : child->item_column < best->item_column);
		if (best == NULL || aligned > best_aligned || (aligned == best_aligned && better)) {
			best = child;
			best_aligned = aligned;
			best_last = aligned == 1 ? item_last : last != 0;
		}
	}
	if (best == NULL)
		return any ? -1 : 0;

	/* In a vertical container's logical frame the central baseline is a distance below the content box's top. */
	if (vertical == 2) {
		if (!layout_logical_central_baseline(best, best_last, &inner))
			inner = (best->border[CSS_TOP] + best->padding[CSS_TOP] + best->height + best->padding[CSS_BOTTOM] +
			    best->border[CSS_BOTTOM]) / 2;
		*baseline = best->y + inner;
		return 1;
	}

	/* In a vertical container the central baseline is a distance right of the content box's left edge. */
	if (vertical) {
		if (best->style.writing_mode != box->style.writing_mode || !layout_vertical_baseline(best, best_last, &inner))
			inner = best->border[CSS_LEFT] + best->padding[CSS_LEFT] + best->width + best->padding[CSS_RIGHT] +
			    best->border[CSS_RIGHT];
		*baseline = best->x + inner;
		return 1;
	}

	/* Its baseline, or its border box's bottom (an orthogonal item's always), below the container's content box's top. */
	if (best->style.writing_mode != box->style.writing_mode ||
	    !(best_last ? layout_last_baseline(best, &inner) : layout_first_baseline(best, &inner)))
		inner = best->border[CSS_TOP] + best->padding[CSS_TOP] + best->height + best->padding[CSS_BOTTOM] +
		    best->border[CSS_BOTTOM];
	*baseline = best->y + inner;
	return 1;
}

/* Finds a laid out box's first baseline below its content box's top (a control's text, or its first line in the flow). */
static int
inline_first_baseline(
	const struct layout_box *box,
	layout_unit *baseline,
	int depth)
{
	const struct layout_box *child;
	layout_unit inner;
	int found;

	/* Stops at the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* A form control's text. */
	if (box->control != DOM_CONTROL_NONE) {
		if (!box->has_baseline)
			return 0;
		*baseline = box->control_baseline;
		return 1;
	}

	/* A flex or grid container's: its first row's or line's (when its items know where they were put). */
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID) {
		found = inline_items_baseline(box, baseline, 0, 0);
		if (found >= 0)
			return found;
	}

	/* A block of lines: its first line's baseline. */
	if (box->children_inline) {
		if (box->line_count == 0)
			return 0;
		*baseline = box->lines[0].y + box->lines[0].baseline;
		return 1;
	}

	/* A block of blocks: the first child in the flow that has one, below where that child's content starts. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE || child->replaced)
			continue;
		if (inline_is_legend(box, child))
			continue;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			continue;
		if (inline_first_baseline(child, &inner, depth + 1)) {
			*baseline = child->y + child->border[CSS_TOP] + child->padding[CSS_TOP] + inner;
			return 1;
		}
	}

	/* None. */
	return 0;
}

/*
 * Finds a laid out box's first baseline as a distance below the top of its
 * border box.  Reports whether it has one (a replaced box or a box without
 * lines has none).
 */
int
layout_first_baseline(
	const struct layout_box *box,
	layout_unit *baseline)
{
	layout_unit inner;

	/* A replaced box has no baseline of its own. */
	if (box->replaced)
		return 0;
	if (!inline_first_baseline(box, &inner, 0))
		return 0;

	/* Below the border and the padding, and no further than a box that scrolls reaches. */
	*baseline = box->border[CSS_TOP] + box->padding[CSS_TOP] + inner;
	if (layout_clips(box)) {
		layout_unit extent;

		extent = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (*baseline < 0)
			*baseline = 0;
		if (*baseline > extent)
			*baseline = extent;
	}
	return 1;
}

/*
 * Finds the first (or last) line's central baseline of a vertical box as a
 * distance right of its border box's left edge.  Reports whether it has one.
 */
static int inline_vertical_baseline(const struct layout_box *box, int last, layout_unit *offset);
static int inline_logical_central(const struct layout_box *box, int last, layout_unit *offset);

/* The central baseline of a vertical box as a distance right of its border box's left edge, no further than a box that scrolls reaches. */
int
layout_vertical_baseline(
	const struct layout_box *box,
	int last,
	layout_unit *offset)
{
	layout_unit extent;

	if (!inline_vertical_baseline(box, last, offset))
		return 0;
	if (layout_clips(box)) {
		extent = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
		if (*offset < 0)
			*offset = 0;
		if (*offset > extent)
			*offset = extent;
	}
	return 1;
}

static int
inline_vertical_baseline(
	const struct layout_box *box,
	int last,
	layout_unit *offset)
{
	const struct layout_box *child;
	const struct layout_line *line;
	layout_unit centre;
	layout_unit inner;
	int found;

	/* A replaced box has no baseline. */
	if (box->replaced)
		return 0;

	/* A flex or grid container's: its items' (when they know where they were put). */
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID) {
		found = inline_items_baseline(box, &inner, last, 1);
		if (found > 0) {
			*offset = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + inner;
			return 1;
		}
		if (found == 0)
			return 0;
	}

	/* A block of lines: the line's centre (a vertical box's lines keep their physical offset from the content box's left edge). */
	if (box->children_inline) {
		if (box->line_count == 0)
			return 0;
		line = last ? &box->lines[box->line_count - 1] : &box->lines[0];
		centre = line->y + line->height / 2;
		*offset = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + centre;
		return 1;
	}

	/* A block of blocks: the first (or last) child in the flow that has one. */
	found = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE || child->replaced)
			continue;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			continue;
		if (inline_is_legend(box, child))
			continue;
		if (!layout_vertical_baseline(child, last, &inner))
			continue;
		*offset = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + child->x + inner;
		found = 1;
		if (!last)
			break;
	}
	return found;
}

/*
 * Finds the first (or last) line's central baseline of a box laid out in its writing mode's logical frame
 * (a vertical box's grid layout) as a distance below the top of its border box.  Reports whether it has one.
 */
int
layout_logical_central_baseline(
	const struct layout_box *box,
	int last,
	layout_unit *offset)
{
	layout_unit extent;

	if (!inline_logical_central(box, last, offset))
		return 0;
	if (layout_clips(box)) {
		extent = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (*offset < 0)
			*offset = 0;
		if (*offset > extent)
			*offset = extent;
	}
	return 1;
}

static int
inline_logical_central(
	const struct layout_box *box,
	int last,
	layout_unit *offset)
{
	const struct layout_box *child;
	const struct layout_line *line;
	layout_unit inner;
	int found;

	if (box->replaced)
		return 0;

	/* A flex or grid container's: its items'. */
	if (box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID) {
		found = inline_items_baseline(box, &inner, last, 2);
		if (found > 0) {
			*offset = box->border[CSS_TOP] + box->padding[CSS_TOP] + inner;
			return 1;
		}
		if (found == 0)
			return 0;
	}
	if (box->children_inline) {
		if (box->line_count == 0)
			return 0;
		line = last ? &box->lines[box->line_count - 1] : &box->lines[0];
		*offset = box->border[CSS_TOP] + box->padding[CSS_TOP] + line->y + line->height / 2;
		return 1;
	}
	found = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE || child->replaced)
			continue;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			continue;
		if (inline_is_legend(box, child))
			continue;
		if (!layout_logical_central_baseline(child, last, &inner))
			continue;
		*offset = box->border[CSS_TOP] + box->padding[CSS_TOP] + child->y + inner;
		found = 1;
		if (!last)
			break;
	}
	return found;
}

/*
 * Finds a laid out box's last baseline as a distance below the top of its
 * border box.  Reports whether it has one.
 */
int
layout_last_baseline(
	const struct layout_box *box,
	layout_unit *baseline)
{
	layout_unit inner;

	/* A replaced box has no baseline of its own. */
	if (box->replaced)
		return 0;
	if (!inline_last_baseline(box, &inner, 0))
		return 0;

	/* Below the border and the padding, and no further than a box that scrolls reaches. */
	*baseline = box->border[CSS_TOP] + box->padding[CSS_TOP] + inner;
	if (layout_clips(box)) {
		layout_unit extent;

		extent = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
		if (*baseline < 0)
			*baseline = 0;
		if (*baseline > extent)
			*baseline = extent;
	}
	return 1;
}
