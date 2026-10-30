/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Layout (plan/ws074/design.md §6): the box tree built from the DOM and its
 * computed styles, block and inline formatting, and the positions the
 * painting reads.
 *
 * Positions are in layout units, 1/64 of a pixel, so lengths add up without
 * the drift of floating point.  Normal flow stacks block boxes with their
 * margins collapsing and breaks inline content into line boxes; then
 * relatively positioned boxes are shifted, and absolutely positioned and
 * fixed boxes, taken out of the flow, are placed in their containing
 * blocks (position.c).  Floats, tables, flex and grid come later; until
 * then their boxes are laid out as blocks.  An <img> is a replaced box
 * (replaced.c): inline, an atomic piece of its line; otherwise a block
 * without content; either sized by its image and its width and height.
 * A form control (<input>, <textarea>, <select>) is a replaced box too,
 * sized by its text and attributes and standing on its text's baseline
 * (control.c).  An inline block (ws074-p060) is a block laid out on its
 * own and placed as one atomic piece of its line, standing on its last
 * line's baseline; vertical-align moves the pieces of a line up and down.
 */

#ifndef KEILAND_BROWSER_LAYOUT_H
#define KEILAND_BROWSER_LAYOUT_H

#include "css/css.h"
#include "image/image.h"
#include "text/text.h"

/* How many layout units make a pixel. */
#define LAYOUT_UNIT		64

/* The deepest box nesting laid out (deeper boxes are dropped). */
#define LAYOUT_DEPTH_MAX	256

/*
 * A length in layout units.
 */
typedef int32_t layout_unit;

/*
 * The kinds of box.
 */
enum layout_box_kind {
	LAYOUT_BLOCK,
	LAYOUT_ANONYMOUS_BLOCK,
	LAYOUT_INLINE,
	LAYOUT_TEXT,
	LAYOUT_LINE_BREAK,
	LAYOUT_REPLACED
};

/*
 * Finds the decoded image an element shows (NULL when it has none); the
 * page supplies it, so that the layout needs no knowledge of loading.
 */
typedef const struct img_bitmap *(*layout_image_lookup)(void *context, const struct dom_element *element);

/*
 * Finds the decoded image a style's URL names (a background image; NULL
 * when it cannot be had), for the page to supply likewise.
 */
typedef const struct img_bitmap *(*layout_url_lookup)(void *context, const struct vm_string *url);

/*
 * A piece of text on a line: a run of a text box's characters, with its
 * font, position and width, or an atomic piece (a replaced box or an
 * inline block).  x is from the line's left; the piece's baseline is the
 * line's moved down by shift (up when it is negative), as vertical-align
 * places it.
 */
struct layout_fragment {
	struct layout_box *box;
	const uint16_t *text;
	size_t length;
	struct text_font font;
	uint32_t color;
	int underline;
	layout_unit x;
	layout_unit width;
	layout_unit ascent;
	layout_unit descent;
	layout_unit shift;
	/* A fragment of no text that stands for an inline box's left or right margin, border and padding (1 left, 2 right). */
	int edge;
};

/*
 * What an inline box (with a background, borders or padding) takes of one
 * line: where its fragments start and end, how far they reach above and
 * below the baseline, and whether the line holds its left and right edges.
 */
struct layout_decoration {
	struct layout_box *box;
	layout_unit x;
	layout_unit width;
	layout_unit ascent;
	layout_unit descent;
	layout_unit shift;
	int has_left;
	int has_right;
};

/*
 * One line box of a block's inline content: where it is in the block's
 * content box, its height and baseline, and its fragments.
 */
struct layout_line {
	layout_unit y;
	layout_unit height;
	layout_unit baseline;
	layout_unit left;
	struct layout_fragment *fragments;
	size_t fragment_count;
	struct layout_decoration *decorations;
	size_t decoration_count;
};

/*
 * A box: a block (or anonymous block), an inline element, a run of text,
 * or a line break.
 *
 * x and y are the border box's position relative to the parent's content
 * box until the final pass makes them absolute; width and height are the
 * content box's.  A block whose children are inline has lines instead of
 * laid out children.  An absolutely positioned or fixed box is out of the
 * flow: its parent's layout passes it by and records its static position
 * (where it would have been, relative to the parent's content box, then
 * absolute), and position.c places it.  An atomic box (an inline block)
 * is a block among its parent's inline content: the line it is on places
 * it (x and y relative to that block's content box, like a float).
 */
