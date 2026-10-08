/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Nodes for scripts: the one object each node gets (its wrapper), the
 * Node interface (the tree's links, textContent, and inserting, removing
 * and cloning nodes), and the helpers the other node interfaces share.
 *
 * Lists of nodes (childNodes, children, getElementsByTagName) are arrays
 * made when asked for, not the DOM's live collections.
 */

#include "bind/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int node_type_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_name_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_value_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_value_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_text_content_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_text_content_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_parent_node(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_parent_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_first_child(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_last_child(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_previous_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_next_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_owner_document(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_is_connected(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_child_nodes(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_append_child(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_insert_before(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_remove_child(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_replace_child(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_has_child_nodes(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_contains(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_clone(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int node_link(struct vm_realm *realm, vm_value this_value, struct dom_node *linked, vm_value *result);
static int node_prototype_index(const struct dom_node *node);

/*
 * The attributes of Node.  The table is constant for the life of the
 * program.
 */
static const struct bind_attribute node_attributes[] = {
	{ "nodeType", node_type_get, NULL },
	{ "nodeName", node_name_get, NULL },
	{ "nodeValue", node_value_get, node_value_set },
	{ "textContent", node_text_content_get, node_text_content_set },
	{ "parentNode", node_parent_node, NULL },
	{ "parentElement", node_parent_element, NULL },
	{ "firstChild", node_first_child, NULL },
	{ "lastChild", node_last_child, NULL },
	{ "previousSibling", node_previous_sibling, NULL },
	{ "nextSibling", node_next_sibling, NULL },
	{ "ownerDocument", node_owner_document, NULL },
	{ "isConnected", node_is_connected, NULL },
	{ "childNodes", node_child_nodes, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Node.  The table is constant for the life of the
 * program.
 */
static const struct bind_operation node_operations[] = {
	{ "appendChild", 1, node_append_child },
	{ "insertBefore", 2, node_insert_before },
	{ "removeChild", 1, node_remove_child },
	{ "replaceChild", 2, node_replace_child },
	{ "hasChildNodes", 0, node_has_child_nodes },
	{ "contains", 1, node_contains },
	{ "cloneNode", 0, node_clone },
	{ NULL, 0, NULL }
};

/*
 * The constants of Node: the node types.  The table is constant for the
 * life of the program.
 */
static const struct bind_constant node_constants[] = {
	{ "ELEMENT_NODE", 1 },
	{ "ATTRIBUTE_NODE", 2 },
	{ "TEXT_NODE", 3 },
	{ "CDATA_SECTION_NODE", 4 },
	{ "PROCESSING_INSTRUCTION_NODE", 7 },
	{ "COMMENT_NODE", 8 },
	{ "DOCUMENT_NODE", 9 },
	{ "DOCUMENT_TYPE_NODE", 10 },
	{ "DOCUMENT_FRAGMENT_NODE", 11 },
	{ NULL, 0 }
};

/*
 * The Node interface.
 */
const struct bind_interface bind_node_interface = {
	"Node", BIND_EVENT_TARGET, 0, NULL, node_attributes, node_operations, node_constants
};

/*
 * Reports the script's object for a node, making it the first time.
 */
int
bind_wrap(
	struct bind_window *window,
	struct dom_node *node,
	vm_value *value)
{
	struct vm_object *wrapper;
	struct vm_object *prototype;
	struct vm_heap *heap;
	struct vm_cell *node_root;
	vm_value snapshot;
	int status;
	int index;

	/* A node keeps its object once it has one. */
	if (node->wrapper != NULL) {
		*value = vm_value_cell(node->wrapper);
		return 0;
	}

	/* Callee ownership retains the actual node, target and Document through wrapper allocation. */
	heap = node->document->heap;
	node_root = &node->cell;
	status = vm_heap_add_root(heap, &node_root);
	if (status != 0)
		return status;

	/* A borrowed method still creates wrappers in the Document's owning realm. */
	if (node->document->view != NULL)
		window = node->document->view;

	/* The object, with the prototype of the node's most specific interface. */
	index = node_prototype_index(node);

	/* XML factory nodes use a traced snapshot even through another realm's method. */
	if (node->document->binding_prototypes != NULL) {
		status = vm_object_get(node->document->binding_prototypes, vm_value_int32(index), &snapshot);
		if (status != 0) {
			vm_heap_remove_root(heap, &node_root);
			return status;
		}

		/* Only the private snapshot's object entries can supply a native prototype. */
		if (snapshot == VM_VALUE_UNDEFINED) {
			vm_heap_remove_root(heap, &node_root);
			return EINVAL;
		}

		/* The retained owning snapshot supplies this exact native interface prototype. */
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Ordinary parser nodes still require their existing live binding owner. */
		if (window == NULL) {
			vm_heap_remove_root(heap, &node_root);
			return EINVAL;
		}

		/* Ordinary owning Windows expose the same interface table identities. */
		prototype = window->prototypes[index];
	}

	/* Allocates in the shared owning heap without a raw Window lifetime dependency. */
	wrapper = vm_object_create(heap, prototype);
	if (wrapper == NULL) {
		vm_heap_remove_root(heap, &node_root);
		return ENOMEM;
	}

	/* It stands for the node, and the node keeps it. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(node);
	node->wrapper = wrapper;

	/* Succeeded: the value is the node's object. */
	*value = vm_value_cell(wrapper);
	vm_heap_remove_root(heap, &node_root);
	return 0;
}

/*
 * Reports the script's object for a node, or null for no node.
 */
int
bind_wrap_or_null(
	struct bind_window *window,
	struct dom_node *node,
	vm_value *value)
{
	int error;

	/* No node is null. */
	if (node == NULL) {
		*value = VM_VALUE_NULL;
		return 0;
	}

	/* A node is its object. */
	error = bind_wrap(window, node, value);
	if (error != 0)
		return error;

	/* Succeeded: the value is the node's object. */
	return 0;
}

/*
 * Reports the node a value stands for, or NULL when it is not a node's
 * object.
 */
struct dom_node *
bind_node_of(
	vm_value value)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int is_node;

	/* Only an object can stand for a node. */
	is_object = vm_value_is_object(value);
	if (!is_object)
		return NULL;

	/* A platform object whose cell is a node. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM)
		return NULL;
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell)
		return NULL;
	cell = vm_value_as_cell(object->internal);
	is_node = dom_is_node(cell);
	if (!is_node)
		return NULL;

	/* The node. */
	return (struct dom_node *)cell;
}

/*
 * Finds the node a method's this value stands for, throwing a TypeError
 * when it stands for none.
 */
int
bind_this_node(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_node **node)
{
	int status;

	/* The node, or the error of a method on the wrong object. */
	*node = bind_node_of(this_value);
	if (*node == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the node is found. */
	return 0;
}

/*
 * Finds the node an argument stands for, throwing a TypeError when it
 * stands for none.
 */
int
bind_argument_node(
	struct vm_realm *realm,
	vm_value value,
	struct dom_node **node)
{
	int status;

	/* The node, or the error of an argument of the wrong type. */
	*node = bind_node_of(value);
	if (*node == NULL) {
		status = vm_throw_type_error(realm, "The argument is not of type 'Node'.");
		return status;
	}

	/* Succeeded: the node is found. */
	return 0;
}

/*
 * Appends the text of a node's descendant text nodes, in tree order (an
 * element's or a fragment's textContent).
 */
int
bind_text_content(
	const struct dom_node *node,
	struct wb_units *units)
{
	const struct dom_node *walk;
	const struct dom_character_data *text;
	int error;

	/* Walks the descendants in tree order. */
	for (walk = bind_following(node, node); walk != NULL; walk = bind_following(walk, node)) {
		if (walk->type != DOM_TEXT && walk->type != DOM_CDATA_SECTION)
			continue;
		text = (const struct dom_character_data *)walk;
		error = wb_units_append(units, text->data.data, text->data.length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}

/*
 * Inserts a node into parent before reference (at the end when reference
 * is NULL), after the checks of the DOM's pre-insertion; a fragment
 * inserts its children.  Throws a DOM error when the insertion is not
 * allowed.
 */
int
bind_insert(
	struct vm_realm *realm,
	struct dom_node *parent,
	struct dom_node *node,
	struct dom_node *reference)
{
	struct bind_window *window;
	struct dom_node *child;
	int status;

	/* Validate the whole operation before adoption, host resolution or fragment splicing. */
	status = bind_validate_insert(realm, parent, node, reference);
	if (status != 0)
		return status;

	/* The actual parent Document's host observes even borrowed-method insertions. */
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;

	/* Inserting a node before itself inserts it before its next sibling. */
	if (reference == node)
		reference = node->next;

	/* Checked adoption preserves original removal notifications and updates all current owners first. */
	status = dom_adopt(parent->document, node);
	if (status != 0)
		return status;

	/* A fragment's children move, in order; the fragment is left empty. */
	if (node->type == DOM_DOCUMENT_FRAGMENT) {
		for (child = node->first_child; child != NULL; child = node->first_child) {
			dom_insert_before(parent, child, reference);
			status = bind_environment_child_mutation(window, parent, child, NULL);
			if (status != 0)
				return status;
			if (window->host.node_inserted != NULL) {
				status = window->host.node_inserted(window->host.context, child);
				if (status != 0)
					return status;
			}
		}

		/* Succeeded: every child was moved. */
		return 0;
	}

	/* Any other node moves from where it was. */
	dom_insert_before(parent, node, reference);
	status = bind_environment_child_mutation(window, parent, node, NULL);
	if (status != 0)
		return status;
	if (window->host.node_inserted != NULL) {
		status = window->host.node_inserted(window->host.context, node);
		if (status != 0)
			return status;
	}

	/* Succeeded: the node is in the parent. */
	return 0;
}

/*
 * Makes an empty array for a list of nodes.
 */
int
bind_array_create(
	struct vm_realm *realm,
	struct vm_object **array)
{
	/* An array of the realm. */
	*array = vm_array_create(realm->heap, realm->array_prototype);
	if (*array == NULL)
		return ENOMEM;

	/* Succeeded: the array is empty. */
	return 0;
}

/*
 * Appends a node's object to an array.
 */
int
bind_array_push_node(
	struct bind_window *window,
	struct vm_object *array,
	struct dom_node *node)
{
	struct vm_cell *array_root;
	struct vm_cell *wrapper_root;
	struct vm_heap *node_heap;
	vm_value wrapper;
	int error;

	/* The result array survives wrapper construction in its caller's heap. */
	array_root = &array->cell;
	error = vm_heap_add_root(window->realm->heap, &array_root);
	if (error != 0)
		return error;

	/* The node's object. */
	error = bind_wrap(window, node, &wrapper);
	if (error != 0) {
		vm_heap_remove_root(window->realm->heap, &array_root);
		return error;
	}

	/* The wrapper's owning heap may differ from the array's heap. */
	node_heap = node->document->heap;
	wrapper_root = vm_value_as_cell(wrapper);
	error = vm_heap_add_root(node_heap, &wrapper_root);
	if (error != 0) {
		vm_heap_remove_root(window->realm->heap, &array_root);
		return error;
	}

	/* At the array's end. */
	error = vm_object_define(window->realm->heap, array, vm_value_int32((int32_t)array->length), wrapper, VM_PROPERTY_DEFAULT);

	/* Each temporary root leaves the heap that owns its cell. */
	vm_heap_remove_root(node_heap, &wrapper_root);
	vm_heap_remove_root(window->realm->heap, &array_root);

	/* Reports the append or allocation outcome. */
	return error;
}

/*
 * Reports the node after a node in tree order within root's subtree, or
 * NULL at its end.
 */
struct dom_node *
bind_following(
	const struct dom_node *node,
	const struct dom_node *root)
{
	const struct dom_node *walk;

	/* A node's first child comes next. */
	if (node->first_child != NULL)
		return node->first_child;

	/* Otherwise the next sibling of the node or of its nearest ancestor below root that has one. */
	for (walk = node; walk != NULL && walk != root; walk = walk->parent) {
		if (walk->next != NULL)
			return walk->next;
	}

	/* The subtree is done. */
	return NULL;
}

/*
 * Reports a node's first element child, or NULL.
 */
struct dom_element *
bind_first_element_child(
	const struct dom_node *node)
{
	struct dom_node *child;

	/* The first child that is an element. */
	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type == DOM_ELEMENT)
			return (struct dom_element *)child;
	}

	/* No element child. */
	return NULL;
}

/* Reports the node's type number (nodeType). */
static int
node_type_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Succeeded: the DOM numbers the types as the node records them. */
	*result = vm_value_int32(node->type);
	return 0;
}

/* Reports the node's name (nodeName): an element's tag name, or the name of the node's kind. */
static int
node_name_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	const char *name;
	vm_value key;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* An element's name is its tagName. */
	if (node->type == DOM_ELEMENT) {
		key = vm_key_from_ascii(realm->heap, "tagName");
		if (key == VM_VALUE_EMPTY)
			return ENOMEM;
		status = vm_get(realm, this_value, key, result);
		return status;
	}

	/* A DOCTYPE's is its name. */
	if (node->type == DOM_DOCUMENT_TYPE) {
		*result = vm_value_cell(((struct dom_doctype *)node)->name);
		return 0;
	}

	/* Processing instructions report their actual immutable native target. */
	if (node->type == DOM_PROCESSING_INSTRUCTION) {
		*result = vm_value_cell(((struct dom_character_data *)node)->target);
		return 0;
	}

	/* The others have the names of their kinds. */
	name = "#document-fragment";
	if (node->type == DOM_TEXT) {
		name = "#text";
	} else if (node->type == DOM_CDATA_SECTION) {
		name = "#cdata-section";
	} else if (node->type == DOM_COMMENT) {
		name = "#comment";
	} else if (node->type == DOM_DOCUMENT) {
		name = "#document";
	}

	/* The name as a string. */
	status = bind_string(realm, name, result);
	if (status != 0)
		return status;

	/* Succeeded: the name is reported. */
	return 0;
}

