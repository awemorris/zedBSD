/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The box tree: built from the DOM and the computed styles, with anonymous
 * blocks where a block holds both block and inline children, then laid out
 * from the root and given absolute positions (relatively positioned boxes
 * shifted by their offsets), and its out-of-flow boxes placed.  An inline
 * block is a block box marked atomic, which counts as inline content.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The list markers of list-style-type, as UTF-16. */
#define BOX_BULLET_DISC		0x2022U
#define BOX_BULLET_CIRCLE	0x25e6U
#define BOX_BULLET_SQUARE	0x25aaU

static void box_root_flow(struct layout_box *root);
static int box_build_element(struct layout_tree *tree, struct css_engine *css, struct dom_element *element, const struct css_style *parent_style, struct layout_box *parent, int depth);
static int box_build_pseudo(struct layout_tree *tree, struct css_engine *css, struct dom_element *element, struct layout_box *box, int pseudo);
static int box_build_children(struct layout_tree *tree, struct css_engine *css, struct dom_node *node, const struct css_style *style, struct layout_box *box, int depth);
static struct layout_box *box_new(struct layout_tree *tree, int kind, struct dom_node *node, const struct css_style *style);
static void box_append(struct layout_box *parent, struct layout_box *child);
static int box_is_inline_level(const struct layout_box *box);
static int box_is_whitespace(const struct layout_box *box);
static int box_is_blank(const struct layout_box *box);
static int box_fix_children(struct layout_tree *tree, struct layout_box *box);
static int box_table_fix(struct layout_tree *tree, struct layout_box *box);
static int box_table_children(struct layout_tree *tree, struct layout_box *box);
static int box_wrap_tables(struct layout_tree *tree, struct layout_box *box);
static int box_is_table_part(const struct layout_box *box);
static struct layout_box *box_anonymous_of(struct layout_tree *tree, const struct layout_box *parent, int display);
static int box_flex_items(struct layout_tree *tree, struct layout_box *box);
static int box_build_transition(struct layout_tree *tree, struct css_engine *css, struct dom_element *element, struct layout_box *root);
static void box_anonymous_style(const struct css_style *parent, struct css_style *style);
static void box_marker(struct layout_box *box, int ordinal);
static void box_relative_offset(const struct layout_box *box, layout_unit *dx, layout_unit *dy);
static void box_relative_within(const struct layout_box *box, const struct layout_box *containing, layout_unit *dx, layout_unit *dy);
static void box_inline_relative(struct layout_box *block);
static void box_transform_offset(const struct layout_box *box, layout_unit *dx, layout_unit *dy);
static void box_apply_scale(struct layout_box *box);
static void box_static_inline(struct layout_box *box, layout_unit x, layout_unit y);

/*
 * Builds and lays out the box tree of a document for a viewport of width
 * by height pixels; image_lookup (with its context) finds the image of an
 * <img>, url_lookup the image a background names.
 */
int
layout_build(
	struct layout_tree *tree,
	struct css_engine *css,
	struct text_system *text,
	struct dom_document *document,
	layout_image_lookup image_lookup,
	layout_url_lookup url_lookup,
	void *image_context,
	int width,
	int height,
	const struct layout_transition *transition)
{
	struct dom_node *node;
	struct layout_box *root;
	layout_unit total;
	int error;

	/* Starts an empty tree for the viewport. */
	memset(tree, 0, sizeof(*tree));
	wb_arena_init(&tree->arena, 0);
	tree->text = text;
	tree->image_lookup = image_lookup;
	tree->url_lookup = url_lookup;
	tree->image_context = image_context;
	tree->transition = transition;
	tree->viewport_width = (layout_unit)width * LAYOUT_UNIT;
	tree->viewport_height = (layout_unit)height * LAYOUT_UNIT;
	tree->containing_height = tree->viewport_height;
	tree->containing_height_definite = 1;
	css_engine_set_viewport(css, (float)width, (float)height);

	/* Builds the boxes of the root element. */
	for (node = document->node.first_child; node != NULL; node = node->next) {
		if (node->type != DOM_ELEMENT)
			continue;

		/* The root element's box has no parent. */
		error = box_build_element(tree, css, (struct dom_element *)node, NULL, NULL, 0);
		if (error != 0)
			return error;
		break;
	}

	/* A document without a rendered root has nothing to lay out. */
	root = tree->root;
	if (root == NULL)
		return 0;

	/* An HTML root uses its body's writing mode and direction (its own computed ones still pass to its children). */
	box_root_flow(root);

	/* Lays the root out in the viewport and makes every position absolute. */
	error = layout_block(tree, root, tree->viewport_width);
	if (error != 0)
		return error;
	root->x = root->margin[CSS_LEFT];
	root->y = root->margin[CSS_TOP];

	/* A vertical-rl root starts its blocks at the viewport's right edge. */
	if (root->style.writing_mode == CSS_WRITING_VERTICAL_RL)
		root->x = tree->viewport_width - root->margin[CSS_RIGHT] - root->border[CSS_RIGHT] -
		    root->padding[CSS_RIGHT] - root->width - root->padding[CSS_LEFT] - root->border[CSS_LEFT];
	layout_absolute(root, 0, 0);

	/* The document is as tall as the root's margin box. */
	total = root->margin[CSS_TOP] + root->border[CSS_TOP] + root->padding[CSS_TOP] + root->height +
	    root->padding[CSS_BOTTOM] + root->border[CSS_BOTTOM] + root->margin[CSS_BOTTOM];
	tree->document_height = total;

	/* The boxes out of the flow go to their containing blocks (the document grows to hold them). */
	error = layout_position(tree);
	if (error != 0)
		return error;

	/* The scrolled containers move their content. */
	error = layout_scroll_apply(tree);
	if (error != 0)
		return error;

	/* A box with nothing inside it that transform scales (a hairline border drawn at twice the size and halved) shrinks about its origin. */
	box_apply_scale(root);

	/* Sticky boxes sit where the page's scroll (zero until the view says) puts them. */
	error = layout_sticky_build(tree, 0);
	if (error != 0)
		return error;

	/* Succeeded: the tree is laid out. */
	return 0;
}

/*
 * Frees a layout tree.
 */
void
layout_release(
	struct layout_tree *tree)
{
	/* Everything lives in the arena. */
	wb_arena_release(&tree->arena);
	tree->root = NULL;
}

/*
 * Converts pixels to layout units, rounding to the nearest unit.
 */
layout_unit
layout_from_px(
	float px)
{
	float units;
	float limit;

	/* Scales finite pixels without casting NaN or infinity to an integer. */
	units = px * (float)LAYOUT_UNIT;
	if (units != units)
		return 0;
	limit = (float)INT32_MAX;
	if (units >= limit)
		return INT32_MAX;
	if (units <= -limit)
		return -INT32_MAX;

	/* Rounds ordinary values away from zero at the half. */
	if (units < 0)
		return (layout_unit)(units - 0.5f);

	/* Positive and zero values. */
	return (layout_unit)(units + 0.5f);
}

/*
 * Converts layout units to pixels.
 */
float
layout_to_px(
	layout_unit value)
{
	/* Divides by the units in a pixel. */
	return (float)value / (float)LAYOUT_UNIT;
}

/*
 * Turns positions relative to the parent's content box into absolute ones
 * for a box placed at x and y (its parent's content origin) and its
 * descendants: a relatively positioned box moves by its offsets, and a
 * box out of the flow gets its absolute static position (it is placed
 * later).
 */
