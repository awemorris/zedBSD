/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The record of what changed in a document's tree.
 *
 * Every change of the tree (a node inserted or removed, characters, an
 * attribute, a control's state, a scroll position) grows the document's
 * generation, as it always did, and leaves a trace for the style and the
 * layout: bits in the node it happened to, and a summary in the document.
 * The bits say what must be computed again (DOM_CHANGED_*): a node's style,
 * its whole subtree's, its following siblings', or its layout; a *_BELOW
 * bit on every ancestor tells a walk where to look, so that finding the
 * changed nodes costs what they cost and not what the tree weighs.
 *
 * Bits are set on nodes that are reached from the document through
 * children only: a change inside a shadow tree is recorded on its host (the
 * nearest one to the document when shadow trees nest), as a change of the
 * host's whole subtree, and a change of a tree that is not in the document
 * is not recorded at all (its nodes get their bits when they are inserted).
 * A bit that is no longer needed may stay set (it only makes the next pass
 * look at more); a bit that is needed is never missing.
 *
 * The summary: change_sheets (the set of style sheets may have changed),
 * change_full_style and change_full_layout (compute everything again), and
 * change_count (how many changes are recorded; past DOM_CHANGE_LIMIT the
 * record stops telling them apart and asks for everything).
 */

#include "dom/dom.h"

/* How deep the search for style and link elements goes before it says there may be one. */
#define CHANGE_SCAN_DEPTH	512U

static struct dom_node *change_anchor(struct dom_node *node, int *connected);
static void change_mark(struct dom_node *node, unsigned bits);
static int change_is_sheet(const struct dom_node *node);
static int change_has_sheet(const struct dom_node *node, unsigned depth);
static void change_clear_subtree(struct dom_node *root);

/*
 * Records a change of a document's tree: the node's bits and the document's
 * summary.  The document's generation grows as it always did, except for a
 * resource that arrived (the page's own count of them tells the layout).
 * child is the node that came in or went out for DOM_CHANGE_CHILDREN (NULL
 * when it is not known), attribute the changed attribute's name for
 * DOM_CHANGE_ATTRIBUTE (it may be NULL).  A node outside the document's
 * tree only grows the generation.
 */
void
dom_note_change(
	struct dom_node *node,
	enum dom_change_kind kind,
	struct dom_node *child,
	const struct vm_string *attribute)
{
	struct dom_document *document;
	struct dom_node *anchor;
	struct dom_node *parent;
	unsigned bits;
	int connected;

	/* The generation, as before. */
	document = node->document;
	if (kind != DOM_CHANGE_RESOURCE)
		document->generation++;
	(void)attribute;

	/* A change that reaches the whole document. */
	if (kind == DOM_CHANGE_DOCUMENT) {
		document->change_count++;
		document->change_full_style = 1;
		document->change_full_layout = 1;
		return;
	}
	if (kind == DOM_CHANGE_SHEETS) {
		document->change_count++;
		document->change_sheets = 1;
		document->change_full_style = 1;
		document->change_full_layout = 1;
		return;
	}

	/* A resource of the document that no element is named for: every box that shows one is laid out again. */
	if (kind == DOM_CHANGE_RESOURCE && node->type == DOM_DOCUMENT) {
		document->change_count++;
		document->change_full_layout = 1;
		return;
	}

	/* The sheets: a style or link element changed, or went in or out. */
	if (kind == DOM_CHANGE_CHILDREN) {
		if (change_is_sheet(node) || (child != NULL && change_has_sheet(child, 0U)))
			document->change_sheets = 1;
	} else if (kind == DOM_CHANGE_TEXT) {
		if (node->parent != NULL && change_is_sheet(node->parent))
			document->change_sheets = 1;
	} else if (kind == DOM_CHANGE_ATTRIBUTE) {
		if (change_is_sheet(node))
			document->change_sheets = 1;
	}

	/* A comment has no box and no style: nothing needs computing again. */
	if (kind == DOM_CHANGE_TEXT && node->type == DOM_COMMENT)
		return;

	/* The node, or the host a shadow tree's change is recorded on; a tree outside the document records nothing. */
	anchor = change_anchor(node, &connected);
	if (!connected) {
		if (kind == DOM_CHANGE_CHILDREN && child != NULL && child->parent == NULL)
			change_clear_subtree(child);
		return;
	}

	/* Too many changes of the tree to tell apart: everything is computed again. */
	document->change_count++;
	if (document->change_count > DOM_CHANGE_LIMIT) {
		document->change_full_style = 1;
		document->change_full_layout = 1;
		if (kind == DOM_CHANGE_CHILDREN && child != NULL && child->parent == NULL)
			change_clear_subtree(child);
		return;
	}

	/* A change inside a shadow tree is one of its host's whole subtree. */
	if (anchor != node) {
		change_mark(anchor, DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT);
		if (kind == DOM_CHANGE_CHILDREN && child != NULL && child->parent == NULL)
			change_clear_subtree(child);
		return;
	}

	/* What the kind of change leaves to compute again. */
	switch (kind) {
	case DOM_CHANGE_CHILDREN:
		/* The parent's other children may match a different :nth-child or sibling selector now. */
		change_mark(node, DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT);
		if (child != NULL) {
			if (child->parent == node)
				change_mark(child, DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT);
			else if (child->parent == NULL)
				change_clear_subtree(child);
		}
		break;

	case DOM_CHANGE_TEXT:
		/* The text's own box, and the parent's (:empty, and the parent's inline layout). */
		change_mark(node, DOM_CHANGED_LAYOUT);
		parent = node->parent;
		if (parent != NULL)
			change_mark(parent, DOM_CHANGED_STYLE | DOM_CHANGED_LAYOUT);
		break;

	case DOM_CHANGE_ATTRIBUTE:
	case DOM_CHANGE_STATE:
		/* Selectors look at attributes and states of the element, its ancestors and its preceding siblings. */
		bits = DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_STYLE_SIBLINGS | DOM_CHANGED_LAYOUT;
		change_mark(node, bits);
		break;

	case DOM_CHANGE_SHADOW:
		change_mark(node, DOM_CHANGED_STYLE_SUBTREE | DOM_CHANGED_LAYOUT);
		break;

	case DOM_CHANGE_SCROLL:
	case DOM_CHANGE_RESOURCE:
		/* No style depends on them. */
		change_mark(node, DOM_CHANGED_LAYOUT);
		break;

	default:
		document->change_full_style = 1;
		document->change_full_layout = 1;
		break;
	}
}

/* Tells whether the changes since the style was last read may have changed the set of style sheets. */
int
dom_change_needs_sheets(
	const struct dom_document *document)
{
	return document->change_sheets != 0U;
}

/*
 * Visits, in tree order, the nodes that have style bits (visit gets the node
 * and the style bits it has), then clears every style bit and the summary
 * of the style changes (change_sheets and change_full_style) and the count.
 * visit may be NULL, which only clears.  A visit that returns another value
 * than 0 is the last one called; the bits are cleared all the same, and the
 * value is returned.  A visit must not change the tree.
 */
int
dom_changes_take_style(
	struct dom_document *document,
	dom_change_visit visit,
	void *context)
{
	struct dom_node *node;
	unsigned bits;
	int result;

	/* The tree's nodes that have style bits, and the ones above them. */
	result = 0;
	node = &document->node;
	if ((node->change & (DOM_CHANGED_STYLE_BITS | DOM_CHANGED_STYLE_BELOW)) != 0U) {
		for (;;) {
			bits = node->change;
			node->change &= ~(DOM_CHANGED_STYLE_BITS | DOM_CHANGED_STYLE_BELOW);
			if ((bits & DOM_CHANGED_STYLE_BITS) != 0U && visit != NULL && result == 0)
				result = visit(context, node, bits & DOM_CHANGED_STYLE_BITS);

			/* Below it, when something there has bits. */
			if ((bits & DOM_CHANGED_STYLE_BELOW) != 0U && node->first_child != NULL) {
				node = node->first_child;
				continue;
			}

			/* The next node after its subtree, or the end. */
			for (;;) {
				if (node == &document->node)
					goto done;
				if (node->next != NULL) {
					node = node->next;
					break;
				}
				node = node->parent;
			}
		}
	}

done:
	/* The summary the style pass has read. */
	document->change_sheets = 0U;
	document->change_full_style = 0U;
	document->change_count = 0U;
	return result;
}

/* Clears the layout bits and the summary of the layout changes (after a layout that read them). */
void
dom_changes_clear_layout(
	struct dom_document *document)
{
	struct dom_node *node;
	unsigned bits;

	/* The tree's nodes that have layout bits, and the ones above them. */
	node = &document->node;
	if ((node->change & (DOM_CHANGED_LAYOUT | DOM_CHANGED_LAYOUT_BELOW)) != 0U) {
		for (;;) {
			bits = node->change;
			node->change &= ~(DOM_CHANGED_LAYOUT | DOM_CHANGED_LAYOUT_BELOW);

			/* Below it, when something there has bits. */
			if ((bits & DOM_CHANGED_LAYOUT_BELOW) != 0U && node->first_child != NULL) {
				node = node->first_child;
				continue;
			}

			/* The next node after its subtree, or the end. */
			for (;;) {
				if (node == &document->node)
					goto done;
				if (node->next != NULL) {
					node = node->next;
					break;
				}
				node = node->parent;
			}
		}
	}

done:
	/* The summary the layout has read. */
	document->change_full_layout = 0U;
	document->change_count = 0U;
}

/*
 * Finds the node a change of a node is recorded on, and whether it is in
 * the document's tree: the node itself when its ancestors reach the
 * document through children, otherwise the host of the shadow tree it is in
 * (the nearest to the document when they nest).
 */
static struct dom_node *
change_anchor(
	struct dom_node *node,
	int *connected)
{
	struct dom_node *anchor;
	struct dom_node *top;
	struct dom_shadow_root *shadow;

	/* Up to the root of the tree, and through the host of a shadow root. */
	anchor = node;
	top = node;
	for (;;) {
		while (top->parent != NULL)
			top = top->parent;
		shadow = dom_shadow_of(top);
		if (shadow == NULL || shadow->host == NULL)
			break;
		anchor = &shadow->host->node;
		top = anchor;
	}

	/* Connected when the root is the document itself. */
	*connected = top == &node->document->node;
	return anchor;
}

/* Sets bits on a node (style bits and layout bits, as given) and the matching *_BELOW bits on its ancestors. */
static void
change_mark(
	struct dom_node *node,
	unsigned bits)
{
	struct dom_node *ancestor;
	unsigned below;

	/* The node's own bits. */
	node->change |= bits;

	/* The bits of the ancestors that say something is below, up to one that has them already (all of its ancestors do). */
	below = 0U;
	if ((bits & DOM_CHANGED_STYLE_BITS) != 0U)
		below |= DOM_CHANGED_STYLE_BELOW;
	if ((bits & DOM_CHANGED_LAYOUT) != 0U)
		below |= DOM_CHANGED_LAYOUT_BELOW;
	for (ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if ((ancestor->change & below) == below)
			break;
		ancestor->change |= below;
	}
}

/* Tells whether a node is an element that brings a style sheet: an HTML style or link, or an SVG style. */
static int
change_is_sheet(
	const struct dom_node *node)
{
	const struct dom_element *element;

	if (node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)node;
	if (element->ns == DOM_NS_HTML)
		return element->tag == DOM_TAG_STYLE || element->tag == DOM_TAG_LINK;
	if (element->ns == DOM_NS_SVG)
		return element->local_name != NULL && vm_string_equal_ascii(element->local_name, "style");
	return 0;
}

/* Tells whether a subtree (and the shadow trees in it) has an element that brings a style sheet; too deep a tree says yes. */
static int
change_has_sheet(
	const struct dom_node *node,
	unsigned depth)
{
	const struct dom_node *child;
	const struct dom_element *element;

	/* The node itself. */
	if (change_is_sheet(node))
		return 1;
	if (depth >= CHANGE_SCAN_DEPTH)
		return 1;

	/* Its shadow tree, whose style elements are its own sheets. */
	if (node->type == DOM_ELEMENT) {
		element = (const struct dom_element *)node;
		if (element->shadow_root != NULL && change_has_sheet(element->shadow_root, depth + 1U))
			return 1;
	}

	/* Its children. */
	for (child = node->first_child; child != NULL; child = child->next) {
		if (change_has_sheet(child, depth + 1U))
			return 1;
	}
	return 0;
}

/* Clears the bits of a node that is out of the tree (and of what has bits below it). */
static void
change_clear_subtree(
	struct dom_node *root)
{
	struct dom_node *node;
	unsigned bits;

	node = root;
	for (;;) {
		bits = node->change;
		node->change = 0U;

		/* Below it, when something there has bits. */
		if ((bits & (DOM_CHANGED_STYLE_BELOW | DOM_CHANGED_LAYOUT_BELOW)) != 0U && node->first_child != NULL) {
			node = node->first_child;
			continue;
		}

		/* The next node of the subtree, or the end. */
		for (;;) {
			if (node == root)
				return;
			if (node->next != NULL) {
				node = node->next;
				break;
			}
			node = node->parent;
		}
	}
}