struct layout_transition_name;
struct layout_box {
	int kind;
	struct dom_node *node;
	struct css_style style;
	struct layout_box *parent;
	struct layout_box *first_child;
	struct layout_box *last_child;
	struct layout_box *next;
	int children_inline;
	int out_of_flow;
	int floating;
	int atomic;
	layout_unit static_x;
	layout_unit static_y;
	/* A Grid absolute child's static_x names the right edge of its area. */
	int static_x_from_right;
	layout_unit static_x_area_width;
	int static_x_align;
	int static_x_align_from_right;
	/* Definite Grid block-axis area, used after an absolute child's height is known. */
	layout_unit static_y_area_height;
	int static_y_align;
	/* A flex container's static position centers and ends without clamping an overflow. */
	int static_unsafe;
	/* The alignment written safe: an overflowing box starts at the area's start instead. */
	int static_x_safe;
	int static_y_safe;
	/* Definite Grid area supplies the containing block for explicit insets. */
	int static_grid_area;
	/*
	 * The containing block an absolute child takes from its Grid parent's
	 * lines (relative to the parent's content box); an edge flagged auto is
	 * the parent's padding edge.
	 */
	int cb_set;
	int cb_left_auto;
	int cb_right_auto;
	int cb_top_auto;
	int cb_bottom_auto;
	layout_unit cb_left;
	layout_unit cb_right;
	layout_unit cb_top;
	layout_unit cb_bottom;

	/* The box model, resolved. */
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
	layout_unit margin[4];
	layout_unit border[4];
	layout_unit padding[4];

	/* The margins after collapsing with the first and last children. */
	layout_unit collapsed_top;
	layout_unit collapsed_bottom;

	/* The lines of a block with inline children. */
	struct layout_line *lines;
	size_t line_count;
	/* A bounded vertical-lr text-only line mapping; line coordinates are logical until painted. */
	int vertical_inline;
	/* An in-flow item of a horizontal grid whose writing mode is vertical; its inline room is the row's height (negative: unbounded). */
	int grid_orthogonal;
	layout_unit grid_room;
	/*
	 * Where a grid or flex item was put: 1 + its grid row and column, or
	 * 1 + its flex line and its place in the line (0 when not known); the
	 * container's own baseline comes from its first row or line.
	 */
	int item_row;
	int item_column;

	/* The text of a text box, whitespace kept as it came (the lines collapse it). */
	const uint16_t *text;
	size_t text_length;

	/* The list marker of a list item ("•" or "1."), empty when none. */
	uint16_t marker[8];
	size_t marker_length;

	/* A replaced box (an <img>), and the image it shows (NULL when it has none). */
	int replaced;
	const struct img_bitmap *image;

	/*
	 * A form control's box (control.c): its kind (DOM_CONTROL_*, NONE for
	 * other boxes), its natural size, and whether its text has a baseline
	 * and how far below its content box's top that is.  An inline block
	 * keeps its last line's baseline in the same two fields (inline.c).
	 */
	int control;
	layout_unit natural_width;
	layout_unit natural_height;
	int has_baseline;
	layout_unit control_baseline;

	/* The background image the style names, decoded (NULL when there is none, or it could not be had). */
	const struct img_bitmap *background;

	/* A flex item's margin box along its container's main axis before it flexed (flex.c). */
	layout_unit flex_hypothetical;

	/* A grid container's (or a table's) columns and gaps at their content's sizes, which its content is as wide as (grid.c, table.c). */
	layout_unit grid_content;

	/* Final horizontal Grid tracks in CSS pixels, owned by this layout tree's arena. */
	double *grid_used_columns;
	double *grid_used_rows;
	int grid_used_column_count;
	int grid_used_row_count;
	int grid_used_valid;
	/* A box out of the flow's insets from the area it was placed in (top, right, bottom, left), when used_inset_known. */
	layout_unit used_inset[4];
	int used_inset_known;
	/* The pass of layout_position that placed the box out of the flow. */
	int placed_pass;

	/* anchor-center keeps a box inside its containing block (the range across, then down, when anchor_clamp_x or _y). */
	int anchor_clamp_x;
	int anchor_clamp_y;
	layout_unit anchor_clamp[4];

	/* How a position-try fallback turned the box's position-area: 4 over the diagonal (first), then 1 down and 2 across. */
	int anchor_flip;

	/* The implicit tracks a Grid put before its explicit ones (grid-names.c). */
	int grid_leading_columns;
	int grid_leading_rows;

	/*
	 * The width the box's content takes laid out without a limit (its
	 * max-content width), once it has been measured for shrinking to fit;
	 * the measurement does not depend on the room, so it is made once.
	 */
	layout_unit max_content;
	int max_content_known;

	/* A box of the view transition tree (transition.c): in the tree; the old or new image of a name. */
	int vt_tree;
	const struct layout_transition_name *vt_old;
	const struct layout_transition_name *vt_new;

	/* How far a sticky box is moved from where the flow put it (sticky.c). */
	layout_unit sticky_dx;
	layout_unit sticky_dy;

