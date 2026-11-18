/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DOM's mixins, whose members several interfaces share: ParentNode
 * (children, the first and last element child, append and prepend),
 * ChildNode (remove), NonDocumentTypeChildNode (the element siblings),
 * and the getElementsBy* methods of Document and Element.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

/*
 * One stable replacement snapshot retains native nodes after their original tree edges leave.
 * Every registered root belongs to heap until release, including partially acquired snapshots.
 */
struct mixin_node_snapshot {
	struct vm_heap *heap;
	struct dom_node **nodes;
	struct vm_cell **roots;
	size_t count;
	size_t registered;
};

static int mixin_snapshot_capture(struct dom_node *node, int children, struct mixin_node_snapshot *snapshot);
static void mixin_snapshot_release(struct mixin_node_snapshot *snapshot);
static int mixin_adopt_replacement(struct vm_realm *realm, struct dom_node *parent, struct dom_node *node);
static int mixin_move_replacement(struct vm_realm *realm, struct dom_node *parent, struct dom_node *node);
static int mixin_insert_values(struct vm_realm *realm, struct dom_node *parent, const vm_value *args, unsigned count, struct dom_node *reference);
static int mixin_element_link(struct vm_realm *realm, struct dom_node *node, vm_value *result);
static int mixin_name_matches(const struct dom_element *element, const struct vm_string *name, const struct vm_string *lower);

/*
 * Reports a parent's first element child (firstElementChild).
 */
int
bind_first_element_child_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *first;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Its first element child, or null. */
	first = bind_first_element_child(node);
	status = mixin_element_link(realm, (struct dom_node *)first, result);
	if (status != 0)
		return status;

	/* Succeeded: the child is reported. */
	return 0;
}

/*
 * Reports a parent's last element child (lastElementChild).
 */
int
bind_last_element_child_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *child;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The last child that is an element. */
	for (child = node->last_child; child != NULL; child = child->previous) {
		if (child->type == DOM_ELEMENT)
			break;
	}

	/* The child, or null. */
	status = mixin_element_link(realm, child, result);
	if (status != 0)
		return status;

	/* Succeeded: the child is reported. */
	return 0;
}

/*
 * Reports how many element children a parent has (childElementCount).
 */
int
bind_child_element_count(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *child;
	int32_t elements;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Counts the element children. */
	elements = 0;
	for (child = node->first_child; child != NULL; child = child->next) {
		if (child->type == DOM_ELEMENT)
			elements++;
	}

	/* Succeeded: the count is reported. */
	*result = vm_value_int32(elements);
	return 0;
}

/*
 * Appends nodes and strings (as text) to a parent's children (append).
 */
int
bind_append(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	/* The parent. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The arguments at the end, in order. */
	status = mixin_insert_values(realm, node, args, count, NULL);
	if (status != 0)
		return status;

	/* Succeeded: the nodes are appended. */
	return 0;
}

/*
 * Inserts nodes and strings (as text) before a parent's first child
 * (prepend).
 */
int
bind_prepend(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	int status;

	/* The parent. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The arguments before the first child, in order. */
	status = mixin_insert_values(realm, node, args, count, node->first_child);
	if (status != 0)
		return status;

	/* Succeeded: the nodes are prepended. */
	return 0;
}

/*
 * Replaces every child after converting and validating the complete argument list.
 * Temporary native roots preserve converted text and detached nodes across allocations.
 */
