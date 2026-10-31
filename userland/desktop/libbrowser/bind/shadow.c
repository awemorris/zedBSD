/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shadow trees for scripts: Element.attachShadow() and shadowRoot, the
 * ShadowRoot interface, Node.getRootNode(), and a slot's assigned nodes.
 * https://dom.spec.whatwg.org/#shadow-trees
 */

#include "bind/internal.h"

#include <errno.h>

static int shadow_this(struct vm_realm *realm, vm_value this_value, struct dom_shadow_root **root);
static int shadow_mode_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int shadow_host_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int shadow_false_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int shadow_slot_assignment_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int shadow_get_element_by_id(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The attributes of ShadowRoot (the fragment's ParentNode ones are inherited). */
static const struct bind_attribute shadow_attributes[] = {
	{ "mode", shadow_mode_get, NULL },
	{ "host", shadow_host_get, NULL },
	{ "delegatesFocus", shadow_false_get, NULL },
	{ "clonable", shadow_false_get, NULL },
	{ "serializable", shadow_false_get, NULL },
	{ "slotAssignment", shadow_slot_assignment_get, NULL },
	{ "innerHTML", bind_inner_html_get, bind_inner_html_set },
	{ NULL, NULL, NULL }
};

/* The operations of ShadowRoot. */
static const struct bind_operation shadow_operations[] = {
	{ "getElementById", 1, shadow_get_element_by_id },
	{ NULL, 0, NULL }
};

/* The ShadowRoot interface. */
const struct bind_interface bind_shadow_root_interface = {
	"ShadowRoot", BIND_DOCUMENT_FRAGMENT, 0, NULL, shadow_attributes, shadow_operations, NULL
};

/* Finds the shadow root a method's this value stands for, throwing a TypeError otherwise. */
static int
shadow_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_shadow_root **root)
{
	struct dom_node *node;

	node = bind_node_of(this_value);
	*root = dom_shadow_of(node);
	if (*root == NULL)
		return bind_throw_illegal(realm);
	return 0;
}

/* Reports "open" or "closed" (mode). */
static int
shadow_mode_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_shadow_root *root;
	struct vm_string *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = shadow_this(realm, this_value, &root);
	if (status != 0)
		return status;
	text = vm_string_from_utf8(realm->heap, root->closed ? "closed" : "open", root->closed ? 6U : 4U);
	if (text == NULL)
		return ENOMEM;
	*result = vm_value_cell(text);
	return 0;
}

/* Reports "named" (slotAssignment). */
static int
shadow_slot_assignment_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_shadow_root *root;
	struct vm_string *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = shadow_this(realm, this_value, &root);
	if (status != 0)
		return status;
	text = vm_string_from_utf8(realm->heap, root->slot_manual ? "manual" : "named", root->slot_manual ? 6U : 5U);
	if (text == NULL)
		return ENOMEM;
	*result = vm_value_cell(text);
	return 0;
}

/* Reports false (the flags no shadow root of this engine has). */
static int
shadow_false_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_shadow_root *root;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = shadow_this(realm, this_value, &root);
	if (status != 0)
		return status;
	*result = vm_value_boolean(0);
	return 0;
}

/* Reports the host element (host). */
static int
shadow_host_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_shadow_root *root;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = shadow_this(realm, this_value, &root);
	if (status != 0)
		return status;
	return bind_wrap(bind_window_of(realm), &root->host->node, result);
}

/* Finds the first element under the root whose id is the string (getElementById). */
static int
shadow_get_element_by_id(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_shadow_root *root;
	struct dom_node *walk;
	struct dom_attribute *attribute;
	struct vm_string *id;
	struct vm_string *name;
	struct vm_cell *id_root;
	int status;

	status = shadow_this(realm, this_value, &root);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &id);
	if (status != 0)
		return status;
	id_root = &id->cell;
	status = vm_heap_add_root(realm->heap, &id_root);
	if (status != 0)
		return status;
	name = vm_atom_from_ascii(realm->heap, "id");
	vm_heap_remove_root(realm->heap, &id_root);
	if (name == NULL)
		return ENOMEM;
	for (walk = bind_following(&root->node, &root->node); walk != NULL; walk = bind_following(walk, &root->node)) {
		if (walk->type != DOM_ELEMENT)
			continue;
		attribute = dom_element_find_attribute((struct dom_element *)walk, DOM_NS_NONE, name);
		if (attribute != NULL && vm_string_equal(attribute->value, id))
			break;
	}
	return bind_wrap_or_null(bind_window_of(realm), walk, result);
}

/*
 * Gives an element a shadow root ({ mode: "open" | "closed" }) and reports it.
 * https://dom.spec.whatwg.org/#dom-element-attachshadow
 */
int
bind_attach_shadow(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_shadow_root *root;
	struct vm_string *mode;
	vm_value value;
	int present;
	int closed;
	int status;

	/* The element. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT)
		return bind_throw_illegal(realm);

	/* The mode is required, and one of two words. */
	if (!vm_value_is_object(js_argument(args, count, 0)))
		return vm_throw_type_error(realm, "Failed to execute 'attachShadow' on 'Element': The provided value is not of type 'ShadowRootInit'.");
	status = bind_get_option(realm, js_argument(args, count, 0), "mode", &present, &value);
	if (status != 0)
		return status;
	if (!present || value == VM_VALUE_UNDEFINED)
		return vm_throw_type_error(realm, "Failed to execute 'attachShadow' on 'Element': Required member is undefined.");
	status = bind_to_string(realm, value, &mode);
	if (status != 0)
		return status;
	if (vm_string_equal_ascii(mode, "open"))
		closed = 0;
	else if (vm_string_equal_ascii(mode, "closed"))
		closed = 1;
	else
		return vm_throw_type_error(realm, "Failed to execute 'attachShadow' on 'Element': The provided value is not a valid ShadowRootMode.");

	/* Not every element may host one, and one that has one cannot get another. */
	if (!dom_can_attach_shadow((struct dom_element *)node))
		return bind_throw_dom(realm, "NotSupportedError", "This element does not support attachShadow");
	root = dom_shadow_attach((struct dom_element *)node, closed);
	if (root == NULL)
		return ENOMEM;
	return bind_wrap(bind_window_of(realm), &root->node, result);
}

/* Reports the element's open shadow root, or null (shadowRoot). */
int
bind_shadow_root_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT)
		return bind_throw_illegal(realm);
	element = (struct dom_element *)node;
	*result = VM_VALUE_NULL;
	if (element->shadow_root == NULL || dom_shadow_of(element->shadow_root)->closed)
		return 0;
	return bind_wrap(bind_window_of(realm), element->shadow_root, result);
}

/*
 * Reports the root of the tree a node is in; with { composed: true } a shadow root's host's root instead.
 * https://dom.spec.whatwg.org/#dom-node-getrootnode
 */
int
bind_get_root_node(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	vm_value value;
	int present;
	int composed;
	int status;

	node = bind_node_of(this_value);
	if (node == NULL)
		return bind_throw_illegal(realm);
	composed = 0;
	if (vm_value_is_object(js_argument(args, count, 0))) {
		status = bind_get_option(realm, js_argument(args, count, 0), "composed", &present, &value);
		if (status != 0)
			return status;
		composed = vm_to_boolean(value);
	}
	node = dom_tree_root(node);
	while (composed && dom_shadow_host(node) != NULL)
		node = dom_tree_root(&dom_shadow_host(node)->node);
	return bind_wrap(bind_window_of(realm), node, result);
}
