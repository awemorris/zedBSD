/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The node interfaces besides Element and Document: CharacterData with
 * Text and Comment, DocumentType and DocumentFragment.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

static int text_split(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static void text_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);
static int text_this(struct vm_realm *realm, vm_value this_value, struct dom_character_data **text);
static int text_data_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int text_data_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int text_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int text_append_data(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int text_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int comment_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int text_make(struct vm_realm *realm, int type, const vm_value *args, unsigned count, vm_value *result);
static int doctype_this(struct vm_realm *realm, vm_value this_value, struct dom_doctype **doctype);
static int doctype_name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int doctype_public_id(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int doctype_system_id(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int fragment_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of CharacterData, with the element siblings of
 * NonDocumentTypeChildNode.  The table is constant for the life of the
 * program.
 */
static const struct bind_attribute character_data_attributes[] = {
	{ "data", text_data_get, text_data_set },
	{ "length", text_length, NULL },
	{ "previousElementSibling", bind_previous_element_sibling, NULL },
	{ "nextElementSibling", bind_next_element_sibling, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of CharacterData, with ChildNode's remove.  The table is
 * constant for the life of the program.
 */
static const struct bind_operation character_data_operations[] = {
	{ "appendData", 1, text_append_data },
	{ "remove", 0, bind_remove },
	{ NULL, 0, NULL }
};

/*
 * The CharacterData interface.
 */
const struct bind_interface bind_character_data_interface = {
	"CharacterData", BIND_NODE, 0, NULL, character_data_attributes, character_data_operations, NULL
};

/* The concrete Text split operation follows current native data and owner graphs. */
static const struct bind_operation text_operations[] = {
	{ "splitText", 1, text_split },
	{ NULL, 0, NULL }
};

/*
 * The Text interface (new Text(data) makes a text node).
 */
const struct bind_interface bind_text_interface = {
	"Text", BIND_CHARACTER_DATA, 0, text_construct, NULL, text_operations, NULL
};

/*
 * The Comment interface (new Comment(data) makes a comment).
 */
const struct bind_interface bind_comment_interface = {
	"Comment", BIND_CHARACTER_DATA, 0, comment_construct, NULL, NULL, NULL
};

/*
 * The attributes of DocumentType.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute doctype_attributes[] = {
	{ "name", doctype_name, NULL },
	{ "publicId", doctype_public_id, NULL },
	{ "systemId", doctype_system_id, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of DocumentType: ChildNode's remove.  The table is
 * constant for the life of the program.
 */
static const struct bind_operation doctype_operations[] = {
	{ "remove", 0, bind_remove },
	{ NULL, 0, NULL }
};

/*
 * The DocumentType interface.
 */
const struct bind_interface bind_document_type_interface = {
	"DocumentType", BIND_NODE, 0, NULL, doctype_attributes, doctype_operations, NULL
};

/*
 * The attributes of DocumentFragment: ParentNode's.  The table is
 * constant for the life of the program.
 */
static const struct bind_attribute fragment_attributes[] = {
	{ "children", bind_children, NULL },
	{ "firstElementChild", bind_first_element_child_get, NULL },
	{ "lastElementChild", bind_last_element_child_get, NULL },
	{ "childElementCount", bind_child_element_count, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of DocumentFragment: ParentNode's.  The table is
 * constant for the life of the program.
 */
static const struct bind_operation fragment_operations[] = {
	{ "append", 0, bind_append },
	{ "prepend", 0, bind_prepend },
	{ "replaceChildren", 0, bind_replace_children },
	{ "querySelector", 1, bind_query_selector },
	{ "querySelectorAll", 1, bind_query_selector_all },
	{ NULL, 0, NULL }
};

/*
 * The DocumentFragment interface (new DocumentFragment() makes one).
 */
const struct bind_interface bind_document_fragment_interface = {
	"DocumentFragment", BIND_NODE, 0, fragment_construct, fragment_attributes, fragment_operations, NULL
};

/*
 * Splits genuine native Text while retaining both halves through actual host callbacks.
 */
int
bind_split_text(
	struct vm_realm *realm,
	struct dom_node *node,
	uint32_t offset,
	struct dom_node **created)
{
	struct dom_character_data *text;
	struct dom_node *suffix;
	struct dom_node *parent;
	struct bind_window *window;
	struct vm_cell *roots[2];
	const uint16_t *units;
	size_t length;
	unsigned index;
	int status;

	/* Invalid embedding arguments cannot start a split or publish an output. */
	if (realm == NULL ||
	    node == NULL ||
	    created == NULL)
		return EINVAL;

	/* The shared native helper supports only genuine Text in the invoking collector. */
	if ((node->type != DOM_TEXT &&
	     node->type != DOM_CDATA_SECTION) ||
	    node->document->heap != realm->heap)
		return EINVAL;

	/* Both original ownership and a pending suffix survive native allocation and host collection. */
	roots[0] = &node->cell;
	roots[1] = NULL;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			text_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Current characters determine bounds after the public method's reentrant conversion. */
	text = (struct dom_character_data *)node;
	if (offset > text->data.length) {
		status = bind_throw_dom(realm, "IndexSizeError", "The Text split offset exceeds its length.");
		text_unroot(realm->heap, roots, 2);
		return status;
	}

	/* Borrowed methods still deliver hooks to the actual current Document owner. */
	window = node->document->view;
	if (window == NULL)
		window = bind_window_of(realm);

	/* An embedding without a binding owner cannot deliver normal DOM environment hooks. */
	if (window == NULL) {
		text_unroot(realm->heap, roots, 2);
		return EINVAL;
	}

	/* Empty suffixes avoid pointer arithmetic on an empty native buffer. */
	length = text->data.length - offset;
	units = NULL;
	if (length != 0)
		units = text->data.data + offset;

	/* Allocate complete suffix data before changing the original tree or characters. */
	suffix = dom_text_create(node->document, units, length);
	if (suffix == NULL) {
		text_unroot(realm->heap, roots, 2);
		return ENOMEM;
	}

	/* Retain the complete suffix before insertion or any host callback can collect. */
	roots[1] = &suffix->cell;

	/* Attached splits repair insertion positions before transferring Text and equal parent points. */
	parent = node->parent;
	if (parent != NULL) {
		dom_insert_before(parent, suffix, node->next);
		dom_split_notify(node->document, node, suffix, offset);
	}

	/* Suffix truncation needs no allocation after the new Text has been inserted. */
	status = dom_text_truncate(node, offset);
	if (status != 0) {
		text_unroot(realm->heap, roots, 2);
		return status;
	}

	/* Actual host observers see the complete split and all repaired live boundaries. */
	if (parent != NULL) {
		status = bind_environment_child_mutation(window, parent, suffix, NULL);
		if (status != 0) {
			text_unroot(realm->heap, roots, 2);
			return status;
		}

		/* Optional host work retains both halves even if it collects or reports a failure. */
		if (window->host.node_inserted != NULL) {
			status = window->host.node_inserted(window->host.context, suffix);
			if (status != 0) {
				text_unroot(realm->heap, roots, 2);
				return status;
			}
		}
	}

	/* Publish the complete native suffix only after all checked split work succeeds. */
	*created = suffix;
	text_unroot(realm->heap, roots, 2);

	/* Succeeded: the caller receives a same-current-Document native Text suffix. */
	return 0;
}

/* Converts one current split offset while keeping native ownership independent of VM call frames. */
static int
text_split(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *created;
	struct bind_window *window;
	struct vm_cell *roots[3];
	uint32_t offset;
	unsigned index;
	int cell;
	int status;

	/* Exact Text branding precedes required arguments and numeric conversion. */
	*result = VM_VALUE_UNDEFINED;
	node = bind_node_of(receiver);
	if (node == NULL ||
	    (node->type != DOM_TEXT &&
	     node->type != DOM_CDATA_SECTION)) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A missing required offset cannot enter observable numeric conversion. */
	if (count < 1U) {
		status = vm_throw_type_error(realm, "Text.splitText requires an offset.");
		return status;
	}

	/* Foreign collector cells cannot be retained by these root slots. */
	if (node->document->heap != realm->heap)
		return EINVAL;

	/* Register genuine node, conversion input and pending suffix before any reentrant work. */
	roots[0] = &node->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	cell = vm_value_is_cell(args[0]);
	if (cell)
		roots[1] = vm_value_as_cell(args[0]);

	/* Conversion may remove or adopt the only otherwise reachable original Text. */
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			text_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* Resolve actual current data and owner only after one unsigned offset conversion. */
	status = vm_to_uint32(realm, args[0], &offset);
	if (status != 0) {
		text_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Split the genuine Text using its post-conversion data and owner. */
	status = bind_split_text(realm, node, offset, &created);
	if (status != 0) {
		text_unroot(realm->heap, roots, 3);
		return status;
	}

	/* Keep a detached returned suffix alive during wrapper allocation. */
	roots[2] = &created->cell;

	/* The actual current Document chooses the returned suffix's binding prototype. */
	window = bind_window_of(realm);
	status = bind_wrap(window, created, result);
	text_unroot(realm->heap, roots, 3);
	if (status != 0)
		return status;

	/* Succeeded: one genuine new Text wrapper is returned with no temporary root slots. */
	return 0;
}

/* Releases only the successfully registered temporary native root slots. */
static void
text_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	/* Reverse registration order keeps partial construction cleanup deterministic. */
	while (count != 0) {
		count--;
		vm_heap_remove_root(heap, &roots[count]);
	}

	/* Succeeded: the embedding holds no root slot on this returned C stack. */
	return;
}

/* Finds the text or comment node a method's this value stands for, throwing a TypeError otherwise. */
static int
text_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_character_data **text)
{
	struct dom_node *node;
	int status;

	/* The node, which must be character data. */
	node = bind_node_of(this_value);
	if (node == NULL ||
	    (node->type != DOM_TEXT &&
	     node->type != DOM_CDATA_SECTION &&
	     node->type != DOM_PROCESSING_INSTRUCTION &&
	     node->type != DOM_COMMENT)) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the node is found. */
	*text = (struct dom_character_data *)node;
	return 0;
}

/* Reports the node's characters (data). */
static int
text_data_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_character_data *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = text_this(realm, this_value, &text);
	if (status != 0)
		return status;

	/* Its characters as a string. */
	status = bind_units(realm, text->data.data, text->data.length, result);
	if (status != 0)
		return status;

	/* Succeeded: the data is reported. */
	return 0;
}

/* Replaces the node's characters (data; null is the empty string). */
static int
text_data_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_character_data *text;
	struct vm_string *string;
	struct wb_units units;
	vm_value value;
	int status;

	/* The node. */
	*result = VM_VALUE_UNDEFINED;
	status = text_this(realm, this_value, &text);
	if (status != 0)
		return status;

	/* The new characters. */
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

	/* The node takes them. */
	status = dom_text_set(&text->node, units.data, units.length);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the data is set. */
	return 0;
}

/* Reports how many UTF-16 code units the node holds (length). */
static int
text_length(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_character_data *text;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = text_this(realm, this_value, &text);
	if (status != 0)
		return status;

	/* Succeeded: the count is reported. */
	*result = vm_value_number((double)text->data.length);
	return 0;
}

/* Adds a string's characters at the end of the node's (appendData). */
static int
text_append_data(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_character_data *text;
	struct vm_string *string;
	struct wb_units units;
	int status;

	/* The node and the string. */
	*result = VM_VALUE_UNDEFINED;
	status = text_this(realm, this_value, &text);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;

	/* The string's characters at the end. */
	wb_units_init(&units);
	status = vm_string_append_units(string, &units);
	if (status == 0)
		status = dom_text_append(&text->node, units.data, units.length);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the characters are appended. */
	return 0;
}

/* Makes a text node of the window's document (new Text(data)). */
static int
text_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* A text node with the data. */
	status = text_make(realm, DOM_TEXT, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is reported. */
	return 0;
}

/* Makes a comment of the window's document (new Comment(data)). */
static int
comment_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* A comment with the data. */
	status = text_make(realm, DOM_COMMENT, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is reported. */
	return 0;
}

/* Makes a text node or a comment of the window's document from an optional data argument. */
static int
text_make(
	struct vm_realm *realm,
	int type,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_string *string;
	struct dom_node *node;
	struct wb_units units;
	vm_value value;
	int status;

	/* The data: the argument's string, or empty. */
	window = bind_window_of(realm);
	value = js_argument(args, count, 0);
	wb_units_init(&units);
	if (value != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, value, &string);
		if (status == 0)
			status = vm_string_append_units(string, &units);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* The node. */
	if (type == DOM_TEXT) {
		node = dom_text_create(window->document, units.data, units.length);
	} else {
		node = dom_comment_create(window->document, units.data, units.length);
	}

	/* The characters are in the node now. */
	wb_units_release(&units);
	if (node == NULL)
		return ENOMEM;

	/* Its object. */
	status = bind_wrap(window, node, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is made. */
	return 0;
}

/* Finds the DOCTYPE a method's this value stands for, throwing a TypeError otherwise. */
static int
doctype_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_doctype **doctype)
{
	struct dom_node *node;
	int status;

	/* The node, which must be a DOCTYPE. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_DOCUMENT_TYPE) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the DOCTYPE is found. */
	*doctype = (struct dom_doctype *)node;
	return 0;
}

/* Reports the DOCTYPE's name (name). */
static int
doctype_name(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_doctype *doctype;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The DOCTYPE. */
	status = doctype_this(realm, this_value, &doctype);
	if (status != 0)
		return status;

	/* Succeeded: the name is reported. */
	*result = vm_value_cell(doctype->name);
	return 0;
}

/* Reports the DOCTYPE's public identifier (publicId). */
static int
doctype_public_id(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_doctype *doctype;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The DOCTYPE. */
	status = doctype_this(realm, this_value, &doctype);
	if (status != 0)
		return status;

	/* Succeeded: the identifier is reported. */
	*result = vm_value_cell(doctype->public_id);
	return 0;
}

/* Reports the DOCTYPE's system identifier (systemId). */
static int
doctype_system_id(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_doctype *doctype;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The DOCTYPE. */
	status = doctype_this(realm, this_value, &doctype);
	if (status != 0)
		return status;

	/* Succeeded: the identifier is reported. */
	*result = vm_value_cell(doctype->system_id);
	return 0;
}

/* Makes an empty fragment of the window's document (new DocumentFragment()). */
static int
fragment_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *fragment;
	int status;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The fragment. */
	window = bind_window_of(realm);
	fragment = dom_fragment_create(window->document);
	if (fragment == NULL)
		return ENOMEM;

	/* Its object. */
	status = bind_wrap(window, fragment, result);
	if (status != 0)
		return status;

	/* Succeeded: the fragment is made. */
	return 0;
}