void
layout_absolute(
	struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	struct layout_box *child;
	layout_unit content_x;
	layout_unit content_y;
	layout_unit dx;
	layout_unit dy;

	/* The box's border box moves by the parent's content origin, and by its offsets when it is relative. */
	box_relative_offset(box, &dx, &dy);
	box_transform_offset(box, &dx, &dy);
	box->x += x + dx;
	box->y += y + dy;

	/* Its children are relative to its content box. */
	content_x = box->x + box->border[CSS_LEFT] + box->padding[CSS_LEFT];
	content_y = box->y + box->border[CSS_TOP] + box->padding[CSS_TOP];

	/* The lines are relative to the block; boxes out of the flow among them start at its content's top. */
	if (box->children_inline) {
		box_inline_relative(box);
		box_static_inline(box, content_x, content_y);
		return;
	}

	/* A block child moves with the box; one out of the flow only learns where its static position is. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow) {
			child->static_x += content_x;
			child->static_y += content_y;
			child->cb_left += content_x;
			child->cb_right += content_x;
			child->cb_top += content_y;
			child->cb_bottom += content_y;
			continue;
		}

		/* A child in the flow. */
		layout_absolute(child, content_x, content_y);
	}
}

/*
 * Tells whether a box is positioned for the painting order: relative,
 * absolute or fixed (sticky is laid out as static in this pass).
 */
int
layout_is_positioned(
	const struct layout_box *box)
{
	/* Only blocks are painted as positioned boxes. */
	if (box->kind != LAYOUT_BLOCK)
		return 0;

	/* The three positioned schemes. */
	if (box->style.position == CSS_POSITION_RELATIVE)
		return 1;
	if (box->style.position == CSS_POSITION_ABSOLUTE)
		return 1;
	if (box->style.position == CSS_POSITION_FIXED)
		return 1;
	if (box->style.position == CSS_POSITION_STICKY)
		return 1;

	/* Static boxes. */
	return 0;
}

/* Tells whether a box is the containing block of the boxes out of the flow inside it: a positioned box or one with a transform. */
int
layout_contains_absolute(
	const struct layout_box *box)
{
	/* Positioned, or transformed. */
	return layout_is_positioned(box) || layout_contains_fixed(box);
}

/* Tells whether a box is the containing block of the fixed boxes inside it: one with a transform. */
int
layout_contains_fixed(
	const struct layout_box *box)
{
	/* A transformed block, or one with a backdrop filter or a will-change that makes one. */
	return box->kind == LAYOUT_BLOCK &&
	    (box->style.transform_set || box->style.backdrop_filter_count != 0 || (box->style.will_change_contains & 1));
}

/*
 * Tells whether a box is painted in its own turn of its stacking context:
 * a positioned box, or a flex or grid item with a z-index (which stacks as
 * a positioned box does without being positioned).
 * https://drafts.csswg.org/css-flexbox-1/#painting
 * https://drafts.csswg.org/css-grid-2/#z-order
 */
int
layout_is_layered(
	const struct layout_box *box)
{
	int display;

	/* Positioned boxes. */
	if (layout_is_positioned(box))
		return 1;

	/* An in-flow flex or grid item with an integer z-index. */
	if (box->kind != LAYOUT_BLOCK || box->style.z_index_auto || box->out_of_flow || box->parent == NULL)
		return 0;
	display = box->parent->style.display;
	if (display == CSS_DISPLAY_FLEX || display == CSS_DISPLAY_INLINE_FLEX ||
	    display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID)
		return 1;

	/* Other boxes. */
	return 0;
}

/*
 * Tells whether a box clips its content to its padding box: its overflow
 * is not visible, and is not the viewport's (the root's overflow, or the
 * body's when the root's is visible, goes to the viewport).
 */
int
layout_clips(
	const struct layout_box *box)
{
	const struct dom_element *element;
	int is_body;

	/* Visible on both axes clips nothing. */
	if (box->style.overflow_x == CSS_OVERFLOW_VISIBLE && box->style.overflow_y == CSS_OVERFLOW_VISIBLE)
		return 0;

	/* The root's overflow is the viewport's. */
	if (box->parent == NULL)
		return 0;

	/* So is the body's, when the root leaves its own visible. */
	is_body = 0;
	if (box->node != NULL && box->node->type == DOM_ELEMENT && box->parent->parent == NULL) {
		element = (const struct dom_element *)box->node;
		is_body = dom_element_is(&element->node, DOM_NS_HTML, DOM_TAG_BODY);
	}

	/* The body gives its overflow to the viewport when the root keeps visible. */
	if (is_body &&
	    box->parent->style.overflow_x == CSS_OVERFLOW_VISIBLE &&
	    box->parent->style.overflow_y == CSS_OVERFLOW_VISIBLE)
		return 0;

	/* Any other box clips. */
	return 1;
}

/*
 * Picks the font a style draws text with.
 */
void
layout_font_of(
	struct layout_tree *tree,
	const struct css_style *style,
	struct text_font *font)
{
	int monospace;
	int found;
	int index;

	/* The monospace family uses the monospace face. */
	monospace = 0;
	if (style->generic_family == CSS_FAMILY_MONOSPACE)
		monospace = 1;
	text_select_font(tree->text, monospace, style->font_size, style->font_weight, font);

	/* The first family of the list the page's web fonts have (ws074-p070) takes its place. */
	if (tree->text->family_count == 0)
		return;
	for (index = 0; index < style->family_count; index++) {
		found = text_select_family(tree->text, style->families[index], style->font_weight, style->font_italic, font);
		if (found)
			return;
	}
}

/*
 * Gives the root element's box the writing mode and direction of its first
 * body child, as HTML documents use them.
 * https://drafts.csswg.org/css-writing-modes-4/#principal-flow
 */
static void
box_root_flow(
	struct layout_box *root)
{
	const struct dom_element *element;
	struct layout_box *child;
	int is_body;

	/* The first child box of a body element. */
	for (child = root->first_child; child != NULL; child = child->next) {
		if (child->node == NULL || child->node->type != DOM_ELEMENT)
			continue;
		element = (const struct dom_element *)child->node;
		is_body = vm_string_equal_ascii(element->local_name, "body");
		if (!is_body)
			continue;

		/* Its flow is the root's. */
		root->style.writing_mode = child->style.writing_mode;
		root->style.writing_sideways = child->style.writing_sideways;
		root->style.direction = child->style.direction;
		return;
	}
}