int
bind_replace_children(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_heap *heap;
	struct dom_node *parent;
	struct dom_node *node;
	struct dom_node *replacement;
	struct vm_string *string;
	struct vm_cell **converted;
	struct vm_cell *roots[3];
	struct wb_units units;
	struct mixin_node_snapshot added = {0};
	struct mixin_node_snapshot removed = {0};
	unsigned index;
	unsigned registered;
	unsigned fixed_registered;
	int status;

	/* Only ParentNode receivers can provide the destination Document and heap. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &parent);
	if (status != 0)
		return status;

	/* Borrowing this method does not add ParentNode to CharacterData or DocumentType. */
	if (parent->type != DOM_ELEMENT &&
	    parent->type != DOM_DOCUMENT &&
	    parent->type != DOM_DOCUMENT_FRAGMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Stable slots must not move after registration with the owning heap. */
	heap = parent->document->heap;
	converted = NULL;
	if (count != 0) {
		converted = calloc(count, sizeof(*converted));
		if (converted == NULL)
			return ENOMEM;
	}

	/* The parent, temporary fragment and detached outgoing node have independent owners. */
	memset(roots, 0, sizeof(roots));
	roots[0] = &parent->cell;
	registered = 0;
	fixed_registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup releases only slots successfully published to the heap. */
		fixed_registered++;
	}

	/* All string conversions finish before fragment assembly can move an argument. */
	for (index = 0; index < count; index++) {
		/* Native Node arguments retain identity, while every other value uses ToString. */
		node = bind_node_of(args[index]);
		if (node == NULL) {
			status = bind_to_string(realm, args[index], &string);
			if (status != 0)
				goto cleanup;

			/* Copy UTF-16 units before allocating the new collectible Text node. */
			wb_units_init(&units);
			status = vm_string_append_units(string, &units);
			if (status != 0) {
				wb_units_release(&units);
				goto cleanup;
			}

			/* The new Text owns its copy, and the native character buffer can leave. */
			node = dom_text_create(parent->document, units.data, units.length);
			wb_units_release(&units);
			if (node == NULL) {
				status = ENOMEM;
				goto cleanup;
			}
		}

		/* The existing adoption implementation cannot transfer cells between independent heaps. */
		if (node->document->heap != heap) {
			status = EINVAL;
			goto cleanup;
		}

		/* Earlier converted nodes survive later user conversion and fragment allocation. */
		converted[index] = &node->cell;
		status = vm_heap_add_root(heap, &converted[index]);
		if (status != 0)
			goto cleanup;

		/* Each registered argument owns one cleanup obligation, including duplicate nodes. */
		registered++;
	}

	/* A single argument is used directly; every other arity is converted through a fragment. */
	if (count == 1U) {
		replacement = (struct dom_node *)converted[0];
	} else {
		replacement = dom_fragment_create(parent->document);
		if (replacement == NULL) {
			status = ENOMEM;
			goto cleanup;
		}

		/* The fragment retains assembled children without replacing per-argument roots. */
		roots[1] = &replacement->cell;

		/* Appending through the checked insertion path preserves duplicate-node move order. */
		for (index = 0; index < count; index++) {
			node = (struct dom_node *)converted[index];
			status = mixin_move_replacement(realm, replacement, node);
			if (status != 0)
				goto cleanup;
		}
	}

	/* Invalid replacement leaves every still-existing destination child in place. */
	status = bind_validate_replace_all(realm, parent, replacement);
	if (status != 0)
		goto cleanup;

	/* Resolve notifications against the actual destination rather than the borrowed method realm. */
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;

	/* Detached outgoing children remain alive until the complete replacement record owns them. */
	status = mixin_snapshot_capture(parent, 1, &removed);
	if (status != 0)
		goto cleanup;

	/* Capture the incoming sequence before insertion empties any argument fragment. */
	if (replacement->type == DOM_DOCUMENT_FRAGMENT) {
		status = mixin_snapshot_capture(replacement, 1, &added);
	} else {
		status = mixin_snapshot_capture(replacement, 0, &added);
	}

	/* No child can leave the destination until both independent snapshots exist. */
	if (status != 0)
		goto cleanup;

	/* Native removal repairs ranges, iterators, embedded views and style generations. */
	while (parent->first_child != NULL) {
		node = parent->first_child;
		roots[2] = &node->cell;
		dom_remove(node);
	}

	/* The validated replacement now enters the empty destination through ordinary insertion. */
	status = mixin_move_replacement(realm, parent, replacement);
	if (status != 0)
		goto cleanup;

	/* Replace-all contributes one target record, and empty-to-empty replacement contributes none. */
	if (added.count != 0 || removed.count != 0) {
		status = bind_environment_children_mutation(window, parent, added.nodes, added.count, removed.nodes, removed.count, NULL, NULL);
		if (status != 0)
			goto cleanup;
	}

cleanup:
	/* Complete and partially acquired snapshots release every registered node before native storage. */
	mixin_snapshot_release(&added);
	mixin_snapshot_release(&removed);

	/* Detached argument ownership lasts through all native and observer notifications. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &converted[registered]);
	}

	/* Release each acquired fixed slot before its stack storage disappears. */
	while (fixed_registered != 0) {
		fixed_registered--;
		vm_heap_remove_root(heap, &roots[fixed_registered]);
	}

	/* The stable slot array is native storage and has no remaining heap registrations. */
	free(converted);

	/* Failed conversion, validation or notification propagates after ownership cleanup. */
	if (status != 0)
		return status;

	/* Succeeded: the destination contains the converted replacement children. */
	return 0;
}

