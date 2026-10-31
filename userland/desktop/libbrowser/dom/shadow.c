/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Slots: which element of a shadow tree a node of its host takes the
 * place of.
 * https://dom.spec.whatwg.org/#concept-slotable
 */

#include "dom/dom.h"

/* Tells whether a string is the empty one (an absent slot name is the same). */
static int
shadow_name_empty(
	const struct vm_string *name)
{
	return name == NULL || name->length == 0;
}

/* Tells whether two slot names are the same. */
static int
shadow_name_equal(
	const struct vm_string *a,
	const struct vm_string *b)
{
	if (shadow_name_empty(a) || shadow_name_empty(b))
		return shadow_name_empty(a) && shadow_name_empty(b);
	return vm_string_equal(a, b);
}

/* Reports the slot name a slot element of a shadow tree has (its name attribute). */
static const struct vm_string *
shadow_slot_element_name(
	const struct dom_element *slot)
{
	const struct dom_attribute *attribute;
	struct vm_string *name;

	/* The name attribute, as an atom. */
	name = vm_atom_from_ascii(slot->node.document->heap, "name");
	if (name == NULL)
		return NULL;
	attribute = dom_element_find_attribute(slot, DOM_NS_NONE, name);
	if (attribute == NULL)
		return NULL;
	return attribute->value;
}

/*
 * Reports the name of the slot a node of a host asks for: the slot
 * attribute of an element, and the default (NULL) for text.
 */
const struct vm_string *
dom_slot_name(
	const struct dom_node *node)
{
	const struct dom_element *element;
	const struct dom_attribute *attribute;
	struct vm_string *name;

	if (node->type != DOM_ELEMENT)
		return NULL;
	element = (const struct dom_element *)node;
	name = vm_atom_from_ascii(node->document->heap, "slot");
	if (name == NULL)
		return NULL;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL)
		return NULL;
	return attribute->value;
}

/*
 * Finds the first slot of a shadow tree (in tree order) with a name,
 * stopping at nothing; skip is a node whose subtree is not searched.
 */
struct dom_element *
dom_slot_first(
	const struct dom_node *shadow,
	const struct vm_string *name,
	const struct dom_node *skip)
{
	const struct dom_node *node;
	struct dom_element *found;

	for (node = shadow->first_child; node != NULL; node = node->next) {
		if (node == skip || node->type != DOM_ELEMENT)
			continue;
		if (dom_element_is(node, DOM_NS_HTML, DOM_TAG_SLOT) &&
		    shadow_name_equal(shadow_slot_element_name((const struct dom_element *)node), name))
			return (struct dom_element *)node;
		found = dom_slot_first(node, name, skip);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/*
 * Reports the slot a node of a host is assigned to, or NULL: the first
 * slot of the host's shadow tree with the node's slot name.
 */
struct dom_element *
dom_slot_of(
	const struct dom_node *node)
{
	const struct dom_element *host;

	if (node->parent == NULL || node->parent->type != DOM_ELEMENT)
		return NULL;
	if (node->type != DOM_ELEMENT && node->type != DOM_TEXT)
		return NULL;
	host = (const struct dom_element *)node->parent;
	if (host->shadow_root == NULL)
		return NULL;
	return dom_slot_first(host->shadow_root, dom_slot_name(node), NULL);
}

/* Tells whether a node of a host is assigned to a slot of its shadow tree. */
int
dom_assigned_to(
	const struct dom_element *slot,
	const struct dom_node *node)
{
	return dom_slot_of(node) == slot;
}

/*
 * Reports the parent of a node in the flat tree: the host for the child of a shadow root, the slot
 * for a node assigned to one, and the ordinary parent otherwise.
 */
struct dom_node *
dom_flat_parent(
	const struct dom_node *node)
{
	struct dom_element *slot;
	struct dom_node *parent;

	parent = node->parent;
	if (parent == NULL)
		return NULL;
	if (dom_shadow_of(parent) != NULL)
		return &dom_shadow_of(parent)->host->node;
	if (parent->type == DOM_ELEMENT && ((struct dom_element *)parent)->shadow_root != NULL) {
		slot = dom_slot_of(node);
		if (slot != NULL)
			return &slot->node;
	}
	return parent;
}

/* Tells whether a node is in its document's shadow-including tree. */
int
dom_is_connected(
	const struct dom_node *node)
{
	while (node != NULL) {
		if (node == &node->document->node)
			return 1;
		if (node->parent != NULL)
			node = node->parent;
		else
			node = dom_shadow_of(node) != NULL ? &dom_shadow_of(node)->host->node : NULL;
	}
	return 0;
}