/* Builds the box of an element (none for display: none) and of its children. */
static int
box_build_element(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_element *element,
	const struct css_style *parent_style,
	struct layout_box *parent,
	int depth)
{
	struct css_style *style;
	struct layout_box *box;
	int out_of_flow;
	int floating;
	int replaced;
	int is_control;
	int is_iframe;
	int is_loaded_object;
	int atomic;
	int kind;
	int error;

	/* Boxes past the depth limit are dropped. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* Computes the style into a temporary (the box keeps its own copy). */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;
	error = css_engine_compute(css, element, parent_style, style);
	if (error != 0) {
		free(style);
		return error;
	}

	/* Keep iframe descendants in the DOM without making parent-page boxes for them. */
	is_iframe = element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IFRAME;
	is_loaded_object = element->ns == DOM_NS_HTML && element->tag == DOM_TAG_OBJECT &&
	    element->child_context != NULL && element->child_load_seen &&
	    element->child_loaded_epoch == element->child_epoch;

	/* display: none makes no box, for the element or its children (a table column neither, in this pass). */
	if (style->display == CSS_DISPLAY_NONE || style->display == CSS_DISPLAY_TABLE_COLUMN) {
		free(style);
		return 0;
	}

	/* display: contents makes no box; ordinary children's boxes go to the parent. */
	if (style->display == CSS_DISPLAY_CONTENTS && parent != NULL) {
		/* iframe descendants never represent fallback content, even without an iframe box. */
		error = 0;
		if (!is_iframe && !is_loaded_object)
			error = box_build_children(tree, css, &element->node, style, parent, depth + 1);
		free(style);
		return error;
	}

	/* The kind of box: blocks for every block-level display in this pass, inline otherwise. */
	kind = LAYOUT_BLOCK;
	if (style->display == CSS_DISPLAY_INLINE)
		kind = LAYOUT_INLINE;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_BR)
		kind = LAYOUT_LINE_BREAK;

	/* An <img>, a <video> (ws121-p004), or an <object> whose data decoded as an image, is a replaced box. */
	replaced = 0;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IMG)
		replaced = 1;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_VIDEO)
		replaced = 1;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_CANVAS)
		replaced = 1;
	if (element->ns == DOM_NS_SVG && element->tag == DOM_TAG_SVG && tree->image_lookup != NULL)
		replaced = tree->image_lookup(tree->image_context, element) != NULL;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_OBJECT && tree->image_lookup != NULL)
		replaced = tree->image_lookup(tree->image_context, element) != NULL;
	if (replaced && (kind == LAYOUT_INLINE || style->display == CSS_DISPLAY_INLINE_BLOCK))
		kind = LAYOUT_REPLACED;

	/*
	 * An <input> or a <textarea> is a replaced box too, drawn by the
	 * painting from its state: an atomic piece of its line when it is inline
	 * or an inline block, as the user agent's sheet makes it.
	 */
	is_control = 0;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_INPUT)
		is_control = 1;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_TEXTAREA)
		is_control = 1;
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_SELECT)
		is_control = 1;
	if (is_control) {
		replaced = 1;
		if (kind == LAYOUT_INLINE || style->display == CSS_DISPLAY_INLINE_BLOCK)
			kind = LAYOUT_REPLACED;
	}

	/* The root is a block. */
	if (parent == NULL)
		kind = LAYOUT_BLOCK;

	/* An inline block that is not replaced is a block placed as one piece of its line (ws074-p060). */
	atomic = 0;
	if (parent != NULL && kind == LAYOUT_BLOCK && !replaced && style->display == CSS_DISPLAY_INLINE_BLOCK)
		atomic = 1;
	if (parent != NULL && kind == LAYOUT_BLOCK && !replaced && style->display == CSS_DISPLAY_INLINE_TABLE)
		atomic = 1;
	if (parent != NULL && kind == LAYOUT_BLOCK && !replaced && style->display == CSS_DISPLAY_INLINE_GRID)
		atomic = 1;
	if (parent != NULL && kind == LAYOUT_BLOCK && !replaced && style->display == CSS_DISPLAY_INLINE_FLEX)
		atomic = 1;

	/* An absolutely positioned or fixed box is a block out of the flow (the root stays in it). */
	out_of_flow = 0;
	if (parent != NULL && (style->position == CSS_POSITION_ABSOLUTE || style->position == CSS_POSITION_FIXED)) {
		kind = LAYOUT_BLOCK;
		out_of_flow = 1;
		atomic = 0;
	}

	/* A float that is not out of the flow is a block beside the flow. */
	floating = CSS_FLOAT_NONE;
	if (parent != NULL && !out_of_flow && style->float_side != CSS_FLOAT_NONE) {
		kind = LAYOUT_BLOCK;
		floating = style->float_side;
		atomic = 0;
	}

	/* Makes the box and places it in the tree. */
	box = box_new(tree, kind, &element->node, style);
	free(style);
	if (box == NULL)
		return ENOMEM;
	box->out_of_flow = out_of_flow;
	box->floating = floating;
	box->atomic = atomic;

	/* A replaced box shows its element's image, and has no children. */
	if (replaced && !is_control) {
		box->replaced = 1;
		if (tree->image_lookup != NULL)
			box->image = tree->image_lookup(tree->image_context, element);
	}

	/* A control's box has the natural size of its text and attributes instead. */
	if (is_control) {
		box->replaced = 1;
		error = layout_control_measure(tree, box);
		if (error != 0)
			return error;
	}

	/* The box goes under its parent, or is the root. */
	if (parent == NULL) {
		tree->root = box;
	} else {
		box_append(parent, box);
	}

	/* A loaded object or iframe keeps fallback children only in the DOM. */
	if (replaced || is_iframe || is_loaded_object)
		return 0;

	/* The content of a box with content-visibility hidden is skipped: the box has no children, as if it were size-contained. */
	if (box->style.content_visibility == 2 && parent != NULL)
		return 0;

	/* The ::before box, the children, the ::after box, then the fix-ups a block needs. */
	error = box_build_pseudo(tree, css, element, box, CSS_PSEUDO_ELEMENT_BEFORE);
	if (error != 0)
		return error;
	error = box_build_children(tree, css, &element->node, &box->style, box, depth + 1);
	if (error != 0)
		return error;
	error = box_build_pseudo(tree, css, element, box, CSS_PSEUDO_ELEMENT_AFTER);
	if (error != 0)
		return error;

	/* The root element of a page with a view transition on show has the transition's tree. */
	if (parent == NULL && tree->transition != NULL && tree->transition->show) {
		error = box_build_transition(tree, css, element, box);
		if (error != 0)
			return error;
	}
	error = box_fix_children(tree, box);
	if (error != 0)
		return error;

	/* Succeeded: the element's boxes are built. */
	return 0;
}

/*
 * Builds an element's ::before or ::after box (ws074-p069) as the first or
 * last child of the element's box: a box of its display with its content
 * as text, when some rule gives it content.  The boxes belong to no node.
 */
static int
box_build_pseudo(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_element *element,
	struct layout_box *box,
	int pseudo)
{
	struct css_style *style;
	struct layout_box *generated;
	struct layout_box *text_box;
	const struct css_content_item *item;
	const struct vm_string *part;
	struct dom_attribute *attribute;
	struct wb_units text;
	uint16_t *units;
	uint16_t unit;
	size_t index;
	size_t position;
	int kind;
	int error;

	/* Only a pseudo-element some rule names is styled. */
	if ((box->style.pseudo_elements & (1 << pseudo)) == 0)
		return 0;

	/* Its style, inherited from the element's. */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;
	error = css_engine_compute_pseudo(css, element, pseudo, &box->style, style);
	if (error != 0) {
		free(style);
		return error;
	}

	/* No content, or display: none, makes no box. */
	if (style->content_kind == CSS_CONTENT_NONE || style->display == CSS_DISPLAY_NONE) {
		free(style);
		return 0;
	}

	/* The text: the items' strings and attributes' values, joined. */
	wb_units_init(&text);
	error = 0;
	for (index = 0; index < style->content->count && error == 0; index++) {
		item = &style->content->items[index];
		part = item->text;
		if (item->is_attribute) {
			part = NULL;
			attribute = dom_element_find_attribute(element, DOM_NS_NONE, item->text);
			if (attribute != NULL)
				part = attribute->value;
		}

		/* A missing attribute adds nothing; the rest adds its characters. */
		if (part == NULL)
			continue;
		for (position = 0; position < part->length && error == 0; position++) {
			unit = vm_string_at(part, position);
			error = wb_units_append(&text, &unit, 1);
		}
	}

	/* Memory ran out on the way. */
	if (error != 0) {
		wb_units_release(&text);
		free(style);
		return error;
	}

	/* The box: inline, or a block (out of the flow, or floating, as an element's). */
	kind = LAYOUT_BLOCK;
	if (style->display == CSS_DISPLAY_INLINE)
		kind = LAYOUT_INLINE;
	generated = box_new(tree, kind, NULL, style);
	if (generated == NULL) {
		wb_units_release(&text);
		free(style);
		return ENOMEM;
	}

	/* Out of the flow, or floating, as an element's box would be; otherwise an inline block is atomic. */
	if (style->position == CSS_POSITION_ABSOLUTE || style->position == CSS_POSITION_FIXED) {
		generated->kind = LAYOUT_BLOCK;
		generated->out_of_flow = 1;
	} else if (style->float_side != CSS_FLOAT_NONE) {
		generated->kind = LAYOUT_BLOCK;
		generated->floating = style->float_side;
	} else if (style->display == CSS_DISPLAY_INLINE_BLOCK) {
		generated->atomic = 1;
	}

	/* The box goes into the element's. */
	box_append(box, generated);

	/* The text as a text box, its characters copied into the tree's arena. */
	if (text.length != 0) {
		units = wb_arena_alloc(&tree->arena, text.length * sizeof(uint16_t));
		if (units == NULL) {
			wb_units_release(&text);
			free(style);
			return ENOMEM;
		}

		/* The characters. */
		memcpy(units, text.data, text.length * sizeof(uint16_t));

		/* The text box takes the pseudo-element's style. */
		text_box = box_new(tree, LAYOUT_TEXT, NULL, style);
		if (text_box == NULL) {
			wb_units_release(&text);
			free(style);
			return ENOMEM;
		}

		/* It shows the characters, inside the pseudo-element's box. */
		text_box->text = units;
		text_box->text_length = text.length;
		box_append(generated, text_box);
	}

	/* The text and the style were copied into the boxes. */
	wb_units_release(&text);
	free(style);

	/* A block around text needs the fix-ups of any block. */
	error = box_fix_children(tree, generated);
	if (error != 0)
		return error;

	/* Succeeded: the pseudo-element's box is built. */
	return 0;
}