	/*
	 * While a vertical writing mode's domain is laid out in logical
	 * coordinates (logical.c): the box's physical style, kept in the tree's
	 * arena while its style is the logical one (NULL otherwise), and whether
	 * the box is the root being laid out in coordinates of its own.
	 */
	struct css_style *physical_style;
	int logical_root;
	/* Set while a flex container lays the item out: its main size is not definite for its children's percentages. */
	int height_indefinite;
};

/*
 * A rectangle of the page in layout units from the document's top left:
 * the place a node takes (layout_node_bounds).
 */
struct layout_rect {
	layout_unit x;
	layout_unit y;
	layout_unit width;
	layout_unit height;
};

struct paint_snapshot;

/*
 * One name of a view transition (https://drafts.csswg.org/css-view-transitions-1/): the root or an element, what
 * was captured of its old state (an image of what was painted and where the border box was) and where its new
 * state's border box is.  The name is an atom.
 */
struct layout_transition_name {
	struct vm_string *name;
	struct vm_string *classes;
	int is_root;
	int has_old;
	int has_new;
	struct layout_rect old_rect;
	struct layout_rect new_rect;
	struct paint_snapshot *old_image;
};

/*
 * A view transition's state the layout and the painting read: its names in order and whether the pseudo-element
 * tree is shown (from when the new state is made until the transition ends).
 */
struct layout_transition {
	struct layout_transition_name *names;
	size_t count;
	size_t capacity;
	int show;
	/* The heap the generated names (match-element's) are atoms of. */
	struct vm_heap *heap;
	/* The seconds the animations of the pseudo-elements have run (they start when the tree is shown). */
	float elapsed;
};

/*
 * A laid out page: the box tree, the arena the boxes live in and the size
 * the layout was made for.  While a layout runs, floats is the current
 * block formatting context's list of floats (float.c) and origin_x,
 * origin_y the content box of the block being laid out in that context's
 * coordinates.  image_lookup and image_context find the image of an
 * <img>, url_lookup a background image's.  measuring counts the
 * measurements of content laid out at a very wide width that are in
 * progress (shrink-to-fit, a flex item's content size); while one is, a
 * percentage of the containing width is indefinite, and a flex basis given
 * in percent is the content's size instead.
 */
struct layout_tree {
	struct wb_arena arena;
	struct layout_box *root;
	struct text_system *text;
	layout_unit viewport_width;
	layout_unit viewport_height;
	layout_unit document_height;
	struct wb_vector *floats;
	layout_unit origin_x;
	layout_unit origin_y;
	layout_image_lookup image_lookup;
	layout_url_lookup url_lookup;
	void *image_context;
	int measuring;
	/* The content height of the current containing block, when it is definite. */
	layout_unit containing_height;
	int containing_height_definite;
	/* The sticky boxes in tree order and the page scroll they are placed for (sticky.c). */
	struct layout_box **sticky;
	size_t sticky_count;
	layout_unit sticky_scroll;
	/* The placing of the boxes out of the flow: which pass it is (layout_box.placed_pass names the pass that placed a box). */
	int position_pass;
	/* The writing mode whose logical coordinates the boxes are being laid out in (horizontal-tb outside any vertical domain). */
	int logical_mode;
	/* The view transition being shown or measured (not owned), and whether this build is the measuring one. */
	const struct layout_transition *transition;
};

/*
 * The block formatting context a box's layout started a new one inside of
 * (float.c): its floats and the origin of the box's content box in it.
 */
struct layout_context {
	struct wb_vector *floats;
	layout_unit origin_x;
	layout_unit origin_y;
};

/* The layout (box.c, block.c, inline.c, float.c, position.c, replaced.c, dump.c, hit.c). */
int layout_build(struct layout_tree *tree, struct css_engine *css, struct text_system *text, struct dom_document *document, layout_image_lookup image_lookup, layout_url_lookup url_lookup, void *image_context, int width, int height, const struct layout_transition *transition);
void layout_box_model(struct layout_box *box, layout_unit containing_width);
void layout_auto_margins(struct layout_box *box, layout_unit containing_width);
void layout_replaced_size(struct layout_box *box, layout_unit containing_width, layout_unit containing_height, int height_definite);
void layout_absolute(struct layout_box *box, layout_unit x, layout_unit y);
int layout_is_positioned(const struct layout_box *box);
int layout_is_layered(const struct layout_box *box);
int layout_contains_absolute(const struct layout_box *box);
int layout_contains_fixed(const struct layout_box *box);

