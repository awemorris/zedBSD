/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Ordinary-tree form ownership follows current ancestry, IDs and explicit form attributes. */

#include "dom/dom.h"

static int form_html(const struct dom_element *element, int tag);
static struct dom_node *form_root(struct dom_node *node);
static struct dom_node *form_next(struct dom_node *node, struct dom_node *root);
static int form_image(const struct dom_element *element);

/*
 * Classifies built-in listed controls by actual namespace and exact local name.
 */
int
dom_form_listed(
	const struct dom_element *element)
{
	int listed;

	/* A NULL optional control never supplies a form-associated category. */
	if (element == NULL)
		return 0;

	/* Each built-in listed tag must also carry its exact HTML namespace local name. */
	switch (element->tag) {
	case DOM_TAG_BUTTON:
	case DOM_TAG_FIELDSET:
	case DOM_TAG_INPUT:
	case DOM_TAG_OBJECT:
	case DOM_TAG_OUTPUT:
	case DOM_TAG_SELECT:
	case DOM_TAG_TEXTAREA:
		listed = form_html(element, element->tag);
		if (!listed)
			return 0;

		/* Succeeded: this built-in is listed independently of disabled/name/type state. */
		return 1;
	default:
		break;
	}

	/* Refuses ordinary elements, images, legacy tags and unimplemented custom elements. */
	return 0;
}

/*
 * Classifies form.elements members, retaining listed controls except image inputs.
 */
int
dom_form_control_member(
	const struct dom_element *element)
{
	int listed;
	int image;

	/* Hidden, disabled, unnamed and fieldset controls remain listed members. */
	listed = dom_form_listed(element);
	if (!listed)
		return 0;

	/* Image inputs are listed but excluded from the historical form.elements collection. */
	image = form_image(element);
	if (image)
		return 0;

	/* Succeeded: this listed built-in can occur in its owning form's controls collection. */
	return 1;
}

/*
 * Resolves ordinary-tree ownership without parser-special persisted associations.
 * Connected listed controls honor the first matching ID for their form attribute;
 * otherwise the nearest actual HTML form ancestor owns them. No state is cached.
 */
struct dom_element *
dom_form_owner(
	const struct dom_element *element)
{
	struct dom_node *node;
	struct dom_node *root;
	struct dom_element *candidate;
	struct vm_string *reference;
	struct vm_string *id;
	int listed;
	int associated;
	int same;
	int actual;

	/* Only listed built-ins and historical images participate in this scoped model. */
	if (element == NULL)
		return NULL;

	/* Determine form association from native categories rather than script properties. */
	listed = dom_form_listed(element);
	associated = listed;
	if (!associated)
		associated = form_html(element, DOM_TAG_IMG);

	/* Ordinary elements have no form owner even when nested inside a form. */
	if (!associated)
		return NULL;

	/* Connected means the actual tree root is a Document, including XML Documents. */
	root = form_root((struct dom_node *)&element->node);
	reference = NULL;

	/* Only listed controls honor an explicit form attribute. */
	if (listed)
		reference = dom_attribute_ascii(element, "form");

	/* Resolve a connected explicit reference without falling back to ancestry. */
	if (reference != NULL && root->type == DOM_DOCUMENT) {
		/* An empty explicit attribute has no matching nonempty ID and never falls back. */
		if (reference->length == 0)
			return NULL;

		/* The first matching ID wins even when it belongs to a non-form or foreign node. */
		node = root;
		while (node != NULL) {
			/* Only elements can supply the first matching ID in tree order. */
			if (node->type == DOM_ELEMENT) {
				candidate = (struct dom_element *)node;
				id = dom_attribute_ascii(candidate, "id");

				/* Empty or absent IDs cannot match the nonempty explicit reference. */
				if (id != NULL && id->length != 0) {
					same = vm_string_equal(id, reference);
					if (same) {
						actual = form_html(candidate, DOM_TAG_FORM);
						if (!actual)
							return NULL;

						/* Succeeded: this first matching ID denotes an actual HTML form. */
						return candidate;
					}
				}
			}

			/* Tree order ignores template content because it is not a descendant subtree. */
			node = form_next(node, root);
		}

		/* A connected explicit attribute that misses never delegates to ancestry. */
		return NULL;
	}

	/* Detached explicit references and nonlisted images use ordinary live ancestry. */
	for (node = element->node.parent;
	     node != NULL;
	     node = node->parent) {
		/* Non-element ancestors cannot supply an HTML form identity. */
		if (node->type != DOM_ELEMENT)
			continue;

		/* Return the nearest actual HTML form rather than a folded-tag lookalike. */
		candidate = (struct dom_element *)node;
		actual = form_html(candidate, DOM_TAG_FORM);
		if (actual)
			return candidate;
	}

	/* Exhausted: no actual form ancestor currently owns this associated built-in. */
	return NULL;
}

/* The internal tag enum is folded, so an XML uppercase local name needs an extra check. */
static int
form_html(
	const struct dom_element *element,
	int tag)
{
	const char *name;
	int same;

	/* Neither a foreign namespace nor another built-in tag can implement this HTML role. */
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;

	/* Require the exact local name because XML elements preserve letter case. */
	name = dom_tag_name(tag);
	same = vm_string_equal_ascii(element->local_name, name);
	if (!same)
		return 0;

	/* Succeeded: both actual namespace and case-sensitive local name identify this role. */
	return 1;
}

/* Finds the current tree root instead of assuming ownerDocument means connected. */
static struct dom_node *
form_root(
	struct dom_node *node)
{
	/* Detached elements and fragments can retain ownerDocument without being connected. */
	while (node->parent != NULL)
		node = node->parent;

	/* Succeeded: the actual root supplies the connected-versus-detached decision. */
	return node;
}

/* Walks current preorder links without including an independent template-content tree. */
static struct dom_node *
form_next(
	struct dom_node *node,
	struct dom_node *root)
{
	/* A real child begins the next subtree. */
	if (node->first_child != NULL)
		return node->first_child;

	/* Climb until a sibling appears, stopping before leaving the original tree. */
	while (node != root) {
		/* A sibling starts the next subtree before another ancestor is visited. */
		if (node->next != NULL)
			return node->next;

		/* Search the current parent without crossing the original traversal root. */
		node = node->parent;
	}

	/* Exhausted: no further element can supply the explicit form reference. */
	return NULL;
}

/* Recognizes the enumerated image state without allocating or accepting whitespace keywords. */
static int
form_image(
	const struct dom_element *element)
{
	struct vm_string *type;
	const char *keyword;
	size_t index;
	uint16_t unit;

	/* Only actual listed HTML input elements can have the historical image exclusion. */
	if (element->tag != DOM_TAG_INPUT)
		return 0;

	/* The image keyword must occupy the complete actual type attribute. */
	type = dom_attribute_ascii(element, "type");
	if (type == NULL || type->length != 5U)
		return 0;

	/* ASCII case-insensitive enumerated matching does not trim invalid content values. */
	keyword = "image";
	for (index = 0; index < 5U; index++) {
		/* Fold only ASCII uppercase units without accepting locale-dependent aliases. */
		unit = vm_string_at(type, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + ('a' - 'A'));

		/* Reject the first unit which differs from the canonical image spelling. */
		if (unit != (unsigned char)keyword[index])
			return 0;
	}

	/* Succeeded: the input type attribute denotes the image state. */
	return 1;
}