/* Sets a length to pixels. */
static void
box_transition_px(
	struct css_length *length,
	layout_unit value)
{
	length->unit = CSS_UNIT_PX;
	length->value = layout_to_px(value);
	length->offset = 0.0f;
}

/*
 * Makes the box of one pseudo-element of the view transition tree below a parent box: its style from the
 * rules for the pseudo-element of the name, the style it inherits from the parent pseudo-element's, and out of
 * the flow when it is positioned.  Returns it (NULL: it has none, or memory ran out, told by *error).
 */
static struct layout_box *
box_transition_pseudo(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_element *element,
	struct layout_box *parent,
	const struct css_style *parent_style,
	int pseudo,
	const struct layout_transition_name *name,
	int siblings,
	struct css_style *style,
	int *error)
{
	struct layout_box *box;

	*error = css_engine_compute_vt_pseudo(css, element, pseudo, name != NULL ? name->name : NULL, name != NULL ? name->classes : NULL, siblings, parent_style, style);
	if (*error != 0)
		return NULL;
	if (style->display == CSS_DISPLAY_NONE)
		return NULL;
	box = box_new(tree, LAYOUT_BLOCK, NULL, style);
	if (box == NULL) {
		*error = ENOMEM;
		return NULL;
	}
	box->vt_tree = 1;
	if (style->position == CSS_POSITION_ABSOLUTE || style->position == CSS_POSITION_FIXED)
		box->out_of_flow = 1;
	box_append(parent, box);
	return box;
}

/*
 * Builds the view transition's pseudo-element tree as the last child of the root's box (box.c's ::after way):
 * ::view-transition, and for each name its group, image pair and old and new images.  A group is as big as the
 * border box its animation has reached; an image is the group's width and as high as its picture is.
 */
static int
box_build_transition(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_element *element,
	struct layout_box *root)
{
	const struct layout_transition *transition;
	const struct layout_transition_name *name;
	struct css_style *root_style;
	struct css_style *group_style;
	struct css_style *pair_style;
	struct css_style *image_style;
	struct layout_box *top;
	struct layout_box *group;
	struct layout_box *pair;
	struct layout_box *image;
	struct layout_rect rect;
	size_t index;
	int error;
	int which;
	float progress;
	float picture_width;
	float picture_height;
	float group_width;
	float group_height;
	size_t first_group;
	size_t last_group;
	int siblings;

	transition = tree->transition;
	first_group = transition->count;
	last_group = 0;
	for (index = 0; index < transition->count; index++) {
		if (!transition->names[index].has_old && !transition->names[index].has_new)
			continue;
		if (first_group == transition->count)
			first_group = index;
		last_group = index;
	}
	root_style = malloc(4U * sizeof(*root_style));
	if (root_style == NULL)
		return ENOMEM;
	group_style = root_style + 1;
	pair_style = root_style + 2;
	image_style = root_style + 3;
	error = 0;
	top = box_transition_pseudo(tree, css, element, root, &root->style, CSS_PSEUDO_ELEMENT_VT_ROOT, NULL, CSS_VT_FIRST | CSS_VT_LAST, root_style, &error);
	for (index = 0; top != NULL && error == 0 && index < transition->count; index++) {
		name = &transition->names[index];
		if (!name->has_old && !name->has_new)
			continue;

		/* Its place among the groups. */
		siblings = 0;
		if (index == first_group)
			siblings |= CSS_VT_FIRST;
		if (index == last_group)
			siblings |= CSS_VT_LAST;

		/* The group, at the size and place its animation has reached. */
		group = box_transition_pseudo(tree, css, element, top, root_style, CSS_PSEUDO_ELEMENT_VT_GROUP, name, siblings, group_style, &error);
		if (group == NULL)
			continue;
		memset(&rect, 0, sizeof(rect));
		layout_transition_group_rect(name, &group->style, transition->elapsed, &rect);
		if (group->style.width.unit == CSS_UNIT_AUTO)
			box_transition_px(&group->style.width, rect.width);
		if (group->style.height.unit == CSS_UNIT_AUTO)
			box_transition_px(&group->style.height, rect.height);
		/* The UA moves the group to its rectangle by a transform, which the author's rules replace. */
		if (group->style.transform_translate[0].value == CSS_VT_TRANSLATE_PENDING)
			group->style.transform_translate[0].value = layout_to_px(rect.x);
		if (group->style.transform_translate[1].value == CSS_VT_TRANSLATE_PENDING)
			group->style.transform_translate[1].value = layout_to_px(rect.y);
		group_width = group->style.width.unit == CSS_UNIT_PX ? group->style.width.value : layout_to_px(rect.width);
		group_height = group->style.height.unit == CSS_UNIT_PX ? group->style.height.value : layout_to_px(rect.height);

		/* The pair holds the two images. */
		pair = box_transition_pseudo(tree, css, element, group, &group->style, CSS_PSEUDO_ELEMENT_VT_PAIR, name, CSS_VT_FIRST | CSS_VT_LAST, pair_style, &error);
		if (pair == NULL)
			continue;
		for (which = 0; which < 2 && error == 0; which++) {
			if (which == 0 && !name->has_old)
				continue;
			if (which == 1 && !name->has_new)
				continue;
			image = box_transition_pseudo(tree, css, element, pair, &pair->style,
			    which == 0 ? CSS_PSEUDO_ELEMENT_VT_OLD : CSS_PSEUDO_ELEMENT_VT_NEW, name,
			    (name->has_old && name->has_new) ? (which == 0 ? CSS_VT_FIRST : CSS_VT_LAST) : (CSS_VT_FIRST | CSS_VT_LAST),
			    image_style, &error);
			if (image == NULL)
				continue;
			if (which == 0)
				image->vt_old = name;
			else
				image->vt_new = name;

			/* The picture's shape gives the height, from the width. */
			picture_width = layout_to_px(which == 0 ? name->old_rect.width : name->new_rect.width);
			picture_height = layout_to_px(which == 0 ? name->old_rect.height : name->new_rect.height);
			if (image->style.height.unit == CSS_UNIT_AUTO && picture_width > 0.0f) {
				float width_px;

				width_px = image->style.width.unit == CSS_UNIT_PERCENT ? group_width * image->style.width.value / 100.0f :
				    image->style.width.unit == CSS_UNIT_PX ? image->style.width.value : picture_width;
				image->style.height.unit = CSS_UNIT_PX;
				image->style.height.value = width_px * picture_height / picture_width;
				image->style.height.offset = 0.0f;
			}

			/* In a vertical writing mode the height is the inline size, given: the picture's shape gives the width from it. */
			if (image->style.width.unit == CSS_UNIT_AUTO && picture_height > 0.0f) {
				float height_px;

				height_px = image->style.height.unit == CSS_UNIT_PERCENT ? group_height * image->style.height.value / 100.0f :
				    image->style.height.unit == CSS_UNIT_PX ? image->style.height.value : picture_height;
				image->style.width.unit = CSS_UNIT_PX;
				image->style.width.value = height_px * picture_width / picture_height;
				image->style.width.offset = 0.0f;
			}

			/* The UA's animations fade the old image out and the new one in. */
			if (image->style.animation_name != NULL &&
			    vm_string_equal_ascii(image->style.animation_name, which == 0 ? "-ua-view-transition-fade-out" : "-ua-view-transition-fade-in")) {
				progress = layout_transition_progress(&image->style, transition->elapsed);
				image->style.opacity = which == 0 ? 1.0f - progress : progress;
			}
		}
	}
	free(root_style);
	return error;
}