/* Reports the node's value (nodeValue): a text or comment node's data, null for the others. */
static int
node_value_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_character_data *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Only character data has a value. */
	if (node->type != DOM_TEXT &&
	    node->type != DOM_CDATA_SECTION &&
	    node->type != DOM_PROCESSING_INSTRUCTION &&
	    node->type != DOM_COMMENT) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* The characters. */
	text = (struct dom_character_data *)node;
	status = bind_units(realm, text->data.data, text->data.length, result);
	if (status != 0)
		return status;

	/* Succeeded: the data is reported. */
	return 0;
}

/* Sets the node's value (nodeValue): a text or comment node's data; nothing for the others. */
static int
node_value_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct vm_string *string;
	struct wb_units units;
	vm_value value;
	int status;

	/* The node. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Only character data has a value to set. */
	if (node->type != DOM_TEXT &&
	    node->type != DOM_CDATA_SECTION &&
	    node->type != DOM_PROCESSING_INSTRUCTION &&
	    node->type != DOM_COMMENT)
		return 0;

	/* The new text (null is the empty string). */
	value = js_argument(args, count, 0);
	wb_units_init(&units);
	if (value != VM_VALUE_NULL) {
		status = bind_to_string(realm, value, &string);
		if (status == 0)
			status = vm_string_append_units(string, &units);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* The node takes it. */
	status = dom_text_set(node, units.data, units.length);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the data is set. */
	return 0;
}

