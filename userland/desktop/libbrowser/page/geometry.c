/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a page's scripts ask the page about its layout and its selectors
 * through the window's host callbacks behind querySelector,
 * getBoundingClientRect, clientWidth, scrollY and the like.
 *
 * A question about the geometry lays the page out first when the document
 * changed since the last layout, at the view's size and with its fonts,
 * as other browsers lay out when a script asks; a page whose view has not
 * given it fonts yet (or whose fonts do not open) has no layout, and its
 * nodes no boxes.  The selectors are matched with a style engine of their
 * own, which lives as long as the page: the styling's engine is made anew
 * at each change of the document.
 *
 * A script also asks for an element's computed style (its
 * box's, or the cascade's from the root down when it has no box: display
 * none, or no layout) and moves the scroll (kept inside the laid out
 * document; the view takes it as its own).
 */

#include "page/page.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The fewest slots the index of boxes has. */
#define GEOMETRY_INDEX_MIN 64U

/* The deepest element whose style is computed without a box. */
#define GEOMETRY_STYLE_DEPTH 512

/*
 * One slot of the page's index of boxes: a node and its first box (an
 * empty slot has no node).
 */
struct page_box_slot {
	const struct dom_node *node;
	const struct layout_box *box;
};

static int geometry_tree_box(const struct layout_tree *tree, const struct layout_box *first, struct dom_node *node, struct bind_box *box);
static int geometry_child_box(struct page *page, struct dom_node *node, struct bind_box *box);
static void geometry_overflow(const struct layout_box *box, int depth, layout_unit *left, layout_unit *top, layout_unit *right, layout_unit *bottom);
static void geometry_scroll_size(const struct layout_box *first, struct bind_box *box);
static int geometry_layout(struct page *page);
static const struct layout_box *geometry_first_box(struct page *page, const struct dom_node *node);
static int geometry_own_place(const struct layout_box *box);
static int geometry_index(struct page *page);
static size_t geometry_count_boxes(const struct layout_box *box, int depth);
static void geometry_index_boxes(struct page *page, const struct layout_box *box, int depth);
static size_t geometry_slot_of(const struct page *page, const struct dom_node *node);
static int geometry_cascade(struct page *page, struct dom_element *element, struct css_style *style);

/*
 * Finds or creates the script selector engine for bind_host.selector_engine.
 * Returns NULL when the engine cannot be made.
 */
struct css_engine *
page_selector_engine(
	void *context)
{
	struct page *page;
	int error;

	/* The engine made before. */
	page = context;
	if (page->query_css != NULL)
		return page->query_css;

	/* A new one; without memory, none. */
	error = css_engine_create(&page->query_css, page->heap);
	if (error != 0) {
		page->query_css = NULL;
		return NULL;
	}

	/* Succeeded: the engine, told of the active view transition. */
	css_engine_set_view_transition(page->query_css, page->vt_active, page->vt_types, page->vt_type_count);
	return page->query_css;
}

/*
 * Finds a node's current geometry for bind_host.node_box.
 * Refreshes changed layout and reports whether the node has a box.
 */
int
page_node_box(
	void *context,
	struct dom_node *node,
	struct bind_box *box)
{
	struct page *page;
	const struct layout_box *first;
	int laid_out;
	int found;

	/* The Page owns fonts and primary layout resources for this synchronous query. */
	page = context;
	memset(box, 0, sizeof(*box));

	/* Foreign nodes use their own active child cascade and actual content viewport. */
	if (node->document != page->document) {
		found = geometry_child_box(page, node, box);
		if (!found)
			return 0;

		/* Succeeded: the active child supplied copied geometry. */
		return 1;
	}

	/* Primary layout keeps its existing indexed first-box lookup. */
	laid_out = geometry_layout(page);
	if (!laid_out)
		return 0;

	/* Resolves the first indexed box and its current native bounds. */
	first = geometry_first_box(page, node);

	/* A successful layout distinguishes absent boxes from unavailable geometry. */
	if (first == NULL)
		box->no_generated_box = 1;

	/* Copies any ordinary bounds separately from the no-box proof. */
	found = geometry_tree_box(&page->layout, first, node, box);
	if (!found)
		return 0;

	/* Succeeded: the current native primary layout supplied the node's geometry. */
	return 1;
}