/* Builds the boxes of a node's children into a box. */
static int
box_build_children(
	struct layout_tree *tree,
	struct css_engine *css,
	struct dom_node *node,
	const struct css_style *style,
	struct layout_box *box,
	int depth)
{
	const struct dom_character_data *text;
	struct dom_node *child;
	struct layout_box *text_box;
	int ordinal;
	int error;

	/* Each child: an element's boxes, or a text box. */
	ordinal = 0;
	for (child = node->first_child; child != NULL; child = child->next) {
		/* Elements build their own boxes; list items number themselves. */
		if (child->type == DOM_ELEMENT) {
			error = box_build_element(tree, css, (struct dom_element *)child, style, box, depth);
			if (error != 0)
				return error;
			if (box->last_child != NULL && box->last_child->node == child && box->last_child->style.display == CSS_DISPLAY_LIST_ITEM) {
				ordinal++;
				box_marker(box->last_child, ordinal);
			}

			continue;
		}

		/* Only text nodes are rendered among the others. */
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;
		text = (const struct dom_character_data *)child;
		if (text->data.length == 0)
			continue;

		/* A text box takes its element's style. */
		text_box = box_new(tree, LAYOUT_TEXT, child, style);
		if (text_box == NULL)
			return ENOMEM;
		text_box->text = text->data.data;
		text_box->text_length = text->data.length;
		box_append(box, text_box);
	}

	/* Succeeded: the children are built. */
	return 0;
}

/* Allocates a box in the tree's arena. */
static struct layout_box *
box_new(
	struct layout_tree *tree,
	int kind,
	struct dom_node *node,
	const struct css_style *style)
{
	struct layout_box *box;

	/* Allocates it zeroed. */
	box = wb_arena_zalloc(&tree->arena, sizeof(*box));
	if (box == NULL)
		return NULL;

	/* Records its kind, node and style. */
	box->kind = kind;
	box->node = node;
	box->style = *style;

	/* Anchor functions are only for boxes out of the flow; the others' are invalid. */
	if (box->style.position != CSS_POSITION_ABSOLUTE && box->style.position != CSS_POSITION_FIXED &&
	    layout_anchor_wanted(box))
		layout_anchor_resolve(tree, box, 0, 0, 0, 0);

	/* An element's box finds the background image its style names (text shares its element's style, not its box). */
	if (kind != LAYOUT_TEXT && style->background_image != NULL && tree->url_lookup != NULL)
		box->background = tree->url_lookup(tree->image_context, style->background_image);

	/* Succeeded: the box is detached. */
	return box;
}

/* Appends a box as the last child of another. */
static void
box_append(
	struct layout_box *parent,
	struct layout_box *child)
{
	/* Links it at the end. */
	child->parent = parent;
	if (parent->last_child != NULL) {
		parent->last_child->next = child;
	} else {
		parent->first_child = child;
	}

	/* The child is the parent's last one now. */
	parent->last_child = child;
}

/*
 * Makes a flex container's children its items (ws074-p035): each element
 * child a block-level box (an inline one, or an inline image, becomes a
 * block; a float stops floating), each run of text an anonymous block,
 * and whitespace alone dropped.
 */
static int
box_flex_items(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct layout_box *child;
	struct layout_box *next;
	struct layout_box *anonymous;
	struct css_style style;
	int whitespace;

	/* The children are laid out by the flex layout, not in lines. */
	box->children_inline = 0;
	box_anonymous_style(&box->style, &style);
	child = box->first_child;
	box->first_child = NULL;
	box->last_child = NULL;
	anonymous = NULL;
	while (child != NULL) {
		next = child->next;
		child->next = NULL;

		/* A box out of the flow stays as it is, and ends a run of text. */
		if (child->out_of_flow) {
			anonymous = NULL;
			box_append(box, child);
			child = next;
			continue;
		}

		/* An element's box is an item of its own, block-level whatever it was. */
		if (child->kind != LAYOUT_TEXT) {
			anonymous = NULL;
			child->floating = CSS_FLOAT_NONE;
			child->atomic = 0;
			if (child->kind == LAYOUT_INLINE || child->kind == LAYOUT_REPLACED || child->kind == LAYOUT_LINE_BREAK)
				child->kind = LAYOUT_BLOCK;
			box_append(box, child);
			child = next;
			continue;
		}

		/* Whitespace alone between items is dropped (preserved or not). */
		whitespace = box_is_blank(child);
		if (anonymous == NULL && whitespace) {
			child = next;
			continue;
		}

		/* Text starts, or goes on in, an anonymous item. */
		if (anonymous == NULL) {
			anonymous = box_new(tree, LAYOUT_ANONYMOUS_BLOCK, NULL, &style);
			if (anonymous == NULL)
				return ENOMEM;
			anonymous->children_inline = 1;
			box_append(box, anonymous);
		}

		/* The text goes into the anonymous item. */
		box_append(anonymous, child);
		child = next;
	}

	/* Succeeded: the children are all items. */
	return 0;
}

/* Tells whether a box takes part in inline formatting. */
static int
box_is_inline_level(
	const struct layout_box *box)
{
	/* Inline boxes, text, line breaks, inline replaced boxes and inline blocks. */
	if (box->kind == LAYOUT_INLINE || box->kind == LAYOUT_TEXT || box->kind == LAYOUT_LINE_BREAK)
		return 1;
	if (box->kind == LAYOUT_REPLACED)
		return 1;
	if (box->atomic)
		return 1;

	/* Blocks are block-level. */
	return 0;
}

/* Tells whether a box is text of nothing but spaces, tabs and line feeds, whether or not they collapse. */
static int
box_is_blank(
	const struct layout_box *box)
{
	size_t index;

	if (box->kind != LAYOUT_TEXT)
		return 0;
	for (index = 0; index < box->text_length; index++) {
		if (!text_is_space(box->text[index]))
			return 0;
	}
	return 1;
}

/* Tells whether a box is text of nothing but collapsible whitespace. */
static int
box_is_whitespace(
	const struct layout_box *box)
{
	size_t index;
	int space;

	/* Only text can be whitespace, and only where whitespace collapses. */
	if (box->kind != LAYOUT_TEXT)
		return 0;
	if (box->style.white_space == CSS_WHITE_SPACE_PRE || box->style.white_space == CSS_WHITE_SPACE_PRE_WRAP)
		return 0;

	/* Every character must be a space, tab or line feed. */
	for (index = 0; index < box->text_length; index++) {
		space = text_is_space(box->text[index]);
		if (!space)
			return 0;
	}

	/* The text is all whitespace. */
	return 1;
}

/*
 * Fixes a box's children after they are built: an inline box with block
 * children becomes a block, and a block with both kinds of children wraps
 * each run of inline children in an anonymous block.
 */