/* Reports the node's text (textContent): its data, its descendants' text, or null for a document or DOCTYPE. */
static int
node_text_content_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct wb_units units;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Character data reports its data, as nodeValue does. */
	if (node->type == DOM_TEXT ||
	    node->type == DOM_CDATA_SECTION ||
	    node->type == DOM_PROCESSING_INSTRUCTION ||
	    node->type == DOM_COMMENT) {
		status = node_value_get(realm, this_value, args, count, result);
		return status;
	}

	/* A document and a DOCTYPE have no text. */
	if (node->type == DOM_DOCUMENT || node->type == DOM_DOCUMENT_TYPE) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* An element or a fragment has its descendants' text. */
	wb_units_init(&units);
	status = bind_text_content(node, &units);
	if (status == 0)
		status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the text is reported. */
	return 0;
}

/* Sets the node's text (textContent): replaces an element's or fragment's children with one text node. */
static int
node_text_content_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *node;
	struct dom_node *removed;
	struct dom_node *text;
	struct vm_string *string;
	struct wb_units units;
	vm_value value;
	int status;

	/* The node. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;
	window = bind_window_of(realm);

	/* Character data sets its data, as nodeValue does. */
	if (node->type == DOM_TEXT ||
	    node->type == DOM_CDATA_SECTION ||
	    node->type == DOM_PROCESSING_INSTRUCTION ||
	    node->type == DOM_COMMENT) {
		status = node_value_set(realm, this_value, args, count, result);
		return status;
	}

	/* A document and a DOCTYPE ignore it. */
	if (node->type == DOM_DOCUMENT || node->type == DOM_DOCUMENT_TYPE)
		return 0;

	/* The new text (null is the empty string). */
	value = js_argument(args, count, 0);
	wb_units_init(&units);
	if (value != VM_VALUE_NULL) {
		status = bind_to_string(realm, value, &string);
		if (status == 0)
			status = vm_string_append_units(string, &units);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* The children go. */
	while (node->first_child != NULL) {
		removed = node->first_child;
		dom_remove(removed);
		status = bind_environment_child_mutation(window, node, NULL, removed);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* One text node takes their place, unless the text is empty. */
	if (units.length != 0) {
		text = dom_text_create(node->document, units.data, units.length);
		if (text == NULL) {
			wb_units_release(&units);
			return ENOMEM;
		}

		/* The node holds the text. */
		dom_append_child(node, text);
		status = bind_environment_child_mutation(window, node, text, NULL);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* The text was copied into the node. */
	wb_units_release(&units);

	/* Succeeded: the node holds the text. */
	return 0;
}

/* Reports the node's parent (parentNode). */
static int
node_parent_node(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Its parent, or null. */
	status = node_link(realm, this_value, node->parent, result);
	if (status != 0)
		return status;

	/* Succeeded: the parent is reported. */
	return 0;
}

/* Reports the node's parent when it is an element (parentElement). */
static int
node_parent_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *parent;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* A parent that is not an element does not count. */
	parent = node->parent;
	if (parent != NULL && parent->type != DOM_ELEMENT)
		parent = NULL;

	/* The parent, or null. */
	status = node_link(realm, this_value, parent, result);
	if (status != 0)
		return status;

	/* Succeeded: the parent element is reported. */
	return 0;
}

/* Reports the node's first child (firstChild). */
static int
node_first_child(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Its first child, or null. */
	status = node_link(realm, this_value, node->first_child, result);
	if (status != 0)
		return status;

	/* Succeeded: the child is reported. */
	return 0;
}

/* Reports the node's last child (lastChild). */
static int
node_last_child(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Its last child, or null. */
	status = node_link(realm, this_value, node->last_child, result);
	if (status != 0)
		return status;

	/* Succeeded: the child is reported. */
	return 0;
}

/* Reports the node's previous sibling (previousSibling). */
static int
node_previous_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The sibling, or null. */
	status = node_link(realm, this_value, node->previous, result);
	if (status != 0)
		return status;

	/* Succeeded: the sibling is reported. */
	return 0;
}