/*
 * Removes a node from its parent (remove).
 */
int
bind_remove(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *parent;
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* Out of its parent, if it has one. */
	window = bind_window_of(realm);
	parent = node->parent;
	dom_remove(node);
	if (parent != NULL) {
		status = bind_environment_child_mutation(window, parent, NULL, node);
		if (status != 0)
			return status;
	}

	/* Succeeded: the node is detached. */
	return 0;
}

/*
 * Reports the element before a node among its siblings
 * (previousElementSibling).
 */
int
bind_previous_element_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *sibling;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The nearest previous sibling that is an element. */
	for (sibling = node->previous; sibling != NULL; sibling = sibling->previous) {
		if (sibling->type == DOM_ELEMENT)
			break;
	}

	/* The sibling, or null. */
	status = mixin_element_link(realm, sibling, result);
	if (status != 0)
		return status;

	/* Succeeded: the sibling is reported. */
	return 0;
}

/*
 * Reports the element after a node among its siblings
 * (nextElementSibling).
 */
int
bind_next_element_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *sibling;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The node. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* The nearest next sibling that is an element. */
	for (sibling = node->next; sibling != NULL; sibling = sibling->next) {
		if (sibling->type == DOM_ELEMENT)
			break;
	}

	/* The sibling, or null. */
	status = mixin_element_link(realm, sibling, result);
	if (status != 0)
		return status;

	/* Succeeded: the sibling is reported. */
	return 0;
}

/*
 * Reports the descendant elements with a qualified name, or all of them
 * for "*", in tree order (getElementsByTagName).
 */
int
bind_get_elements_by_tag_name(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *root;
	struct dom_node *walk;
	struct vm_string *name;
	struct vm_string *lower;
	struct vm_object *array;
	struct vm_cell *roots[4];
	unsigned index;
	unsigned registered;
	int matches;
	int status;

	/* The root, the name as it is and folded, and an empty list. */
	window = bind_window_of(realm);
	status = bind_this_node(realm, this_value, &root);
	if (status != 0)
		return status;

	/* Retain query strings and the unpublished result across wrapper allocations. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only acquired registrations belong to cleanup. */
		registered++;
	}

	/* Both query spellings stay live until the descendant walk finishes. */
	status = bind_to_atom(realm, js_argument(args, count, 0), 0, &name);
	if (status != 0)
		goto cleanup;
	roots[1] = &name->cell;
	status = bind_to_atom(realm, js_argument(args, count, 0), 1, &lower);
	if (status != 0)
		goto cleanup;
	roots[2] = &lower->cell;
	status = bind_array_create(realm, &array);
	if (status != 0)
		goto cleanup;
	roots[3] = &array->cell;

	/* Each descendant element whose name matches. */
	for (walk = bind_following(root, root); walk != NULL; walk = bind_following(walk, root)) {
		if (walk->type != DOM_ELEMENT)
			continue;
		matches = mixin_name_matches((const struct dom_element *)walk, name, lower);
		if (!matches)
			continue;
		status = bind_array_push_node(window, array, walk);
		if (status != 0)
			goto cleanup;
	}

	/* Succeeded: the list is reported. */
	*result = vm_value_cell(array);
	status = 0;

cleanup:
	/* No temporary query or result root outlives this native call. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed conversion or wrapper append never reports a complete list. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns the completed element list. */
	return 0;
}

/*
 * Reports the descendant elements that have every class of a
 * space-separated list, in tree order (getElementsByClassName).
 */