static int
box_fix_children(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct layout_box *child;
	struct layout_box *next;
	struct layout_box *anonymous;
	struct css_style style;
	int has_block;
	int has_inline;
	int inline_level;
	int whitespace;
	int error;

	/* Looks at the kinds of children (those out of the flow and floats count as neither). */
	has_block = 0;
	has_inline = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE)
			continue;
		inline_level = box_is_inline_level(child);
		whitespace = box_is_whitespace(child);
		if (!inline_level)
			has_block = 1;
		if (inline_level && !whitespace)
			has_inline = 1;
	}

	/* The parts of tables, and the anonymous table boxes they need (ws074-p037). */
	error = box_table_fix(tree, box);
	if (error != 0)
		return error;

	/* The kinds of children again, as the table's fix-ups left them. */
	has_block = 0;
	has_inline = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE)
			continue;
		inline_level = box_is_inline_level(child);
		whitespace = box_is_whitespace(child);
		if (!inline_level)
			has_block = 1;
		if (inline_level && !whitespace)
			has_inline = 1;
	}

	/* A flex or grid container's children are its items (ws074-p035, ws074-p072). */
	if (box->kind != LAYOUT_INLINE && (box->style.display == CSS_DISPLAY_FLEX ||
	    box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID)) {
		error = box_flex_items(tree, box);
		return error;
	}

	/* An inline box holding blocks is laid out as a block in this pass. */
	if (box->kind == LAYOUT_INLINE && has_block)
		box->kind = LAYOUT_BLOCK;

	/* Only inline children: the box lays them out in lines. */
	if (!has_block) {
		box->children_inline = 1;
		return 0;
	}

	/* Only block children (and whitespace between them): the whitespace goes. */
	box_anonymous_style(&box->style, &style);
	child = box->first_child;
	box->first_child = NULL;
	box->last_child = NULL;
	anonymous = NULL;
	while (child != NULL) {
		next = child->next;
		child->next = NULL;
		inline_level = box_is_inline_level(child);

		/* A box out of the flow or a float stays where it is among the others, and ends nothing. */
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE) {
			if (anonymous != NULL) {
				box_append(anonymous, child);
			} else {
				box_append(box, child);
			}

			/* On to the next child. */
			child = next;
			continue;
		}

		/* A block ends the anonymous block before it. */
		if (!inline_level) {
			anonymous = NULL;
			box_append(box, child);
			child = next;
			continue;
		}

		/* Whitespace between blocks is dropped; other inline content goes into an anonymous block. */
		whitespace = box_is_whitespace(child);
		if (anonymous == NULL && (whitespace || !has_inline)) {
			child = next;
			continue;
		}

		/* Inline content after a block starts a new anonymous block. */
		if (anonymous == NULL) {
			anonymous = box_new(tree, LAYOUT_ANONYMOUS_BLOCK, NULL, &style);
			if (anonymous == NULL)
				return ENOMEM;
			anonymous->children_inline = 1;
			box_append(box, anonymous);
		}

		/* The inline content goes into the anonymous block. */
		box_append(anonymous, child);
		child = next;
	}

	/* Succeeded: the children are all blocks. */
	return 0;
}

/*
 * Fixes the parts of tables in a box's children (ws074-p037): a table
 * takes its row groups' rows as its own (the groups themselves are left
 * out), a table, a row group and a row wrap what is not theirs in
 * anonymous rows and cells, and another box wraps its runs of rows and
 * cells in anonymous tables.  A flex or grid container's rows and cells
 * are its items instead.
 */
static int
box_table_fix(
	struct layout_tree *tree,
	struct layout_box *box)
{
	int display;
	int error;

	/* What the box is. */
	display = box->style.display;
	if (box->kind == LAYOUT_INLINE)
		return 0;

	/* A table's, a row group's or a row's children. */
	if (display == CSS_DISPLAY_TABLE || display == CSS_DISPLAY_INLINE_TABLE || display == CSS_DISPLAY_TABLE_ROW_GROUP ||
	    display == CSS_DISPLAY_TABLE_ROW) {
		error = box_table_children(tree, box);
		return error;
	}

	/* A flex or grid container's rows and cells are items. */
	if (display == CSS_DISPLAY_FLEX || display == CSS_DISPLAY_INLINE_FLEX ||
	    display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID)
		return 0;

	/* Another box: its runs of table parts in anonymous tables. */
	error = box_wrap_tables(tree, box);
	return error;
}

/*
 * Sorts the children of a table, a row group or a row: the parts that
 * belong there stay, cells outside a row go into anonymous rows, and any
 * other content (whitespace dropped) into anonymous cells, one for each
 * run.
 */
static int
box_table_children(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct layout_box *child;
	struct layout_box *next;
	struct layout_box *row;
	struct layout_box *cell;
	int display;
	int is_row;
	int whitespace;
	int error;

	/* Takes the children off, and puts each back where it belongs. */
	is_row = 0;
	if (box->style.display == CSS_DISPLAY_TABLE_ROW)
		is_row = 1;
	child = box->first_child;
	box->first_child = NULL;
	box->last_child = NULL;
	row = NULL;
	cell = NULL;
	while (child != NULL) {
		next = child->next;
		child->next = NULL;
		display = child->style.display;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK)
			display = CSS_DISPLAY_INLINE;

		/* A box out of the flow or a float stays as it is. */
		if (child->out_of_flow || child->floating != CSS_FLOAT_NONE) {
			box_append(box, child);
			child = next;
			continue;
		}

		/* A row, a row group or a caption stays in a table or a row group; so does a cell in a row. */
		if (!is_row && (display == CSS_DISPLAY_TABLE_ROW || display == CSS_DISPLAY_TABLE_CAPTION || display == CSS_DISPLAY_TABLE_ROW_GROUP)) {
			box_append(box, child);
			row = NULL;
			cell = NULL;
			child = next;
			continue;
		}

		/* A cell stays in a row. */
		if (is_row && display == CSS_DISPLAY_TABLE_CELL) {
			box_append(box, child);
			cell = NULL;
			child = next;
			continue;
		}

		/* Whitespace between the parts goes. */
		whitespace = box_is_whitespace(child);
		if (whitespace && cell == NULL) {
			child = next;
			continue;
		}

		/* Outside a row, the rest goes into an anonymous row. */
		if (!is_row && row == NULL) {
			row = box_anonymous_of(tree, box, CSS_DISPLAY_TABLE_ROW);
			if (row == NULL)
				return ENOMEM;
			box_append(box, row);
			cell = NULL;
		}

		/* A cell outside a row joins the anonymous row. */
		if (display == CSS_DISPLAY_TABLE_CELL) {
			box_append(row, child);
			cell = NULL;
			child = next;
			continue;
		}

		/* Other content goes into an anonymous cell. */
		if (cell == NULL) {
			if (is_row) {
				cell = box_anonymous_of(tree, box, CSS_DISPLAY_TABLE_CELL);
				if (cell == NULL)
					return ENOMEM;
				box_append(box, cell);
			} else {
				cell = box_anonymous_of(tree, row, CSS_DISPLAY_TABLE_CELL);
				if (cell == NULL)
					return ENOMEM;
				box_append(row, cell);
			}
		}

		/* The content joins the cell. */
		box_append(cell, child);
		child = next;
	}

	/* The anonymous cells and rows made here need the fix-ups of any box. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->node != NULL)
			continue;
		error = box_fix_children(tree, child);
		if (error != 0)
			return error;
	}

	/* Succeeded: the children are sorted. */
	return 0;
}