/*
 * Copies a primary Grid's final used tracks into caller-owned native storage.
 * No borrowed arena pointer escapes the query or a later layout generation.
 */
int
page_grid_tracks(
	void *context,
	struct dom_node *node,
	int columns,
	struct wb_vector *sizes,
	int *leading,
	int *repeats,
	int *found)
{
	struct page *page;
	const struct layout_box *box;
	const double *tracks;
	int count;
	int index;
	int laid_out;
	int error;

	/* Child Documents use transient geometry trees and need a separate owned-copy path. */
	page = context;
	*found = 0;
	*leading = 0;
	*repeats = 0;
	if (node->document != page->document)
		return 0;

	/* Flush style and layout before reading the current generation's solver output. */
	laid_out = geometry_layout(page);
	if (!laid_out)
		return 0;

	/* Only a real supported Grid snapshot supplies used rather than computed tracks. */
	box = geometry_first_box(page, node);
	if (box == NULL)
		return 0;

	/* Unsupported axes and non-Grid boxes retain the existing computed-value fallback. */
	if (!box->grid_used_valid)
		return 0;

	/* Columns and rows are independent logical track sequences. */
	tracks = box->grid_used_rows;
	count = box->grid_used_row_count;
	*leading = box->grid_leading_rows;
	*repeats = box->style.row_repeat.count;
	if (columns) {
		tracks = box->grid_used_columns;
		count = box->grid_used_column_count;
		*leading = box->grid_leading_columns;
		*repeats = box->style.column_repeat.count;
	}

	/* Copy every value while the current arena is still alive and unchanged. */
	for (index = 0; index < count; index++) {
		error = wb_vector_push(sizes, &tracks[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: even an empty sequence is a resolved Grid axis rather than a missing box. */
	*found = 1;
	return 0;
}

/*
 * Finds the laid-out document dimensions for bind_host.document_size.
 * Refreshes changed layout and reports zero when it cannot be laid out.
 */
void
page_document_size(
	void *context,
	double *width,
	double *height)
{
	struct page *page;
	int laid_out;

	/* Nothing until the page is laid out. */
	page = context;
	*width = 0.0;
	*height = 0.0;
	laid_out = geometry_layout(page);
	if (!laid_out)
		return;

	/* The viewport's width, and the document's height. */
	*width = layout_to_px(page->layout.viewport_width);
	*height = layout_to_px(page->layout.document_height);

	/* Succeeded: both dimensions reflect the current layout. */
	return;
}

/*
 * Finds the view's scroll offset in CSS pixels for bind_host.scroll.
 */
void
page_scroll(
	void *context,
	double *x,
	double *y)
{
	struct page *page;

	/* What the view last said. */
	page = context;
	*x = page->scroll_x;
	*y = page->scroll_y;

	/* Succeeded: the caller receives the last reported view offsets. */
	return;
}

/*
 * Finds the top element at a point in viewport coordinates.
 */
struct dom_node *
page_element_at(
	void *context,
	double x,
	double y)
{
	struct page *page;
	struct dom_node *node;
	int laid_out;

	/* Points outside the viewport do not hit the document. */
	page = context;
	if (!(x >= 0.0) ||
	    !(y >= 0.0) ||
	    x >= page->viewport_width ||
	    y >= page->viewport_height)
		return NULL;

	/* Uses only a current successful layout for hit testing. */
	laid_out = geometry_layout(page);
	if (!laid_out)
		return NULL;

	/* Layout uses document coordinates; DOM hit testing returns an element. */
	x += page->scroll_x;
	y += page->scroll_y;
	node = layout_hit_node(&page->layout, (layout_unit)(x * LAYOUT_UNIT), (layout_unit)(y * LAYOUT_UNIT));
	while (node != NULL && node->type != DOM_ELEMENT)
		node = node->parent;
	if (node == NULL)
		return NULL;

	/* Succeeded: the hit resolves to an actual DOM element. */
	return node;
}

/*
 * Computes an element's style for bind_host.computed_style.
 * Uses its box when available and otherwise the cascade; reports ENOENT
 * for an element outside the document.
 */
int
page_computed_style(
	void *context,
	struct dom_element *element,
	struct css_style *style)
{
	const struct layout_box *box;
	const struct dom_node *walk;
	struct page *page;
	int laid_out;
	int error;

	/* An element outside the document has no style. */
	page = context;
	for (walk = &element->node; walk->parent != NULL; walk = walk->parent)
		continue;
	if (walk != &page->document->node)
		return ENOENT;

	/* The box's style, when the element has a box. */
	laid_out = geometry_layout(page);
	box = NULL;
	if (laid_out)
		box = geometry_first_box(page, &element->node);
	if (box != NULL) {
		*style = box->style;
		return 0;
	}

	/* Otherwise the cascade's. */
	error = geometry_cascade(page, element, style);
	if (error != 0)
		return error;

	/* Succeeded: the style. */
	return 0;
}

/*
 * Moves the page's scroll within document bounds for bind_host.scroll_to.
 * Does not scroll sideways or past the last viewport of the document.
 */
void
page_scroll_to(
	void *context,
	double x,
	double y)
{
	struct page *page;
	double limit;
	int laid_out;

	UNUSED_PARAMETER(x);

	/* The document's height less the view's is the furthest down. */
	page = context;
	limit = 0.0;
	laid_out = geometry_layout(page);
	if (laid_out)
		limit = layout_to_px(page->layout.document_height) - page->viewport_height;
	if (limit < 0.0)
		limit = 0.0;

	/* The place, within the limits (a number that is not one is the top). */
	if (!(y >= 0.0))
		y = 0.0;
	if (y > limit)
		y = limit;

	/* Succeeded: the view takes it. */
	page->scroll_x = 0.0;
	page->scroll_y = y;
	page->scroll_requested = 1;

	/* Succeeded: the view will consume the bounded scroll request. */
	return;
}

/*
 * Frees the page's index of boxes.
 */
void
page_box_index_release(
	struct page *page)
{
	/* The slots. */
	free(page->box_index);
	page->box_index = NULL;
	page->box_index_capacity = 0;

	/* Succeeded: no native box pointer remains cached. */
	return;
}

/* Serializes the same actual border/content geometry from primary or child box trees. */
static int
geometry_tree_box(
	const struct layout_tree *tree,
	const struct layout_box *first,
	struct dom_node *node,
	struct bind_box *box)
{
	struct layout_rect rect;
	int own;
	int found;
	int block;

	/* A block's own border box differs from the union of ordinary inline fragments. */
	own = geometry_own_place(first);
	if (own) {
		rect.x = first->x;
		rect.y = first->y;
		rect.width = first->border[CSS_LEFT] + first->padding[CSS_LEFT] + first->width + first->padding[CSS_RIGHT] + first->border[CSS_RIGHT];
		rect.height = first->border[CSS_TOP] + first->padding[CSS_TOP] + first->height + first->padding[CSS_BOTTOM] + first->border[CSS_BOTTOM];
	} else {
		found = layout_node_bounds(tree, node, &rect);
		if (!found)
			return 0;
	}

	/* The rectangle in pixels. */
	box->x = layout_to_px(rect.x);
	box->y = layout_to_px(rect.y);
	box->width = layout_to_px(rect.width);
	box->height = layout_to_px(rect.height);

	/* The first box: a block (or a replaced or atomic box) has a client area and borders, an inline box not. */
	block = 0;
	if (first != NULL) {
		if (first->kind == LAYOUT_BLOCK) {
			block = 1;
		} else if (first->kind == LAYOUT_REPLACED) {
			block = 1;
		} else if (first->atomic) {
			block = 1;
		}
	}

	/* Its borders, in the order top, right, bottom, left. */
	box->block = block;
	if (block) {
		box->border_top = layout_to_px(first->border[0]);
		box->border_right = layout_to_px(first->border[1]);
		box->border_bottom = layout_to_px(first->border[2]);
		box->border_left = layout_to_px(first->border[3]);
	}

	/* Its used margins and paddings. */
	if (first != NULL) {
		box->margin_top = layout_to_px(first->margin[0]);
		box->margin_right = layout_to_px(first->margin[1]);
		box->margin_bottom = layout_to_px(first->margin[2]);
		box->margin_left = layout_to_px(first->margin[3]);
		box->padding_top = layout_to_px(first->padding[0]);
		box->padding_right = layout_to_px(first->padding[1]);
		box->padding_bottom = layout_to_px(first->padding[2]);
		box->padding_left = layout_to_px(first->padding[3]);
		box->inset_known = first->used_inset_known;
		box->inset[0] = layout_to_px(first->used_inset[0]);
		box->inset[1] = layout_to_px(first->used_inset[1]);
		box->inset[2] = layout_to_px(first->used_inset[2]);
		box->inset[3] = layout_to_px(first->used_inset[3]);
	}

	/* What a block scrolls over. */
	if (first != NULL && block && first->kind == LAYOUT_BLOCK)
		geometry_scroll_size(first, box);

	/* Succeeded: the node has a box. */
	return 1;
}

/*
 * Adds the border boxes of a box's children in the flow and its absolutely
 * positioned ones when it is positioned (and their descendants that do not
 * clip) to a rectangle; a flex or grid item's margins count too.
 */
static void
geometry_overflow(
	const struct layout_box *box,
	int depth,
	layout_unit *left,
	layout_unit *top,
	layout_unit *right,
	layout_unit *bottom)
{
	const struct layout_box *child;
	layout_unit x0;
	layout_unit y0;
	layout_unit x1;
	layout_unit y1;
	int items;

	/* Stops at the depth the layout stops at; lines of text are within their block. */
	if (depth > 64 || box->children_inline)
		return;
	items = box->style.display == CSS_DISPLAY_FLEX || box->style.display == CSS_DISPLAY_INLINE_FLEX ||
	    box->style.display == CSS_DISPLAY_GRID || box->style.display == CSS_DISPLAY_INLINE_GRID;
	for (child = box->first_child; child != NULL; child = child->next) {
		/* A box out of the flow counts when this box (being positioned) is where it is placed. */
		if (child->out_of_flow &&
		    (child->style.position != CSS_POSITION_ABSOLUTE || box->style.position == CSS_POSITION_STATIC))
			continue;
		if (child->kind != LAYOUT_BLOCK && child->kind != LAYOUT_ANONYMOUS_BLOCK && child->kind != LAYOUT_REPLACED)
			continue;
		x0 = child->x;
		y0 = child->y;
		x1 = x0 + child->border[CSS_LEFT] + child->padding[CSS_LEFT] + child->width + child->padding[CSS_RIGHT] +
		    child->border[CSS_RIGHT];
		y1 = y0 + child->border[CSS_TOP] + child->padding[CSS_TOP] + child->height + child->padding[CSS_BOTTOM] +
		    child->border[CSS_BOTTOM];
		if (items && !child->out_of_flow) {
			x0 -= child->margin[CSS_LEFT] > 0 ? child->margin[CSS_LEFT] : 0;
			y0 -= child->margin[CSS_TOP] > 0 ? child->margin[CSS_TOP] : 0;
			x1 += child->margin[CSS_RIGHT] > 0 ? child->margin[CSS_RIGHT] : 0;
			y1 += child->margin[CSS_BOTTOM] > 0 ? child->margin[CSS_BOTTOM] : 0;
		}
		if (x0 < *left)
			*left = x0;
		if (y0 < *top)
			*top = y0;
		if (x1 > *right)
			*right = x1;
		if (y1 > *bottom)
			*bottom = y1;
		if (!layout_clips(child))
			geometry_overflow(child, depth + 1, left, top, right, bottom);
	}
}

/*
 * Finds what a block scrolls over: its padding box, and its content's
 * overflow with the padding at the far sides; overflow before the start
 * of a block container cannot be scrolled to, a flex or grid container's
 * can.
 */
static void
geometry_scroll_size(
	const struct layout_box *first,
	struct bind_box *box)
{
	layout_unit pad_left;
	layout_unit pad_top;
	layout_unit pad_right;
	layout_unit pad_bottom;
	layout_unit left;
	layout_unit top;
	layout_unit right;
	layout_unit bottom;
	int flex;
	int rtl;
	int wrap_reverse;
	int reversed_x;
	int reversed_y;

	/* The padding box. */
	pad_left = first->x + first->border[CSS_LEFT];
	pad_top = first->y + first->border[CSS_TOP];
	pad_right = pad_left + first->padding[CSS_LEFT] + first->width + first->padding[CSS_RIGHT];
	pad_bottom = pad_top + first->padding[CSS_TOP] + first->height + first->padding[CSS_BOTTOM];

	/* The content, with the padding around it. */
	left = pad_right;
	top = pad_bottom;
	right = pad_left;
	bottom = pad_top;
	geometry_overflow(first, 0, &left, &top, &right, &bottom);
	if (right > left) {
		left -= first->padding[CSS_LEFT];
		right += first->padding[CSS_RIGHT];
	}
	if (bottom > top) {
		top -= first->padding[CSS_TOP];
		bottom += first->padding[CSS_BOTTOM];
	}

	/*
	 * A container scrolls only toward its end sides: a block's or a grid's
	 * follow its writing mode and direction, a horizontal flex container's
	 * its main and cross axes (reversed ones scroll toward their starts
	 * instead); a vertical flex container's overflow all counts.
	 */
	flex = first->style.display == CSS_DISPLAY_FLEX || first->style.display == CSS_DISPLAY_INLINE_FLEX;
	if (!flex) {
		if (first->style.direction == CSS_DIRECTION_RTL || first->style.writing_mode == CSS_WRITING_VERTICAL_RL)
			right = pad_right;
		else
			left = pad_left;
		top = pad_top;
	} else if (flex && first->style.writing_mode == CSS_WRITING_HORIZONTAL_TB) {
		rtl = first->style.direction == CSS_DIRECTION_RTL;
		wrap_reverse = first->style.flex_wrap == CSS_FLEX_WRAP_REVERSE;
		if (first->style.flex_direction == CSS_FLEX_ROW || first->style.flex_direction == CSS_FLEX_ROW_REVERSE) {
			reversed_x = rtl != (first->style.flex_direction == CSS_FLEX_ROW_REVERSE);
			reversed_y = wrap_reverse;
		} else {
			reversed_x = rtl != wrap_reverse;
			reversed_y = first->style.flex_direction == CSS_FLEX_COLUMN_REVERSE;
		}
		if (reversed_x)
			right = pad_right;
		else
			left = pad_left;
		if (reversed_y)
			bottom = pad_bottom;
		else
			top = pad_top;
	}
	if (left > pad_left)
		left = pad_left;
	if (top > pad_top)
		top = pad_top;
	if (right < pad_right)
		right = pad_right;
	if (bottom < pad_bottom)
		bottom = pad_bottom;

	/* In pixels. */
	box->scroll_width = layout_to_px(right - left);
	box->scroll_height = layout_to_px(bottom - top);
}

/* Builds a transient child layout without retaining native DOM edges in a C cache. */
static int
geometry_child_box(
	struct page *page,
	struct dom_node *node,
	struct bind_box *box)
{
	struct bind_window *window;
	struct dom_document *document;
	struct css_engine *engine;
	struct layout_tree tree;
	const struct layout_box *first;
	struct vm_cell *roots[2];
	unsigned index;
	int width;
	int height;
	int connected;
	int status;
	int found;
	int laid_out;

	/* Only a connected node of an actual binding-owned child can have child geometry. */
	document = node->document;
	window = document->view;
	if (window == NULL || document->context == NULL)
		return 0;
	connected = dom_is_inclusive_ancestor(&node->document->node, node);
	if (!connected)
		return 0;

	/* Owner Document and queried node survive parent layout callbacks that retire the child. */
	roots[0] = &document->node.cell;
	roots[1] = &node->cell;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(page->heap, &roots[index]);
		if (status != 0) {
			/* Failed root publication releases only slots that were registered earlier. */
			while (index != 0) {
				index--;
				vm_heap_remove_root(page->heap, &roots[index]);
			}

			/* No partial root registration authorizes child layout. */
			return 0;
		}
	}

	/* Opens borrowed fonts only after the queried native graph is rooted. */
	found = 0;
	engine = NULL;
	laid_out = geometry_layout(page);
	if (!laid_out)
		goto release_roots;

	/* Rejects retirement after the actual viewport refresh callback. */
	status = bind_window_child_styles(window, &engine, &width, &height);
	if (status != 0)
		goto release_roots;

	/* A callback cannot publish layout for an adopted node or retired owner. */
	if (node->document != document || document->view != window)
		goto release_roots;
	if (engine == NULL)
		goto release_roots;

	/* Checked bounds precede signed fixed-unit viewport multiplication. */
	if (width >= 0 &&
	    height >= 0 &&
	    width <= INT_MAX / LAYOUT_UNIT &&
	    height <= INT_MAX / LAYOUT_UNIT) {
		status = layout_build(&tree, engine, &page->text, document, page_image_of, page_image_by_url, page, width, height, NULL);
		if (status != 0) {
			layout_release(&tree);
			goto release_roots;
		}

		/* Copies actual child bounds before releasing the transient arena. */
		first = layout_box_of(&tree, node);

		/* Retains only a scalar absence proof after this child's arena is released. */
		if (first == NULL)
			box->no_generated_box = 1;

		/* Copies available bounds without changing the ordinary found contract. */
		found = geometry_tree_box(&tree, first, node, box);
		layout_release(&tree);
	}

release_roots:
	/* No queried DOM or child context remains rooted by this observation. */
	for (index = 0; index < 2U; index++)
		vm_heap_remove_root(page->heap, &roots[index]);

	/* No ordinary bounds were copied; a separate absence proof may still be present. */
	if (!found)
		return 0;

	/* Succeeded: actual child layout supplied a copied native box without retained storage. */
	return 1;
}

/* Lays the primary Page out with its real fonts before synchronous geometry queries. */
static int
geometry_layout(
	struct page *page)
{
	int changed;
	int error;

	/* A page laid out as it is now. */
	changed = page_needs_layout(page);
	if (!changed)
		return 1;

	/* A page whose view has given it no fonts or no size yet cannot be laid out. */
	if (page->font_paths == NULL)
		return 0;
	if (page->viewport_width <= 0 || page->viewport_height <= 0)
		return 0;

	/* The fonts, opened once. */
	error = page_open_fonts(page, page->font_paths);
	if (error != 0)
		return 0;

	/* The layout; one that fails leaves the page without boxes. */
	error = page_layout(page, page->viewport_width, page->viewport_height);
	if (error != 0)
		return 0;

	/* Succeeded: the page is laid out. */
	return 1;
}

/*
 * Finds a node's first box (in tree order) in the page's layout through
 * the index, made again after each layout; NULL when the node has none.
 * Without memory for the index the layout is searched.
 */
static const struct layout_box *
geometry_first_box(
	struct page *page,
	const struct dom_node *node)
{
	const struct layout_box *box;
	size_t slot;
	int error;

	/* The index of this layout. */
	error = geometry_index(page);
	if (error != 0) {
		/* Index allocation failure retains the ordinary tree-search fallback. */
		box = layout_box_of(&page->layout, node);
		if (box == NULL)
			return NULL;

		/* Succeeded: the layout supplies the box without a cached index. */
		return box;
	}

	/* Reads the node's indexed first box, or its empty slot. */
	slot = geometry_slot_of(page, node);
	box = page->box_index[slot].box;
	if (box == NULL)
		return NULL;

	/* Succeeded: the index supplies the node's first box. */
	return box;
}

/*
 * Tells whether a node's first box has a place of its own that is the
 * node's rectangle: a block, or a replaced box outside a line.
 */
static int
geometry_own_place(
	const struct layout_box *box)
{
	/* No box. */
	if (box == NULL)
		return 0;

	/* A block, atomic or not. */
	if (box->kind == LAYOUT_BLOCK)
		return 1;

	/* A replaced box among inline content is a piece of its line. */
	if (box->kind != LAYOUT_REPLACED)
		return 0;
	if (box->parent != NULL && box->parent->children_inline)
		return 0;

	/* Succeeded: a replaced box that stands as a block. */
	return 1;
}

/*
 * Makes the index of the page's boxes when the layout changed since it
 * was made: twice as many slots as boxes, a power of two.
 */
static int
geometry_index(
	struct page *page)
{
	size_t count;
	size_t capacity;

	/* An index of this layout stays. */
	if (page->box_index != NULL && page->box_index_serial == page->layout_serial)
		return 0;

	/* The slots for the boxes there are. */
	count = 0;
	if (page->layout.root != NULL)
		count = geometry_count_boxes(page->layout.root, 0);
	capacity = GEOMETRY_INDEX_MIN;
	while (capacity < count * 2U)
		capacity *= 2U;

	/* Empty slots. */
	page_box_index_release(page);
	page->box_index = calloc(capacity, sizeof(*page->box_index));
	if (page->box_index == NULL)
		return ENOMEM;

	/* Publishes the allocated empty table before inserting native boxes. */
	page->box_index_capacity = capacity;

	/* Inserts each node's first box in tree order. */
	if (page->layout.root != NULL)
		geometry_index_boxes(page, page->layout.root, 0);
	page->box_index_serial = page->layout_serial;

	/* Succeeded: the index describes the current layout serial. */
	return 0;
}

/* Counts a box and its descendants, down to the depth the layout stops at. */
static size_t
geometry_count_boxes(
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	size_t count;

	/* Past the depth there are none. */
	if (depth > LAYOUT_DEPTH_MAX)
		return 0;

	/* The box and each child's. */
	count = 1;
	for (child = box->first_child; child != NULL; child = child->next)
		count += geometry_count_boxes(child, depth + 1);

	/* Succeeded: the count includes every box within the layout depth bound. */
	return count;
}

/* Puts a box, then its descendants in tree order, in the index where their nodes have no box yet. */
static void
geometry_index_boxes(
	struct page *page,
	const struct layout_box *box,
	int depth)
{
	const struct layout_box *child;
	size_t slot;

	/* Past the depth the layout stops at. */
	if (depth > LAYOUT_DEPTH_MAX)
		return;

	/* The node's first box. */
	if (box->node != NULL) {
		slot = geometry_slot_of(page, box->node);
		if (page->box_index[slot].node == NULL) {
			page->box_index[slot].node = box->node;
			page->box_index[slot].box = box;
		}
	}

	/* Each child. */
	for (child = box->first_child; child != NULL; child = child->next)
		geometry_index_boxes(page, child, depth + 1);

	/* Succeeded: every eligible descendant has its first-box index entry. */
	return;
}

/* Finds a node's slot in the index: its own, or the empty one it would take. */
static size_t
geometry_slot_of(
	const struct page *page,
	const struct dom_node *node)
{
	size_t mask;
	size_t slot;

	/* From the hash of the node's address, on to the next slot while another node has it. */
	mask = page->box_index_capacity - 1U;
	slot = (size_t)((((uintptr_t)node >> 4) * 2654435761U) & mask);
	while (page->box_index[slot].node != NULL && page->box_index[slot].node != node)
		slot = (slot + 1U) & mask;

	/* Succeeded: the slot is owned by this node or remains empty. */
	return slot;
}

/*
 * Computes an element's style with the cascade, from the root element
 * down to it (an element without a box has no style of its own kept).
 */
static int
geometry_cascade(
	struct page *page,
	struct dom_element *element,
	struct css_style *style)
{
	struct dom_element **chain;
	struct css_style *parent;
	const struct css_style *inherited;
	struct dom_node *walk;
	size_t count;
	size_t index;
	int error;

	/* The styles as the document and its sheets are now. */
	error = page_update_styles(page);
	if (error != 0)
		return error;

	/* The element's ancestors that are elements, it first. */
	chain = malloc(GEOMETRY_STYLE_DEPTH * sizeof(*chain));
	if (chain == NULL)
		return ENOMEM;

	/* Records element ancestors within the existing finite depth bound. */
	count = 0;
	for (walk = &element->node;
	     walk != NULL &&
	     walk->type == DOM_ELEMENT &&
	     count < GEOMETRY_STYLE_DEPTH;
	     walk = walk->parent) {
		chain[count] = (struct dom_element *)walk;
		count++;
	}

	/* The parent's style, which each step computes the next one from. */
	parent = malloc(sizeof(*parent));
	if (parent == NULL) {
		free(chain);
		return ENOMEM;
	}

	/* Each one from the root down; the last is the element's. */
	inherited = NULL;
	for (index = count; index > 0; index--) {
		error = css_engine_compute(page->css, chain[index - 1U], inherited, style);
		if (error != 0) {
			free(parent);
			free(chain);
			return error;
		}

		/* The next one inherits from this one. */
		*parent = *style;
		inherited = parent;
	}

	/* The chain and the copy are no longer needed. */
	free(parent);
	free(chain);

	/* Succeeded: the style. */
	return 0;
}