int
bind_get_elements_by_class_name(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *root;
	struct dom_node *walk;
	struct vm_string *classes;
	struct vm_string *name;
	struct vm_object *names;
	struct vm_object *array;
	struct vm_cell *roots[4];
	uint32_t index;
	unsigned slot;
	unsigned registered;
	int has;
	int status;

	/* The root, the class names, and an empty list. */
	window = bind_window_of(realm);
	status = bind_this_node(realm, this_value, &root);
	if (status != 0)
		return status;

	/* Retain the fresh query string, token list and unpublished output. */
	roots[0] = NULL;
	if (realm->managed)
		roots[0] = &realm->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	for (slot = 0; slot < 4U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only slots successfully registered. */
		registered++;
	}

	/* Tokenize the original string before walking any descendant. */
	status = bind_to_string(realm, js_argument(args, count, 0), &classes);
	if (status != 0)
		goto cleanup;
	roots[1] = &classes->cell;
	status = bind_array_create(realm, &names);
	if (status != 0)
		goto cleanup;
	roots[2] = &names->cell;
	status = bind_split_classes(realm, classes, names);
	if (status != 0)
		goto cleanup;
	status = bind_array_create(realm, &array);
	if (status != 0)
		goto cleanup;
	roots[3] = &array->cell;

	/* No class names match nothing. */
	if (names->length == 0) {
		*result = vm_value_cell(array);
		status = 0;
		goto cleanup;
	}

	/* Each descendant element that has every class. */
	for (walk = bind_following(root, root); walk != NULL; walk = bind_following(walk, root)) {
		if (walk->type != DOM_ELEMENT)
			continue;

		/* The element must have each name. */
		has = 1;
		for (index = 0; has && index < names->length; index++) {
			name = (struct vm_string *)vm_value_as_cell(names->elements[index]);
			has = bind_element_has_class((const struct dom_element *)walk, name);
		}

		/* An element with all of them is listed. */
		if (!has)
			continue;
		status = bind_array_push_node(window, array, walk);
		if (status != 0)
			goto cleanup;
	}

	/* Succeeded: the list is reported. */
	*result = vm_value_cell(array);
	status = 0;

cleanup:
	/* Return no root that points into the native query frame. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed token split or descendant wrapping cannot report a full list. */
	if (status != 0)
		return status;

	/* Succeeded: the caller owns the completed class match list. */
	return 0;
}

/*
 * Adds the atoms of the space-separated class names in a string to an
 * array (getElementsByClassName's names, and classList's tokens).
 */
int
bind_split_classes(
	struct vm_realm *realm,
	const struct vm_string *classes,
	struct vm_object *names)
{
	struct vm_string *atom;
	struct vm_cell *roots[2];
	struct wb_units units;
	size_t start;
	size_t index;
	uint16_t unit;
	unsigned slot;
	unsigned registered;
	int space;
	int units_ready;
	int status;

	/* Keep the destination and each new atom alive until its property owns it. */
	roots[0] = &names->cell;
	roots[1] = NULL;
	registered = 0;
	units_ready = 0;
	for (slot = 0; slot < 2U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;

		/* Only acquired roots are released by the shared exit. */
		registered++;
	}

	/* The string's units. */
	wb_units_init(&units);
	units_ready = 1;
	status = vm_string_append_units(classes, &units);
	if (status != 0)
		goto cleanup;

	/* Cuts the units at ASCII whitespace; each piece is a name. */
	start = 0;
	for (index = 0; index <= units.length; index++) {
		/* The end of the string ends the last piece. */
		space = 1;
		if (index < units.length) {
			unit = units.data[index];
			space = 0;
			if (unit == 0x20U || unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU)
				space = 1;
		}

		/* A character of a name goes on to the next. */
		if (!space)
			continue;

		/* A piece between two spaces is a name. */
		if (index > start) {
			atom = vm_atom_from_units(realm->heap, &units.data[start], index - start);
			if (atom == NULL) {
				status = ENOMEM;
				goto cleanup;
			}

			/* The new token remains owned until the destination array holds it. */
			roots[1] = &atom->cell;

			/* The name at the array's end. */
			status = vm_object_define(realm->heap, names, vm_value_int32((int32_t)names->length), vm_value_cell(atom), VM_PROPERTY_DEFAULT);
			if (status != 0)
				goto cleanup;
		}

		/* The next name starts after the space. */
		start = index + 1U;
	}