/* Wraps each run of rows, row groups and cells among a box's children (whitespace between them included) in an anonymous table. */
static int
box_wrap_tables(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct layout_box *child;
	struct layout_box *next;
	struct layout_box *table;
	int part;
	int any;
	int whitespace;
	int error;

	/* Nothing to do without table parts. */
	any = 0;
	for (child = box->first_child; child != NULL; child = child->next) {
		part = box_is_table_part(child);
		if (part)
			any = 1;
	}

	/* None: the children stay as they are. */
	if (!any)
		return 0;

	/* Takes the children off, and puts the runs of parts into tables. */
	child = box->first_child;
	box->first_child = NULL;
	box->last_child = NULL;
	table = NULL;
	while (child != NULL) {
		next = child->next;
		child->next = NULL;
		part = box_is_table_part(child);
		whitespace = box_is_whitespace(child);

		/* A part starts or continues a table; whitespace inside a run stays with it. */
		if (part || (whitespace && table != NULL)) {
			if (table == NULL) {
				table = box_anonymous_of(tree, box, CSS_DISPLAY_TABLE);
				if (table == NULL)
					return ENOMEM;
				box_append(box, table);
			}

			/* The child joins the table. */
			box_append(table, child);
			child = next;
			continue;
		}

		/* Anything else ends the run. */
		table = NULL;
		box_append(box, child);
		child = next;
	}

	/* The anonymous tables need their own fix-ups. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->node != NULL || child->style.display != CSS_DISPLAY_TABLE)
			continue;
		error = box_fix_children(tree, child);
		if (error != 0)
			return error;
	}

	/* Succeeded: the runs are tables. */
	return 0;
}

/* Tells whether a box is a row, a row group or a cell in the flow (a part a table must hold). */
static int
box_is_table_part(
	const struct layout_box *box)
{
	/* Boxes out of the flow, floats and what is not a block (text carries its parent's style) are not parts. */
	if (box->out_of_flow || box->floating != CSS_FLOAT_NONE)
		return 0;
	if (box->kind != LAYOUT_BLOCK && box->kind != LAYOUT_ANONYMOUS_BLOCK)
		return 0;

	/* The three displays. */
	if (box->style.display == CSS_DISPLAY_TABLE_ROW)
		return 1;
	if (box->style.display == CSS_DISPLAY_TABLE_ROW_GROUP)
		return 1;
	if (box->style.display == CSS_DISPLAY_TABLE_CELL)
		return 1;

	/* Anything else. */
	return 0;
}

/* Makes an anonymous table box of a display, with the inherited properties of the box it goes in. */
static struct layout_box *
box_anonymous_of(
	struct layout_tree *tree,
	const struct layout_box *parent,
	int display)
{
	struct css_style style;

	/* The anonymous style, of the display. */
	box_anonymous_style(&parent->style, &style);
	style.display = display;
	style.vertical_align = parent->style.vertical_align;

	/* The box. */
	return box_new(tree, LAYOUT_ANONYMOUS_BLOCK, NULL, &style);
}

/* Makes the style of an anonymous block: the parent's inherited properties, initial values otherwise. */
static void
box_anonymous_style(
	const struct css_style *parent,
	struct css_style *style)
{
	/* The initial values, then what inherits. */
	css_initial_style(style);
	style->display = CSS_DISPLAY_BLOCK;
	style->color = parent->color;
	style->font_size = parent->font_size;
	style->font_size_keyword = parent->font_size_keyword;
	style->font_weight = parent->font_weight;
	style->font_italic = parent->font_italic;
	style->generic_family = parent->generic_family;
	memcpy(style->families, parent->families, sizeof(style->families));
	style->family_count = parent->family_count;
	style->line_height = parent->line_height;
	style->text_align = parent->text_align;
	style->text_indent = parent->text_indent;
	style->direction = parent->direction;
	style->writing_mode = parent->writing_mode;
	style->writing_sideways = parent->writing_sideways;
	style->text_orientation = parent->text_orientation;
	style->white_space = parent->white_space;
	style->visibility = parent->visibility;
	style->underline = parent->underline;
}

/* Gives a list item its marker: a bullet, or its number and a period. */
static void
box_marker(
	struct layout_box *item,
	int ordinal)
{
	struct layout_box *box;
	struct layout_box *child;
	char digits[16];
	size_t length;
	size_t index;

	/*
	 * The marker goes on the first line of the item: its own when its
	 * children are inline, otherwise that of its first block (anonymous or
	 * not) with lines, as long as that block is not a list item itself.
	 */
	box = item;
	while (!box->children_inline && box->first_child != NULL) {
		child = box->first_child;

		/* A nested list item keeps its own marker. */
		if (child->style.display == CSS_DISPLAY_LIST_ITEM)
			break;

		/* The marker moves one block down. */
		box = child;
	}

	/* A box that ends up without lines cannot show the marker; the item keeps it. */
	if (!box->children_inline)
		box = item;

	/* The bullets are one character. */
	box->marker_length = 0;
	switch (item->style.list_style) {
	case CSS_LIST_DISC:
		box->marker[0] = BOX_BULLET_DISC;
		box->marker_length = 1;
		break;
	case CSS_LIST_CIRCLE:
		box->marker[0] = BOX_BULLET_CIRCLE;
		box->marker_length = 1;
		break;
	case CSS_LIST_SQUARE:
		box->marker[0] = BOX_BULLET_SQUARE;
		box->marker_length = 1;
		break;
	case CSS_LIST_DECIMAL:
		/* The number and a period. */
		length = 0;
		while (ordinal > 0 && length < 6) {
			digits[length] = (char)('0' + ordinal % 10);
			ordinal /= 10;
			length++;
		}

		/* The digits were gathered from the lowest; the marker reads from the highest. */
		for (index = 0; index < length; index++)
			box->marker[index] = (uint16_t)digits[length - 1U - index];
		box->marker[length] = '.';
		box->marker_length = length + 1U;
		break;
	default:
		break;
	}
}

/*
 * Applies the scale of transform to a box that has no children and no lines
 * (the rest of a transformed subtree is not scaled): the border box shrinks
 * or grows about transform-origin; a border keeps at least one pixel.
 */
static void
box_apply_scale(
	struct layout_box *box)
{
	struct layout_box *child;
	const struct css_length *scale;
	const struct css_length *origin;
	layout_unit frame_x;
	layout_unit frame_y;
	layout_unit border_width;
	layout_unit border_height;
	layout_unit origin_x;
	layout_unit origin_y;
	layout_unit new_width;
	layout_unit new_height;
	layout_unit minimum;
	int index;

	for (child = box->first_child; child != NULL; child = child->next)
		box_apply_scale(child);
	scale = box->style.transform_scale;
	if (box->first_child != NULL || box->lines != NULL)
		return;
	if (box->kind != LAYOUT_BLOCK && !box->atomic && !box->replaced)
		return;
	if (scale[0].unit != CSS_UNIT_PX || scale[1].unit != CSS_UNIT_PX)
		return;
	if (scale[0].value == 1.0f && scale[1].value == 1.0f)
		return;
	if (scale[0].value < 0 || scale[1].value < 0)
		return;
	origin = box->style.transform_origin;
	frame_x = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	frame_y = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	border_width = frame_x + box->width;
	border_height = frame_y + box->height;

	/* The origin, from the border box's top left. */
	origin_x = 0;
	origin_y = 0;
	if (origin[0].unit == CSS_UNIT_PX)
		origin_x = layout_from_px(origin[0].value);
	else if (origin[0].unit == CSS_UNIT_PERCENT)
		origin_x = (layout_unit)((float)border_width * origin[0].value / 100.0f) + layout_from_px(origin[0].offset);
	if (origin[1].unit == CSS_UNIT_PX)
		origin_y = layout_from_px(origin[1].value);
	else if (origin[1].unit == CSS_UNIT_PERCENT)
		origin_y = (layout_unit)((float)border_height * origin[1].value / 100.0f) + layout_from_px(origin[1].offset);