/* View transitions (transition.c). */
struct layout_transition_name *layout_transition_find(const struct layout_transition *transition, const struct vm_string *name);
int layout_transition_measure(const struct layout_tree *tree, struct layout_transition *transition, int old_state);
const struct layout_box *layout_transition_box(const struct layout_transition *transition, const struct layout_box *box, const struct vm_string *name, int depth);
struct vm_string *layout_transition_name_of(const struct layout_transition *transition, const struct layout_box *box);
void layout_transition_release(struct layout_transition *transition);
float layout_transition_progress(const struct css_style *style, float elapsed);
int layout_transition_group_rect(const struct layout_transition_name *name, const struct css_style *style, float elapsed, struct layout_rect *rect);

/* Anchor positioning (anchor.c). */
int layout_anchor_wanted(const struct layout_box *box);
int layout_anchor_try(struct layout_tree *tree, struct layout_box *box, int entry);
void layout_anchor_center(const struct layout_tree *tree, struct layout_box *box, layout_unit x, layout_unit y, layout_unit width, layout_unit height);
void layout_anchor_area(const struct layout_tree *tree, struct layout_box *box, layout_unit x, layout_unit y, layout_unit width, layout_unit height);
void layout_anchor_resolve(const struct layout_tree *tree, struct layout_box *box, layout_unit x, layout_unit y, layout_unit width, layout_unit height);
int layout_clips(const struct layout_box *box);
int layout_position(struct layout_tree *tree);
int layout_position_box(struct layout_tree *tree, struct layout_box *box);
int layout_sticky_build(struct layout_tree *tree, layout_unit scroll_y);
int layout_sticky_apply(struct layout_tree *tree, layout_unit scroll_y);

/* Centers a button's content in the height it has beyond content_height (block.c). */
void layout_center_button(struct layout_box *box, layout_unit content_height);
layout_unit layout_content_width(const struct layout_box *box, int depth);
void layout_context_begin(struct layout_tree *tree, struct layout_context *context, struct wb_vector *floats);
void layout_context_end(struct layout_tree *tree, const struct layout_context *context);
int layout_place_float(struct layout_tree *tree, struct layout_box *box, layout_unit y_min, layout_unit width);
void layout_line_room(const struct layout_tree *tree, layout_unit top, layout_unit bottom, layout_unit width, layout_unit *left, layout_unit *right);
layout_unit layout_below_float(const struct layout_tree *tree, layout_unit top);
layout_unit layout_clearance(const struct layout_tree *tree, int clear);
layout_unit layout_floats_bottom(const struct layout_tree *tree);
int layout_shrink_to_fit(struct layout_tree *tree, struct layout_box *box, layout_unit room);
int layout_max_content(struct layout_tree *tree, struct layout_box *box, layout_unit *width);
int layout_min_content(struct layout_tree *tree, struct layout_box *box, layout_unit *width);
int layout_stacking_order(const struct layout_tree *tree, struct wb_vector *boxes, size_t *flow_index);
void layout_release(struct layout_tree *tree);
int layout_block(struct layout_tree *tree, struct layout_box *box, layout_unit containing_width);
int layout_inline(struct layout_tree *tree, struct layout_box *box);
int layout_first_baseline(const struct layout_box *box, layout_unit *baseline);
int layout_last_baseline(const struct layout_box *box, layout_unit *baseline);
int layout_flex(struct layout_tree *tree, struct layout_box *box);
int layout_grid(struct layout_tree *tree, struct layout_box *box);
void layout_grid_names(struct layout_box *box);
int layout_physical_mode(const struct layout_box *box);
int layout_logical_wanted(const struct layout_tree *tree, const struct layout_box *box);
int layout_logical(struct layout_tree *tree, struct layout_box *box, layout_unit containing_width);
int layout_table(struct layout_tree *tree, struct layout_box *box);
layout_unit layout_flex_content_width(const struct layout_box *box);
void layout_font_of(struct layout_tree *tree, const struct css_style *style, struct text_font *font);
int layout_dump(const struct layout_tree *tree, struct wb_buffer *out);
const struct layout_box *layout_hit(const struct layout_tree *tree, layout_unit x, layout_unit y);
struct dom_node *layout_hit_node(const struct layout_tree *tree, layout_unit x, layout_unit y);
int layout_node_bounds(const struct layout_tree *tree, const struct dom_node *node, struct layout_rect *rect);
const struct layout_box *layout_box_of(const struct layout_tree *tree, const struct dom_node *node);
layout_unit layout_from_px(float px);
float layout_to_px(layout_unit value);

/* Form controls (control.c). */
int layout_control_measure(struct layout_tree *tree, struct layout_box *box);
int layout_control_line(struct text_system *text, const struct css_style *style, layout_unit *line, layout_unit *ascent);
int layout_units_width(struct text_system *text, const struct text_font *font, const uint16_t *units, size_t length, layout_unit *width);

#endif
