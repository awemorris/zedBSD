/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Table child operations use current DOM links and retain mutation graphs across host callbacks. */

#include "bind/internal.h"

#include <errno.h>

/* One invocation reads, replaces, creates or removes a specific direct table child. */
enum table_action {
	TABLE_GET,
	TABLE_SET,
	TABLE_CREATE,
	TABLE_DELETE
};

/* Actual HTML caption nodes inherit HTMLElement and reject direct script construction. */
const struct bind_interface bind_html_table_caption_element_interface = {
    "HTMLTableCaptionElement", BIND_HTML_ELEMENT, 0, NULL, NULL, NULL, NULL};

static int table_html(const struct dom_node *node, int tag);
static struct dom_node *table_first(struct dom_node *table, int tag);
static struct dom_node *table_reference(struct dom_node *table, int tag);
static int table_access(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, int tag, enum table_action action, vm_value *result);
static int table_change(struct vm_realm *realm, struct dom_node *table, struct dom_node *existing, struct dom_node *incoming, int tag, enum table_action action, struct vm_cell **roots, vm_value *result);
static void table_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);

/*
 * Reads the table's actual caption child through the native structural algorithm.
 */
int
bind_table_caption_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_CAPTION, TABLE_GET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Replaces the table's actual caption child through the native structural algorithm.
 */
int
bind_table_caption_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_CAPTION, TABLE_SET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Creates or reuses the table's actual caption child through the native structural algorithm.
 */