	/* The copied units are no longer needed after splitting. */
	status = 0;

cleanup:
	/* Native units exist only after their buffer initialization completed. */
	if (units_ready)
		wb_units_release(&units);

	/* Release only the native registrations acquired by this split. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* An allocation or property write failure leaves the list incomplete. */
	if (status != 0)
		return status;

	/* Succeeded: the names are in the array. */
	return 0;
}

/* Captures one node or its current children with stable independent native roots. */
static int
mixin_snapshot_capture(
	struct dom_node *node,
	int children,
	struct mixin_node_snapshot *snapshot)
{
	struct dom_node *child;
	size_t count;
	size_t index;
	int status;

	/* A single node is the default replacement sequence. */
	count = 1;
	if (children) {
		/* Count the existing child edges before allocating or registering any stable slot. */
		count = 0;
		for (child = node->first_child;
		     child != NULL;
		     child = child->next)
			count++;
	}

	/* An empty sequence has no native allocation or registration obligations. */
	snapshot->heap = node->document->heap;
	if (count == 0)
		return 0;

	/* The node-pointer view is borrowed by mutation-record construction. */
	snapshot->nodes = calloc(count, sizeof(*snapshot->nodes));
	if (snapshot->nodes == NULL)
		return ENOMEM;

	/* Root slots remain separate objects instead of aliasing differently typed node pointers. */
	snapshot->roots = calloc(count, sizeof(*snapshot->roots));
	if (snapshot->roots == NULL) {
		mixin_snapshot_release(snapshot);
		return ENOMEM;
	}

	/* A children snapshot preserves tree order; a node snapshot has exactly one entry. */
	if (children) {
		/* Copy the still-connected children before removal can destroy their sibling chain. */
		index = 0;
		for (child = node->first_child;
		     child != NULL;
		     child = child->next) {
			snapshot->nodes[index] = child;
			snapshot->roots[index] = &child->cell;
			index++;
		}
	} else {
		snapshot->nodes[0] = node;
		snapshot->roots[0] = &node->cell;
	}

	/* Register every stable slot before the caller can detach even the first node. */
	snapshot->count = count;
	for (index = 0; index < count; index++) {
		status = vm_heap_add_root(snapshot->heap, &snapshot->roots[index]);
		if (status != 0) {
			mixin_snapshot_release(snapshot);
			return status;
		}

		/* A failed later registration must release exactly this successful prefix. */
		snapshot->registered++;
	}

	/* Succeeded: every node remains independently owned until snapshot release. */
	return 0;
}

/* Releases a replacement snapshot, including partially initialized allocation failures. */
static void
mixin_snapshot_release(
	struct mixin_node_snapshot *snapshot)
{
	/* Every registered slot leaves the heap before either native array can disappear. */
	while (snapshot->registered != 0) {
		snapshot->registered--;
		vm_heap_remove_root(snapshot->heap, &snapshot->roots[snapshot->registered]);
	}

	/* Both native arrays have independent allocation lifetimes. */
	free(snapshot->roots);
	free(snapshot->nodes);

	/* A released snapshot is safe to release again along the caller's common unwind path. */
	snapshot->roots = NULL;
	snapshot->nodes = NULL;
	snapshot->count = 0;

	/* Succeeded: no snapshot storage or registered ownership remains. */
	return;
}

/* Adopts a replacement node and records its original parent removal before target replacement. */
static int
mixin_adopt_replacement(
	struct vm_realm *realm,
	struct dom_node *parent,
	struct dom_node *node)
{
	struct bind_window *source_window;
	struct dom_node *source;
	struct dom_node *previous;
	struct dom_node *next;
	struct vm_heap *heap;
	struct vm_cell *roots[4];
	unsigned registered;
	unsigned index;
	int status;

	/* Capture source ownership and boundaries before checked adoption changes any Document. */
	source = node->parent;
	previous = node->previous;
	next = node->next;
	source_window = bind_window_of(realm);
	if (node->document->view != NULL)
		source_window = node->document->view;

	/* Detached nodes need no source record, but their Document ownership still changes. */
	if (source == NULL) {
		status = dom_adopt(parent->document, node);
		if (status != 0)
			return status;

		/* Succeeded: the detached replacement belongs to the destination Document. */
		return 0;
	}

