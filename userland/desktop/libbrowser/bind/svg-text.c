/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Canonical SVG text interfaces count live native addressable UTF16 data in its actual CSS context. */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <stdlib.h>

/* One traversal retains only the flow's normalized count and deferred collapsible whitespace. */
struct svg_text_flow {
	size_t count;
	int pending_space;
	int line_start;
};

static int svg_text_chars(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* The supported text method lives on its actual inherited SVGTextContentElement prototype. */
static const struct bind_operation svg_text_operations[] = {
    {"getNumberOfChars", 0, svg_text_chars},
    {NULL, 0, NULL}};

/* Length adjustment constants retain their specified native interface values without fabricating metrics APIs. */
static const struct bind_constant svg_text_constants[] = {
    {"LENGTHADJUST_UNKNOWN", 0},
    {"LENGTHADJUST_SPACING", 1},
    {"LENGTHADJUST_SPACINGANDGLYPHS", 2},
    {NULL, 0}};

/* Canonical SVG nodes retain the ordinary native Element and EventTarget graph. */
const struct bind_interface bind_svg_element_interface = {
    "SVGElement", BIND_ELEMENT, 0, NULL, NULL, NULL, NULL};

/* Supported graphical SVG text nodes have their actual inherited interface identity. */
const struct bind_interface bind_svg_graphics_element_interface = {
    "SVGGraphicsElement", BIND_SVG_ELEMENT, 0, NULL, NULL, NULL, NULL};

/* Only real text-content nodes expose the supported addressable-character operation. */
const struct bind_interface bind_svg_text_content_element_interface = {
    "SVGTextContentElement", BIND_SVG_GRAPHICS_ELEMENT, 0, NULL, NULL, svg_text_operations, svg_text_constants};

/* Text and tspan share their native text-positioning prototype without new placeholder attributes. */
const struct bind_interface bind_svg_text_positioning_element_interface = {
    "SVGTextPositioningElement", BIND_SVG_TEXT_CONTENT_ELEMENT, 0, NULL, NULL, NULL, NULL};

/* Exact SVG text nodes use their own actual native subtype. */
const struct bind_interface bind_svg_text_element_interface = {
    "SVGTextElement", BIND_SVG_TEXT_POSITIONING_ELEMENT, 0, NULL, NULL, NULL, NULL};

/* Exact SVG tspan nodes retain the supported positioning/text-content inheritance. */
const struct bind_interface bind_svg_tspan_element_interface = {
    "SVGTSpanElement", BIND_SVG_TEXT_POSITIONING_ELEMENT, 0, NULL, NULL, NULL, NULL};

/* Exact textPath nodes inherit text-content operations independently of positioning elements. */
const struct bind_interface bind_svg_text_path_element_interface = {
    "SVGTextPathElement", BIND_SVG_TEXT_CONTENT_ELEMENT, 0, NULL, NULL, NULL, NULL};

static int svg_text_this(struct vm_realm *realm, vm_value receiver, struct dom_element **element);
static int svg_text_count(struct bind_window *window, struct dom_element *element, struct vm_cell **cursor, size_t *count);
static int svg_text_style(struct bind_window *window, struct dom_element *element, struct css_style *style);
static int svg_text_units(struct svg_text_flow *flow, const struct dom_character_data *text, int white_space);
static int svg_text_add(struct svg_text_flow *flow, size_t count);
static struct dom_node *svg_text_following(struct dom_node *node, struct dom_node *root, int skip);

/*
 * Classifies canonical SVG nodes by exact native local name without allocation.
 */
int
bind_svg_node_interface(
	const struct dom_element *element)
{
	int same;

	/* The caller establishes the real canonical SVG namespace before classification. */
	same = vm_string_equal_ascii(element->local_name, "text");
	if (same)
		return BIND_SVG_TEXT_ELEMENT;

	/* A tspan inherits text positioning on its exact canonical local identity. */
	same = vm_string_equal_ascii(element->local_name, "tspan");
	if (same)
		return BIND_SVG_TSPAN_ELEMENT;

	/* A textPath inherits text content independently of positioning. */
	same = vm_string_equal_ascii(element->local_name, "textPath");
	if (same)
		return BIND_SVG_TEXT_PATH_ELEMENT;

	/* The exact supported geometry subtype retains its own native width interface. */
	same = vm_string_equal_ascii(element->local_name, "rect");
	if (same)
		return BIND_SVG_RECT_ELEMENT;

	/* Succeeded: other SVG nodes retain their genuine generic SVG identity. */
	return BIND_SVG_ELEMENT;
}

/* Counts actual supported text through the receiver's owning native Document and CSS context. */
static int
svg_text_chars(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct bind_window *window;
	struct vm_cell *roots[2];
	size_t characters;
	unsigned index;
	unsigned registered;
	int connected;
	int error;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The actual receiver brand is independent of prototype changes and public text properties. */
	element = NULL;
	error = svg_text_this(realm, receiver, &element);
	if (error != 0)
		return error;

	/* Inactive native owners contribute the existing zero addressable count. */
	*result = vm_value_number(0);
	window = element->node.document->view;
	if (window == NULL || window->detached)
		return 0;

	/* A disconnected receiver has no rendered native text flow. */
	connected = dom_is_inclusive_ancestor(&window->document->node, &element->node);
	if (!connected)
		return 0;

	/* Precise native receiver and traversal slots remain valid across CSS allocation and host callbacks. */
	roots[0] = &element->node.cell;
	roots[1] = NULL;
	registered = 0;
	error = 0;
	characters = 0;
	for (index = 0; index < 2U; index++) {
		/* Each registered stack slot must remain stable until normal cleanup. */
		error = vm_heap_add_root(realm->heap, &roots[index]);
		if (error != 0)
			goto cleanup;

		/* Only successful registration contributes a later removal obligation. */
		registered++;
	}

	/* Only the actual owning view supplies inherited styles for the live native subtree. */
	error = svg_text_count(window, element, &roots[1], &characters);
	if (error != 0)
		goto cleanup;

cleanup:
	/* Releases exactly the stable root slots registered by this attempt. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve native failures without inventing a character result. */
	if (error != 0)
		return error;

	/* Publishes the completed native traversal's actual normalized count. */
	*result = vm_value_number((double)characters);

	/* Succeeded: the result reflects actual normalized UTF16 code units, not glyph count. */
	return 0;
}

/* Rejects wrong namespace, local case, unrelated interfaces and forged platform receivers. */
static int
svg_text_this(
	struct vm_realm *realm,
	vm_value receiver,
	struct dom_element **element)
{
	struct dom_node *node;
	int interface;
	int error;

	/* Existing native node branding rejects public prototype and expando imitations. */
	error = bind_this_node(realm, receiver, &node);
	if (error != 0)
		return error;

	/* Non-elements cannot carry the supported native SVG text-content brand. */
	if (node->type != DOM_ELEMENT) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* Succeeded: the receiver refusal follows the existing native throw contract. */
		return 0;
	}

	/* Only canonical SVG text-content subtypes share this actual operation. */
	*element = (struct dom_element *)node;
	if ((*element)->ns != DOM_NS_SVG) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* Succeeded: the namespace refusal follows the existing native throw contract. */
		return 0;
	}

	/* Exact local spelling determines the native inherited interface, independent of folded tag IDs. */
	interface = bind_svg_node_interface(*element);
	if (interface != BIND_SVG_TEXT_ELEMENT &&
	    interface != BIND_SVG_TSPAN_ELEMENT &&
	    interface != BIND_SVG_TEXT_PATH_ELEMENT) {
		error = bind_throw_illegal(realm);
		if (error != 0)
			return error;

		/* Succeeded: the subtype refusal follows the existing native throw contract. */
		return 0;
	}

	/* Succeeded: this is a genuine supported native text-content element. */
	return 0;
}