/* Reports the node's next sibling (nextSibling). */
static int
node_next_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The sibling, or null. */
	status = node_link(realm, this_value, node->next, result);
	if (status != 0)
		return status;

	/* Succeeded: the sibling is reported. */
	return 0;
}

/* Reports the node's document (ownerDocument), null for a document. */
static int
node_owner_document(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *document;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* A document has no owner. */
	document = &node->document->node;
	if (node->type == DOM_DOCUMENT)
		document = NULL;

	/* The document, or null. */
	status = node_link(realm, this_value, document, result);
	if (status != 0)
		return status;

	/* Succeeded: the document is reported. */
	return 0;
}

/* Reports whether the node is in its document's tree (isConnected). */
static int
node_is_connected(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int connected;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Connected when the document is its inclusive ancestor. */
	connected = dom_is_inclusive_ancestor(&node->document->node, node);

	/* Succeeded: the answer is reported. */
	*result = vm_value_boolean(connected);
	return 0;
}

/* Reports the node's children as an array (childNodes). */
static int
node_child_nodes(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *node;
	struct dom_node *child;
	struct vm_object *array;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node and an empty list. */
	window = bind_window_of(realm);
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;
	status = bind_array_create(realm, &array);
	if (status != 0)
		return status;

	/* Each child in order. */
	for (child = node->first_child; child != NULL; child = child->next) {
		status = bind_array_push_node(window, array, child);
		if (status != 0)
			return status;
	}

	/* Succeeded: the list is reported. */
	*result = vm_value_cell(array);
	return 0;
}