int
bind_table_caption_create(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_CAPTION, TABLE_CREATE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Removes the table's actual caption child through the native structural algorithm.
 */
int
bind_table_caption_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_CAPTION, TABLE_DELETE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Reads the table's actual head child through the native structural algorithm.
 */
int
bind_table_head_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_THEAD, TABLE_GET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Replaces the table's actual head child through the native structural algorithm.
 */
int
bind_table_head_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_THEAD, TABLE_SET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Creates or reuses the table's actual head child through the native structural algorithm.
 */
int
bind_table_head_create(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_THEAD, TABLE_CREATE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Removes the table's actual head child through the native structural algorithm.
 */
int
bind_table_head_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_THEAD, TABLE_DELETE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Reads the table's actual foot child through the native structural algorithm.
 */
int
bind_table_foot_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_TFOOT, TABLE_GET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Replaces the table's actual foot child through the native structural algorithm.
 */
int
bind_table_foot_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_TFOOT, TABLE_SET, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Creates or reuses the table's actual foot child through the native structural algorithm.
 */
int
bind_table_foot_create(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_TFOOT, TABLE_CREATE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/*
 * Removes the table's actual foot child through the native structural algorithm.
 */
int
bind_table_foot_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The shared algorithm validates the receiver before selecting or changing children. */
	status = table_access(realm, receiver, args, count, DOM_TAG_TFOOT, TABLE_DELETE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested table child operation is reflected in current DOM links. */
	return 0;
}

/* Requires actual namespace, internal tag and exact local case, independently of prototypes. */
static int
table_html(
	const struct dom_node *node,
	int tag)
{
	const struct dom_element *element;
	const char *name;
	int equal;

	/* Objects and non-elements cannot acquire a table-family brand through script. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;

	/* A foreign namespace or another internal tag cannot acquire this interface. */
	element = (const struct dom_element *)node;
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;

	/* XML's folded internal tag does not grant the lowercase HTML interface. */
	name = dom_tag_name(tag);
	equal = vm_string_equal_ascii(element->local_name, name);
	if (!equal)
		return 0;

	/* Succeeded: this is the exact actual HTML element. */
	return 1;
}

/* Finds the first eligible direct child without following nested tables or wrappers. */
static struct dom_node *
table_first(
	struct dom_node *table,
	int tag)
{
	struct dom_node *child;
	int matches;

	/* Only direct children take part in table child accessors. */
	for (child = table->first_child; child != NULL; child = child->next) {
		matches = table_html(child, tag);
		if (matches)
			return child;
	}

	/* No eligible direct child exists. */
	return NULL;
}

/* Selects the canonical insertion position from the table's links after removal. */
static struct dom_node *
table_reference(
	struct dom_node *table,
	int tag)
{
	struct dom_node *child;
	int matches;

	/* A caption precedes every node, including whitespace and comments. */
	if (tag == DOM_TAG_CAPTION)
		return table->first_child;

	/* A footer follows all nodes without changing the physical placement of other sections. */
	if (tag == DOM_TAG_TFOOT)
		return NULL;

	/* A header precedes the first element other than actual HTML caption or colgroup. */
	for (child = table->first_child; child != NULL; child = child->next) {
		/* Text and comments do not provide a header insertion anchor. */
		if (child->type != DOM_ELEMENT)
			continue;

		/* Only the two actual HTML prefix elements remain before a newly inserted header. */
		matches = table_html(child, DOM_TAG_CAPTION);
		if (matches)
			continue;
		matches = table_html(child, DOM_TAG_COLGROUP);
		if (matches)
			continue;

		/* This is the first element that must follow the header. */
		return child;
	}

	/* An all-prefix table appends its new header after intervening text and comments. */
	return NULL;
}

/* Converts native brands and roots the complete mutation graph before invoking host callbacks. */
static int
table_access(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	int tag,
	enum table_action action,
	vm_value *result)
{
	struct dom_node *table;
	struct dom_node *existing;
	struct dom_node *incoming;
	struct vm_cell *roots[4];
	vm_value argument;
	unsigned index;
	int matches;
	int status;
	struct bind_window *window;

	/* Receiver validation precedes argument conversion and every DOM observation. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, receiver, &table);
	if (status != 0)
		return status;
	matches = table_html(table, DOM_TAG_TABLE);
	if (!matches) {
		status = vm_throw_type_error(realm, "Table operation requires an actual HTML table.");
		if (status != 0)
			return status;

		/* Succeeded: the VM accepted this native operation exception. */
		return 0;
	}

	/* Nullable interface conversion never invokes user coercion or trusts prototype inheritance. */
	incoming = NULL;
	if (action == TABLE_SET) {
		argument = js_argument(args, count, 0);
		if (argument != VM_VALUE_NULL && argument != VM_VALUE_UNDEFINED) {
			status = bind_argument_node(realm, argument, &incoming);
			if (status != 0)
				return status;
			matches = table_html(incoming, tag);
			if (!matches && tag != DOM_TAG_CAPTION) {
				matches = table_html(incoming, DOM_TAG_THEAD);
				if (!matches)
					matches = table_html(incoming, DOM_TAG_TBODY);
				if (!matches)
					matches = table_html(incoming, DOM_TAG_TFOOT);
			}

			/* An unrelated node fails interface conversion before any old child is removed. */
			if (!matches) {
				status = vm_throw_type_error(realm, "The new child has the wrong table interface.");
				if (status != 0)
					return status;

				/* Succeeded: the VM accepted this native operation exception. */
				return 0;
			}

			/* A genuine section with the wrong section name fails the table setter algorithm. */
			matches = table_html(incoming, tag);
			if (!matches) {
				status = bind_throw_dom(realm, "HierarchyRequestError", "The new section has the wrong table section name.");
				if (status != 0)
					return status;

				/* Succeeded: the VM accepted this native operation exception. */
				return 0;
			}
		}
	}

	/* Reads and repeated creates publish the first current eligible child without mutation. */
	existing = table_first(table, tag);
	if (action == TABLE_GET ||
	    (action == TABLE_CREATE && existing != NULL)) {
		window = bind_window_of(realm);
		status = bind_wrap_or_null(window, existing, result);
		if (status != 0)
			return status;

		/* Succeeded: the relevant Document supplies the existing child's native prototype. */
		return 0;
	}

	/* Owner, detached old child, pending new child and insertion anchor survive arbitrary host GC. */
	roots[0] = &table->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	if (existing != NULL)
		roots[1] = &existing->cell;
	if (incoming != NULL)
		roots[2] = &incoming->cell;

	/* Each successfully registered slot is unwound if a later root registration fails. */
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			table_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* The same cleanup applies after successful mutation, host failure or DOM exception. */
	status = table_change(realm, table, existing, incoming, tag, action, roots, result);
	if (status != 0) {
		table_unroot(realm->heap, roots, 4U);
		return status;
	}

	/* Releases graph roots only after the complete mutation result is checked. */
	table_unroot(realm->heap, roots, 4U);

	/* Succeeded: every temporary root has been removed after result publication. */
	return 0;
}

/* Performs one rooted mutation using native insertion and environment notifications. */
static int
table_change(
	struct vm_realm *realm,
	struct dom_node *table,
	struct dom_node *existing,
	struct dom_node *incoming,
	int tag,
	enum table_action action,
	struct vm_cell **roots,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *created;
	struct dom_node *reference;
	struct vm_string *name;
	const char *local;
	int status;

	/* Host notifications belong to the actual owner even through a borrowed primary method. */
	window = bind_window_of(realm);
	if (table->document->view != NULL)
		window = table->document->view;

	/* Creation allocates in the table's node Document before publishing any partial wrapper. */
	if (action == TABLE_CREATE) {
		local = dom_tag_name(tag);
		name = vm_atom_from_ascii(realm->heap, local);
		if (name == NULL)
			return ENOMEM;

		/* Retains the allocated name while creating the actual child element. */
		roots[2] = &name->cell;
		created = dom_element_create(table->document, DOM_NS_HTML, name, NULL);
		if (created == NULL)
			return ENOMEM;

		/* Retains the new child before any removal or host notification. */
		incoming = &created->node;
		roots[2] = &incoming->cell;
	}

	/* Replacement/removal follows the specified first-child removal before insertion validation. */
	if (existing != NULL) {
		dom_remove(existing);
		status = bind_environment_child_mutation(window, table, NULL, existing);
		if (status != 0)
			return status;
	}

	/* Null setters and delete methods finish after the first eligible removal. */
	if (incoming == NULL)
		return 0;

	/* Native insertion preserves cycle checks, removal repair and actual host notifications. */
	reference = table_reference(table, tag);
	if (reference != NULL)
		roots[3] = &reference->cell;
	status = bind_insert(realm, table, incoming, reference);
	if (status != 0)
		return status;

	/* Only create methods return the newly created relevant-realm wrapper. */
	if (action == TABLE_CREATE) {
		status = bind_wrap(window, incoming, result);
		if (status != 0)
			return status;
	}

	/* Succeeded: the specified insertion or removal is visible through live collections. */
	return 0;
}

/* Removes exactly the slots registered by one native table operation. */
static void
table_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	unsigned index;

	/* No host failure leaves a heap root pointing into an expired native invocation. */
	for (index = 0; index < count; index++) {
		vm_heap_remove_root(heap, &roots[index]);
	}

	/* Succeeded: the invocation has no remaining temporary roots. */
	return;
}