	/* The scaled box, its border no thinner than a pixel. */
	new_width = (layout_unit)((float)border_width * scale[0].value);
	new_height = (layout_unit)((float)border_height * scale[1].value);
	box->x += origin_x - (layout_unit)((float)origin_x * scale[0].value);
	box->y += origin_y - (layout_unit)((float)origin_y * scale[1].value);
	minimum = LAYOUT_UNIT;
	for (index = 0; index < 4; index++) {
		if (box->border[index] > 0) {
			layout_unit scaled;

			scaled = (layout_unit)((float)box->border[index] * scale[index % 2 == 0 ? 1 : 0].value);
			box->border[index] = scaled < minimum ? minimum : scaled;
		}
	}
	frame_x = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	frame_y = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	box->width = new_width > frame_x ? new_width - frame_x : 0;
	box->height = new_height > frame_y ? new_height - frame_y : 0;
}

/*
 * Resolves the translation of a box's transform: pixels, or a percentage of
 * the box's own border box.  It moves the box and its descendants as a
 * relative offset does (rotation, scale and skew are not applied).
 */
static void
box_transform_offset(
	const struct layout_box *box,
	layout_unit *dx,
	layout_unit *dy)
{
	const struct css_length *translate;
	layout_unit border_width;
	layout_unit border_height;

	/* Only a block-level box or an atomic one is transformed. */
	if (box->kind != LAYOUT_BLOCK && !box->atomic && !box->replaced)
		return;
	translate = box->style.transform_translate;
	border_width = box->border[CSS_LEFT] + box->padding[CSS_LEFT] + box->width + box->padding[CSS_RIGHT] + box->border[CSS_RIGHT];
	border_height = box->border[CSS_TOP] + box->padding[CSS_TOP] + box->height + box->padding[CSS_BOTTOM] + box->border[CSS_BOTTOM];
	if (translate[0].unit == CSS_UNIT_PX)
		*dx += layout_from_px(translate[0].value);
	else if (translate[0].unit == CSS_UNIT_PERCENT)
		*dx += (layout_unit)((float)border_width * translate[0].value / 100.0f) + layout_from_px(translate[0].offset);
	if (translate[1].unit == CSS_UNIT_PX)
		*dy += layout_from_px(translate[1].value);
	else if (translate[1].unit == CSS_UNIT_PERCENT)
		*dy += (layout_unit)((float)border_height * translate[1].value / 100.0f) + layout_from_px(translate[1].offset);
}

/* Resolves a relatively positioned box's offsets: left (or minus right), top (or minus bottom); zero otherwise. */
static void
box_relative_offset(
	const struct layout_box *box,
	layout_unit *dx,
	layout_unit *dy)
{
	/* Its containing block is its parent. */
	box_relative_within(box, box->parent, dx, dy);
}

/* Moves the fragments and decorations of a block's lines by the offsets of their relatively positioned inline boxes. */
static void
box_inline_relative(
	struct layout_box *block)
{
	struct layout_line *line;
	const struct layout_box *walk;
	layout_unit dx;
	layout_unit dy;
	layout_unit x;
	layout_unit y;
	size_t index;
	size_t at;
	int depth;

	/* Each fragment and decoration: the sum of its inline ancestors' offsets. */
	for (index = 0; index < block->line_count; index++) {
		line = &block->lines[index];
		for (at = 0; at < line->fragment_count; at++) {
			dx = 0;
			dy = 0;
			for (walk = line->fragments[at].box, depth = 0; walk != NULL && walk != block && depth < 64;
			    walk = walk->parent, depth++) {
				if (walk->kind != LAYOUT_INLINE)
					continue;
				box_relative_within(walk, block, &x, &y);
				dx += x;
				dy += y;
			}
			line->fragments[at].x += dx;
			line->fragments[at].shift += dy;
		}
		for (at = 0; at < line->decoration_count; at++) {
			dx = 0;
			dy = 0;
			for (walk = line->decorations[at].box, depth = 0; walk != NULL && walk != block && depth < 64;
			    walk = walk->parent, depth++) {
				if (walk->kind != LAYOUT_INLINE)
					continue;
				box_relative_within(walk, block, &x, &y);
				dx += x;
				dy += y;
			}
			line->decorations[at].x += dx;
			line->decorations[at].shift += dy;
		}
	}
}

/* Resolves a relatively positioned box's offsets against a containing block; zero for another box. */
static void
box_relative_within(
	const struct layout_box *box,
	const struct layout_box *containing,
	layout_unit *dx,
	layout_unit *dy)
{
	const struct css_length *offset;
	const struct layout_box *parent;
	layout_unit width;
	layout_unit height;
	int definite;

	/* Only a relative box moves. */
	*dx = 0;
	*dy = 0;
	if (box->style.position != CSS_POSITION_RELATIVE)
		return;

	/*
	 * Percentages are of the containing block's width, and of its height when
	 * that is definite (a height of its own, or a flex or grid item's).
	 */
	width = 0;
	height = 0;
	definite = 0;
	parent = containing;
	if (parent != NULL) {
		width = parent->width;
		height = parent->height;
		definite = parent->style.height.unit == CSS_UNIT_PX || parent->style.height.unit == CSS_UNIT_PERCENT;
		if (parent->parent != NULL && !parent->out_of_flow &&
		    (parent->parent->style.display == CSS_DISPLAY_FLEX || parent->parent->style.display == CSS_DISPLAY_INLINE_FLEX ||
		    parent->parent->style.display == CSS_DISPLAY_GRID || parent->parent->style.display == CSS_DISPLAY_INLINE_GRID))
			definite = 1;
	}
	offset = box->style.offset;

	/* Horizontally: left wins over right. */
	if (offset[CSS_LEFT].unit == CSS_UNIT_PX) {
		*dx = layout_from_px(offset[CSS_LEFT].value);
	} else if (offset[CSS_LEFT].unit == CSS_UNIT_PERCENT) {
		*dx = (layout_unit)((float)width * offset[CSS_LEFT].value / 100.0f) + layout_from_px(offset[CSS_LEFT].offset);
	} else if (offset[CSS_RIGHT].unit == CSS_UNIT_PX) {
		*dx = -layout_from_px(offset[CSS_RIGHT].value);
	} else if (offset[CSS_RIGHT].unit == CSS_UNIT_PERCENT) {
		*dx = -((layout_unit)((float)width * offset[CSS_RIGHT].value / 100.0f) + layout_from_px(offset[CSS_RIGHT].offset));
	}

	/* Vertically: top wins over bottom (an indefinite percentage is auto). */
	if (offset[CSS_TOP].unit == CSS_UNIT_PX) {
		*dy = layout_from_px(offset[CSS_TOP].value);
	} else if (offset[CSS_TOP].unit == CSS_UNIT_PERCENT && definite) {
		*dy = (layout_unit)((float)height * offset[CSS_TOP].value / 100.0f) + layout_from_px(offset[CSS_TOP].offset);
	} else if (offset[CSS_BOTTOM].unit == CSS_UNIT_PX) {
		*dy = -layout_from_px(offset[CSS_BOTTOM].value);
	} else if (offset[CSS_BOTTOM].unit == CSS_UNIT_PERCENT && definite) {
		*dy = -((layout_unit)((float)height * offset[CSS_BOTTOM].value / 100.0f) + layout_from_px(offset[CSS_BOTTOM].offset));
	}
}

/*
 * Gives the boxes out of the flow inside a block's inline content their
 * static position (the content's top left), and makes the floats and the
 * inline blocks there absolute (they were placed relative to the content
 * box).
 */
static void
box_static_inline(
	struct layout_box *box,
	layout_unit x,
	layout_unit y)
{
	struct layout_box *child;

	/* The inline boxes are searched through; a box out of the flow or a float is not entered. */
	for (child = box->first_child; child != NULL; child = child->next) {
		if (child->out_of_flow) {
			child->static_x = x;
			child->static_y = y;
			continue;
		}

		/* A float or an inline block moves with the content box. */
		if (child->floating != CSS_FLOAT_NONE || child->atomic) {
			layout_absolute(child, x, y);
			continue;
		}

		/* An inline box's content. */
		box_static_inline(child, x, y);
	}
}