/* Appends a node to the node's children and reports it (appendChild). */
static int
node_append_child(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *parent;
	struct dom_node *child;
	int status;

	/* The parent and the node. */
	status = bind_this_node(realm, this_value, &parent);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 0), &child);
	if (status != 0)
		return status;

	/* The insertion at the end. */
	status = bind_insert(realm, parent, child, NULL);
	if (status != 0)
		return status;

	/* Succeeded: the node is reported. */
	*result = js_argument(args, count, 0);
	return 0;
}

/* Inserts a node before a child of the node, or at the end for null, and reports it (insertBefore). */
static int
node_insert_before(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *parent;
	struct dom_node *child;
	struct dom_node *reference;
	vm_value reference_value;
	int status;

	/* The parent and the node. */
	status = bind_this_node(realm, this_value, &parent);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 0), &child);
	if (status != 0)
		return status;

	/* The reference child: null (or undefined) is the end. */
	reference = NULL;
	reference_value = js_argument(args, count, 1);
	if (reference_value != VM_VALUE_NULL && reference_value != VM_VALUE_UNDEFINED) {
		status = bind_argument_node(realm, reference_value, &reference);
		if (status != 0)
			return status;
	}

	/* The insertion. */
	status = bind_insert(realm, parent, child, reference);
	if (status != 0)
		return status;

	/* Succeeded: the node is reported. */
	*result = js_argument(args, count, 0);
	return 0;
}