	/* Root the old parent, moved node and both boundaries through record construction. */
	heap = node->document->heap;
	memset(roots, 0, sizeof(roots));
	roots[0] = &source->cell;
	roots[1] = &node->cell;
	if (previous != NULL)
		roots[2] = &previous->cell;

	/* The following boundary can exist even when the moved node was the first child. */
	if (next != NULL)
		roots[3] = &next->cell;

	/* Publish the complete source ownership before adoption can detach its participants. */
	registered = 0;
	for (index = 0; index < 4U; index++) {
		status = vm_heap_add_root(heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Each successful registration has one matching unwind obligation. */
		registered++;
	}

	/* Native adoption preserves removal hooks, ranges and iterators before changing Documents. */
	status = dom_adopt(parent->document, node);
	if (status != 0)
		goto cleanup;

	/* Source observers receive this removal before the later target replacement record. */
	status = bind_environment_children_mutation(source_window, source, NULL, 0, &node, 1, previous, next);
	if (status != 0)
		goto cleanup;

cleanup:
	/* Source ownership ends only after the record owns every boundary and removed node. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Preserve adoption or record-publication failure after releasing native roots. */
	if (status != 0)
		return status;

	/* Succeeded: adoption and any original-parent removal record are complete. */
	return 0;
}

/* Moves a validated replacement without emitting individual destination insertion records. */
static int
mixin_move_replacement(
	struct vm_realm *realm,
	struct dom_node *parent,
	struct dom_node *node)
{
	struct bind_window *window;
	struct bind_window *source_window;
	struct dom_node *child;
	struct mixin_node_snapshot children = {0};
	size_t index;
	int status;

	/* Conversion fragments use ordinary insertion constraints before any source mutation. */
	status = bind_validate_insert(realm, parent, node, NULL);
	if (status != 0)
		return status;

	/* Source fragment observers stay associated with their original hosting window. */
	source_window = bind_window_of(realm);
	if (node->document->view != NULL)
		source_window = node->document->view;

	/* A borrowed method resolves insertion callbacks through the actual destination Document. */
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;

	/* Adoption records the node's own source-parent removal before insertion. */
	status = mixin_adopt_replacement(realm, parent, node);
	if (status != 0)
		return status;

	/* A fragment contributes its children, while any other argument contributes itself. */
	if (node->type == DOM_DOCUMENT_FRAGMENT) {
		status = mixin_snapshot_capture(node, 1, &children);
	} else {
		status = mixin_snapshot_capture(node, 0, &children);
	}

	/* Snapshot failure cannot discard children from an argument fragment. */
	if (status != 0)
		return status;

	/* Empty a fragment under suppressed per-child observer records. */
	if (node->type == DOM_DOCUMENT_FRAGMENT) {
		/* Preserve native removal hooks while the snapshot retains all detached children. */
		for (index = 0; index < children.count; index++)
			dom_remove(children.nodes[index]);

		/* The fragment's complete child removal is one observable record. */
		if (children.count != 0) {
			status = bind_environment_children_mutation(source_window, node, NULL, 0, children.nodes, children.count, NULL, NULL);
			if (status != 0)
				goto cleanup;
		}
	}

	/* Insert each already-adopted child with the existing host notification contract. */
	for (index = 0; index < children.count; index++) {
		child = children.nodes[index];
		dom_insert_before(parent, child, NULL);

		/* Host-side style, script and embedded-content processing still sees every insertion. */
		if (window->host.node_inserted != NULL) {
			status = window->host.node_inserted(window->host.context, child);
			if (status != 0)
				goto cleanup;
		}
	}

	/* The caller owns the eventual single target record for the complete replacement. */
	status = 0;

cleanup:
	/* Inserted nodes have tree edges, and failed partial insertion retains no native snapshots. */
	mixin_snapshot_release(&children);

	/* Propagate the first host or source-record failure. */
	if (status != 0)
		return status;