/* Traverses actual DOM iteratively and skips native subtrees excluded by their current display state. */
static int
svg_text_count(
	struct bind_window *window,
	struct dom_element *element,
	struct vm_cell **cursor,
	size_t *count)
{
	struct dom_node *walk;
	struct dom_element *current;
	struct css_style *style;
	struct svg_text_flow flow;
	int connected;
	int skip;
	int white_space;
	int error;

	/* The large computed style record lives in checked C storage, not a recursive native stack. */
	*count = 0;

	/* Allocates one independent style record before native ancestor traversal. */
	style = malloc(sizeof(*style));
	if (style == NULL)
		return ENOMEM;

	/* A display:none ancestor excludes all addressable text even through a method called below it. */
	error = 0;
	walk = &element->node;
	while (walk != NULL && walk->type != DOM_DOCUMENT) {
		/* Retains the actual native cursor before a style callback may collect. */
		*cursor = &walk->cell;
		if (walk->type == DOM_ELEMENT) {
			current = (struct dom_element *)walk;
			error = svg_text_style(window, current, style);
			if (error != 0 || style->display == CSS_DISPLAY_NONE)
				break;
		}

		/* Continue through actual native ancestors rather than CSS or script-visible aliases. */
		walk = walk->parent;
	}

	/* Retired, disconnected or suppressed native owners have no rendered addressable characters. */
	connected = dom_is_inclusive_ancestor(&window->document->node, &element->node);
	if (error == ENOENT ||
	    !connected ||
	    window->detached ||
	    walk == NULL ||
	    walk->type != DOM_DOCUMENT) {
		free(style);
		if (error != 0 && error != ENOENT)
			return error;
		return 0;
	}

	/* Native preorder preserves whitespace continuity across adjacent Text, CDATA and nested spans. */
	flow.count = 0;
	flow.pending_space = 0;
	flow.line_start = 1;
	walk = &element->node;

	/* Visits the native flow in preorder until its actual receiver is exhausted. */
	while (walk != NULL && error == 0) {
		/* Retains each current native node before resolving its computed style. */
		*cursor = &walk->cell;
		skip = 0;
		if (walk->type == DOM_ELEMENT) {
			current = (struct dom_element *)walk;
			error = svg_text_style(window, current, style);
			if (error != 0)
				break;

			/* A suppressed native subtree contributes neither text nor whitespace. */
			if (style->display == CSS_DISPLAY_NONE)
				skip = 1;
		} else if (walk->type == DOM_TEXT || walk->type == DOM_CDATA_SECTION) {
			/* Each actual text node inherits its current native parent's whitespace behavior. */
			current = (struct dom_element *)walk->parent;
			error = svg_text_style(window, current, style);
			if (error != 0)
				break;

			/* Counts this node's actual storage under its completed inherited whitespace mode. */
			white_space = style->white_space;
			error = svg_text_units(&flow, (struct dom_character_data *)walk, white_space);
			if (error != 0)
				break;
		}

		/* A host callback may retire or remove the receiver before the next native traversal. */
		connected = dom_is_inclusive_ancestor(&window->document->node, &element->node);
		if (!connected || window->detached) {
			flow.count = 0;
			break;
		}

		/* Skipped subtrees do not consume whitespace; comments and PI are never character sources. */
		walk = svg_text_following(walk, &element->node, skip);
	}

	/* Deferred collapsible trailing whitespace never becomes an addressable final flow character. */
	free(style);

	/* An inactive native style owner contributes the existing zero result. */
	if (error == ENOENT)
		return 0;

	/* Propagates a failed native traversal after releasing its scratch storage. */
	if (error != 0)
		return error;

	/* Publishes the completed normalized native flow count. */
	*count = flow.count;

	/* Succeeded: the native flow supplies its exact current normalized count. */
	return 0;
}