/* Removes a child of the node and reports it (removeChild). */
static int
node_remove_child(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *parent;
	struct dom_node *child;
	int status;

	/* The parent and the child. */
	status = bind_this_node(realm, this_value, &parent);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 0), &child);
	if (status != 0)
		return status;

	/* It must be the parent's child. */
	if (child->parent != parent) {
		status = bind_throw_dom(realm, "NotFoundError", "The node to be removed is not a child of this node.");
		return status;
	}

	/* The removal. */
	window = bind_window_of(realm);
	dom_remove(child);
	status = bind_environment_child_mutation(window, parent, NULL, child);
	if (status != 0)
		return status;

	/* Succeeded: the child is reported. */
	*result = js_argument(args, count, 0);
	return 0;
}

/* Replaces a child of the node with a node and reports the child (replaceChild). */
static int
node_replace_child(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *parent;
	struct dom_node *node;
	struct dom_node *child;
	struct dom_node *reference;
	int status;

	/* The parent, the new node and the child it replaces. */
	status = bind_this_node(realm, this_value, &parent);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 0), &node);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 1), &child);
	if (status != 0)
		return status;

	/* The child must be the parent's. */
	if (child->parent != parent) {
		status = bind_throw_dom(realm, "NotFoundError", "The node to be replaced is not a child of this node.");
		return status;
	}

	/* A node replacing itself stays where it is. */
	if (node == child) {
		*result = js_argument(args, count, 1);
		return 0;
	}

	/* The new node goes where the child was, and the child goes. */
	window = bind_window_of(realm);
	reference = child->next;
	if (reference == node)
		reference = node->next;
	dom_remove(child);
	status = bind_environment_child_mutation(window, parent, NULL, child);
	if (status != 0)
		return status;
	status = bind_insert(realm, parent, node, reference);
	if (status != 0)
		return status;

	/* Succeeded: the replaced child is reported. */
	*result = js_argument(args, count, 1);
	return 0;
}

/* Reports whether the node has children (hasChildNodes). */
static int
node_has_child_nodes(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Whether it has a first child. */
	*result = VM_VALUE_FALSE;
	if (node->first_child != NULL)
		*result = VM_VALUE_TRUE;

	/* Succeeded: the answer is reported. */
	return 0;
}