	/* Succeeded: all incoming nodes entered the target without per-node target records. */
	return 0;
}

/* Inserts append's or prepend's arguments before reference: nodes as they are, other values as text. */
static int
mixin_insert_values(
	struct vm_realm *realm,
	struct dom_node *parent,
	const vm_value *args,
	unsigned count,
	struct dom_node *reference)
{
	struct dom_node *node;
	struct vm_cell *node_root;
	struct vm_string *string;
	struct wb_units units;
	unsigned index;
	int status;

	/* An unpublished text node survives insertion callbacks until the parent owns it. */
	node_root = NULL;
	status = vm_heap_add_root(realm->heap, &node_root);
	if (status != 0)
		return status;

	/* Each argument in order. */
	for (index = 0; index < count; index++) {
		/* A node is inserted as it is. */
		node = bind_node_of(args[index]);

		/* Any other value becomes a text node of its string. */
		if (node == NULL) {
			status = bind_to_string(realm, args[index], &string);
			if (status != 0)
				goto cleanup;
			wb_units_init(&units);
			status = vm_string_append_units(string, &units);
			if (status == 0) {
				node = dom_text_create(parent->document, units.data, units.length);
				if (node == NULL)
					status = ENOMEM;
				else
					node_root = &node->cell;
			}

			/* The characters are in the node now. */
			wb_units_release(&units);
			if (status != 0)
				goto cleanup;
		}

		/* Before the reference (a reference that is being moved stays the place). */
		node_root = &node->cell;
		if (reference == node)
			reference = node->next;
		status = bind_insert(realm, parent, node, reference);
		if (status != 0)
			goto cleanup;
		node_root = NULL;
	}

	/* Every inserted node now has a durable parent edge. */
	status = 0;

cleanup:
	/* The one native slot outlives no insertion attempt. */
	vm_heap_remove_root(realm->heap, &node_root);

	/* Failed conversion or insertion reports the error after cleanup. */
	if (status != 0)
		return status;

	/* Succeeded: every argument is inserted. */
	return 0;
}

/* Reports an element that an attribute leads to, or null. */
static int
mixin_element_link(
	struct vm_realm *realm,
	struct dom_node *node,
	vm_value *result)
{
	struct bind_window *window;
	int status;

	/* The node's object, or null. */
	window = bind_window_of(realm);
	status = bind_wrap_or_null(window, node, result);
	if (status != 0)
		return status;

	/* Succeeded: the element is reported. */
	return 0;
}

/* Tells whether a qualified name matches using the owning Document's case policy. */
static int
mixin_name_matches(
	const struct dom_element *element,
	const struct vm_string *name,
	const struct vm_string *lower)
{
	const struct vm_string *requested;
	size_t index;
	size_t offset;
	uint16_t actual;
	uint16_t expected;
	int star;

	/* A wildcard includes every descendant Element irrespective of namespace. */
	star = vm_string_equal_ascii(name, "*");
	if (star)
		return 1;

	/* Only HTML elements within HTML Documents use the folded query spelling. */
	requested = name;
	if (element->ns == DOM_NS_HTML && element->node.document->content == DOM_CONTENT_HTML)
		requested = lower;

	/* The qualified name includes an optional prefix and its colon separator. */
	offset = 0;
	if (element->prefix != NULL) {
		offset = element->prefix->length + 1U;

		/* A different qualified-name length cannot match this element. */
		if (requested->length != offset + element->local_name->length)
			return 0;

		/* Prefixes participate in exact comparison rather than disappearing from queries. */
		for (index = 0; index < element->prefix->length; index++) {
			actual = vm_string_at(element->prefix, index);
			expected = vm_string_at(requested, index);
			if (actual != expected)
				return 0;
		}

		/* A prefix always occupies the slice before one literal namespace separator. */
		expected = vm_string_at(requested, offset - 1U);
		if (expected != ':')
			return 0;
	}

	/* Unprefixed and prefixed local slices must have exactly the same remaining length. */
	if (requested->length != offset + element->local_name->length)
		return 0;

	/* Compares UTF-16 code units without allocation or repeating user conversion. */
	for (index = 0; index < element->local_name->length; index++) {
		actual = vm_string_at(element->local_name, index);
		expected = vm_string_at(requested, offset + index);
		if (actual != expected)
			return 0;
	}

	/* Succeeded: every qualified-name code unit matches the selected spelling. */
	return 1;
}