/* Resolves the actual owner's inherited CSS through the existing primary or child path. */
static int
svg_text_style(
	struct bind_window *window,
	struct dom_element *element,
	struct css_style *style)
{
	int error;

	/* An inactive owner cannot dereference an old Page host context. */
	if (window->detached || element->node.document != window->document)
		return ENOENT;

	/* Uses the actual owner's host style callback or its existing child fallback. */
	if (window->host.computed_style != NULL) {
		error = window->host.computed_style(window->host.context, element, style);
		if (error != 0)
			return error;
	} else {
		error = bind_style_context_compute(window, element, style);
		if (error != 0)
			return error;
	}

	/* Succeeded: the actual computed display and whitespace fields are available. */
	return 0;
}

/* Normalizes native UTF16 whitespace while counting code units independently of glyphs or surrogate pairing. */
static int
svg_text_units(
	struct svg_text_flow *flow,
	const struct dom_character_data *text,
	int white_space)
{
	size_t index;
	uint16_t unit;
	int preserve;
	int breaks;
	int space;
	int error;

	/* Preserved spacing and preserved line breaks are separate native CSS behaviors. */
	preserve = 0;
	breaks = 0;

	/* Preformatted modes preserve spacing; pre-line preserves only line breaks. */
	if (white_space == CSS_WHITE_SPACE_PRE || white_space == CSS_WHITE_SPACE_PRE_WRAP) {
		preserve = 1;
		breaks = 1;
	} else if (white_space == CSS_WHITE_SPACE_PRE_LINE) {
		breaks = 1;
	}

	/* Count actual UTF16 storage, leaving combining characters and surrogate code units independent. */
	for (index = 0; index < text->data.length; index++) {
		/* Reads one actual UTF16 code unit before segment-break normalization. */
		unit = text->data.data[index];
		if (unit == '\r') {
			/* CSS segment breaks normalize a native CRLF pair to one line feed. */
			unit = '\n';
			if (index + 1U < text->data.length && text->data.data[index + 1U] == '\n')
				index++;
		}

		/* Preserved line breaks discard preceding collapsible whitespace and start a new flow line. */
		if (unit == '\n' && breaks) {
			flow->pending_space = 0;
			error = svg_text_add(flow, 1U);
			if (error != 0)
				return error;

			/* A preserved break ends the old line without deferred collapsible space. */
			flow->line_start = 1;
			continue;
		}

		/* Collapse only CSS document whitespace; NBSP and ordinary Unicode units remain addressable. */
		space = 0;
		if (unit == ' ' ||
		    unit == '\t' ||
		    unit == '\n' ||
		    unit == '\f')
			space = 1;

		/* Collapsible whitespace is deferred until another actual character follows. */
		if (space && !preserve) {
			flow->pending_space = 1;
			continue;
		}

		/* A deferred internal collapsed run contributes one unit, with no invented leading space. */
		if (flow->pending_space && !flow->line_start) {
			error = svg_text_add(flow, 1U);
			if (error != 0)
				return error;
		}

		/* Preserve actual ordinary or noncollapsed spacing units, including lone surrogate storage. */
		flow->pending_space = 0;
		error = svg_text_add(flow, 1U);
		if (error != 0)
			return error;

		/* The counted ordinary code unit makes this a nonempty flow line. */
		flow->line_start = 0;
	}

	/* Succeeded: any final collapsible run remains deferred for the next actual text node. */
	return 0;
}

/* Checks the signed WebIDL long bound before extending the native addressable count. */
static int
svg_text_add(
	struct svg_text_flow *flow,
	size_t count)
{
	/* An unrepresentable native count is an explicit error instead of silent wrapping. */
	if (count > (size_t)INT32_MAX - flow->count)
		return EOVERFLOW;

	/* Adds only a representable extension to the existing addressable flow. */
	flow->count += count;

	/* Succeeded: the count remains representable as the method's actual result. */
	return 0;
}

/* Advances within one native subtree without descending through a suppressed element. */
static struct dom_node *
svg_text_following(
	struct dom_node *node,
	struct dom_node *root,
	int skip)
{
	/* Ordinary preorder includes children only when their actual ancestor participates. */
	if (!skip && node->first_child != NULL)
		return node->first_child;

	/* Find the next sibling without escaping the receiver's native subtree. */
	while (node != root && node->next == NULL) {
		node = node->parent;
		if (node == NULL)
			return NULL;
	}

	/* Exhausted or removed receiver traversal has no following addressable node. */
	if (node == root)
		return NULL;

	/* Succeeded: this is the next native sibling in the same live flow. */
	return node->next;
}