/* Reports whether a node is the node or one of its descendants (contains). */
static int
node_contains(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *other;
	int contained;
	int status;

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* null (or any other value) is not contained. */
	other = bind_node_of(js_argument(args, count, 0));
	if (other == NULL) {
		*result = VM_VALUE_FALSE;
		return 0;
	}

	/* Contained when the node is its inclusive ancestor. */
	contained = dom_is_inclusive_ancestor(node, other);

	/* Succeeded: the answer is reported. */
	*result = vm_value_boolean(contained);
	return 0;
}

/* Copies a complete native graph and retains the result through actual owner wrapping. */
static int
node_clone(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *node;
	struct dom_node *clone;
	struct vm_cell *source_root;
	struct vm_cell *copy_root;
	int deep;
	int status;

	/* Native receiver validation precedes allocation or unsupported-kind errors. */
	window = bind_window_of(realm);
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;
	deep = vm_to_boolean(js_argument(args, count, 0));

	/* Callee roots cover direct embedding invocations without an outer VM receiver frame. */
	source_root = &node->cell;
	copy_root = NULL;
	status = vm_heap_add_root(realm->heap, &source_root);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &copy_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &source_root);
		return status;
	}

	/* Document cloning remains outside the existing supported node kinds. */
	if (node->type == DOM_DOCUMENT) {
		status = bind_throw_dom(realm, "NotSupportedError", "Cloning a document is not supported.");
		vm_heap_remove_root(realm->heap, &copy_root);
		vm_heap_remove_root(realm->heap, &source_root);
		return status;
	}

	/* The checked shared helper publishes only a complete native copy. */
	status = bind_clone_node(realm, node, deep, &clone);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &copy_root);
		vm_heap_remove_root(realm->heap, &source_root);
		return status;
	}

	/* Retain the native result across wrapper allocation. */
	copy_root = &clone->cell;

	/* Current native ownership determines the wrapper even through a borrowed method. */
	status = bind_wrap(window, clone, result);
	vm_heap_remove_root(realm->heap, &copy_root);
	vm_heap_remove_root(realm->heap, &source_root);
	if (status != 0)
		return status;

	/* Succeeded: the caller receives an independent complete node graph. */
	return 0;
}

/* Reports a node the attribute leads to (a parent, a child, a sibling), or null. */
static int
node_link(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_node *linked,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The node's object, or null. */
	window = bind_window_of(realm);
	status = bind_wrap_or_null(window, linked, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is reported. */
	return 0;
}

/* Reports the interface whose prototype a node's object gets. */
static int
node_prototype_index(
	const struct dom_node *node)
{
	const struct dom_element *element;
	int form;
	const char *table_name;

	/* The kind of node decides, and an element's namespace. */
	switch (node->type) {
	case DOM_ELEMENT:
		element = (const struct dom_element *)node;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IMG)
			return BIND_HTML_IMAGE_ELEMENT;

		/* Media elements play through the host (ws121-p005). */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_VIDEO)
			return BIND_HTML_VIDEO_ELEMENT;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_AUDIO)
			return BIND_HTML_AUDIO_ELEMENT;
		/* Inline style wrappers preserve exact local case in XML-owned HTML nodes. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_STYLE) {
			form = vm_string_equal_ascii(element->local_name, "style");
			if (form)
				return BIND_HTML_STYLE_ELEMENT;
		}

		/* Script elements retain their existing native interface. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_SCRIPT)
			return BIND_HTML_SCRIPT_ELEMENT;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_TEMPLATE)
			return BIND_HTML_TEMPLATE_ELEMENT;
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_IFRAME)
			return BIND_HTML_IFRAME_ELEMENT;
		/* Reflected HTML attributes live on their exact tagged interfaces. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_BUTTON)
			return BIND_HTML_BUTTON_ELEMENT;

		/* Labels expose the for content attribute through their htmlFor alias. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_LABEL)
			return BIND_HTML_LABEL_ELEMENT;

		/* Metadata keeps the http-equiv alias separate from arbitrary expandos. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_META)
			return BIND_HTML_META_ELEMENT;

		/* Objects expose data through their own reflected URL interface. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_OBJECT)
			return BIND_HTML_OBJECT_ELEMENT;

		/* A textarea's value (ws177-p019), by its exact local name. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_TEXTAREA) {
			form = vm_string_equal_ascii(element->local_name, "textarea");
			if (form)
				return BIND_HTML_TEXT_AREA_ELEMENT;
		}

		/* Inputs use exact local identity independently of XML's folded internal tag. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_INPUT) {
			form = vm_string_equal_ascii(element->local_name, "input");
			if (form)
				return BIND_HTML_INPUT_ELEMENT;
		}

		/* Form interfaces require exact local case even in an XML Document. */
		if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_FORM) {
			form = vm_string_equal_ascii(element->local_name, "form");
			if (form)
				return BIND_HTML_FORM_ELEMENT;
		}

		/* Table-family prototypes require exact local names even in factory XML documents. */
		if (element->ns == DOM_NS_HTML &&
		    (element->tag == DOM_TAG_TABLE ||
		     element->tag == DOM_TAG_THEAD ||
		     element->tag == DOM_TAG_TBODY ||
		     element->tag == DOM_TAG_TFOOT ||
		     element->tag == DOM_TAG_TR ||
		     element->tag == DOM_TAG_CAPTION)) {
			table_name = dom_tag_name(element->tag);
			form = vm_string_equal_ascii(element->local_name, table_name);
			if (form) {
				/* Different table roots expose distinct branded live collection getters. */
				switch (element->tag) {
				case DOM_TAG_TABLE:
					return BIND_HTML_TABLE_ELEMENT;
				case DOM_TAG_CAPTION:
					return BIND_HTML_TABLE_CAPTION_ELEMENT;
				case DOM_TAG_THEAD:
				case DOM_TAG_TBODY:
				case DOM_TAG_TFOOT:
					return BIND_HTML_TABLE_SECTION_ELEMENT;
				case DOM_TAG_TR:
					return BIND_HTML_TABLE_ROW_ELEMENT;
				default:
					break;
				}
			}
		}

		/* Select-family identities retain exact local case in XML factory Documents. */
		if (element->ns == DOM_NS_HTML &&
		    (element->tag == DOM_TAG_SELECT ||
		     element->tag == DOM_TAG_OPTION ||
		     element->tag == DOM_TAG_OPTGROUP)) {
			table_name = dom_tag_name(element->tag);
			form = vm_string_equal_ascii(element->local_name, table_name);
			if (form) {
				/* Each exact native identity supplies its own inherited prototype. */
				switch (element->tag) {
				case DOM_TAG_SELECT:
					return BIND_HTML_SELECT_ELEMENT;
				case DOM_TAG_OPTION:
					return BIND_HTML_OPTION_ELEMENT;
				case DOM_TAG_OPTGROUP:
					return BIND_HTML_OPT_GROUP_ELEMENT;
				default:
					break;
				}
			}
		}

		/* Canonical SVG namespace and exact local case select actual native text interfaces. */
		if (element->ns == DOM_NS_SVG) {
			form = bind_svg_node_interface(element);
			return form;
		}

		/* All other HTML elements retain the ordinary HTMLElement interface. */
		if (element->ns == DOM_NS_HTML)
			return BIND_HTML_ELEMENT;
		return BIND_ELEMENT;
	case DOM_TEXT:
		return BIND_TEXT;
	case DOM_CDATA_SECTION:
		return BIND_CDATA_SECTION;
	case DOM_PROCESSING_INSTRUCTION:
		return BIND_PROCESSING_INSTRUCTION;
	case DOM_COMMENT:
		return BIND_COMMENT;
	case DOM_DOCUMENT:
		/* XML factory Documents have their own inherited Document interface. */
		if (node->document->content != DOM_CONTENT_HTML)
			return BIND_XML_DOCUMENT;

		/* Parser-created HTML documents retain their existing interface. */
		return BIND_DOCUMENT;
	case DOM_DOCUMENT_TYPE:
		return BIND_DOCUMENT_TYPE;
	default:
		break;
	}

	/* A document fragment. */
	return BIND_DOCUMENT_FRAGMENT;
}
