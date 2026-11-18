/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NodeIterator traverses live preorder links. Its weak pre-removal subscription
 * repairs both the permanent reference and the in-flight candidate. The original
 * filter argument remains traced even when deletion changes the candidate.
 */

#include "bind/internal.h"

#include <errno.h>

/* A position records which side of a live preorder node the cursor occupies. */
struct iterator_pointer {
	struct dom_node *node;
	int before;
};

/* Reachability retains node/filter graphs; a weak C token never retains this cell. */
struct iterator_state {
	struct vm_cell cell;
	struct dom_node *root;
	struct iterator_pointer reference;
	struct iterator_pointer candidate;
	struct dom_node *pending;
	struct dom_removal_subscription *subscription;
	vm_value filter;
	uint32_t mask;
	int active;
};

static void iterator_trace(struct vm_heap *heap, struct vm_cell *cell);
static void iterator_finalize(struct vm_heap *heap, struct vm_cell *cell);
static int iterator_root(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_reference(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_before(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_mask(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_callback(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_previous(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int iterator_detach(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The native state owns its weak token and traces every in-flight Node edge. */
static const struct vm_cell_type iterator_type = {"node-iterator", iterator_trace, iterator_finalize};

/* Every cursor attribute is readonly, including the permanent reference position. */
static const struct bind_attribute iterator_attributes[] = {
    {"root", iterator_root, NULL},
    {"referenceNode", iterator_reference, NULL},
    {"pointerBeforeReferenceNode", iterator_before, NULL},
    {"whatToShow", iterator_mask, NULL},
    {"filter", iterator_callback, NULL},
    {NULL, NULL, NULL}};

/* Legacy detach is deliberately harmless while traversal remains fully usable. */
static const struct bind_operation iterator_operations[] = {
    {"nextNode", 0, iterator_next},
    {"previousNode", 0, iterator_previous},
    {"detach", 0, iterator_detach},
    {NULL, 0, NULL}};

/* Documents construct native iterators without exposing a callable constructor. */
const struct bind_interface bind_node_iterator_interface = {
    "NodeIterator", BIND_NO_PARENT, 0, NULL, iterator_attributes, iterator_operations, NULL};

static void iterator_removed(void *context, struct dom_node *removed);
static void iterator_adjust(struct iterator_state *state, struct iterator_pointer *pointer, struct dom_node *removed);
static struct dom_node *iterator_following(struct dom_node *node, struct dom_node *root, int descendants);
static struct dom_node *iterator_preceding(struct dom_node *node, struct dom_node *root);
static int iterator_this(struct vm_realm *realm, vm_value this_value, struct iterator_state **state);
static int iterator_filter(struct vm_realm *realm, struct iterator_state *state, struct dom_node *node, uint32_t *accepted);
static int iterator_move(struct vm_realm *realm, vm_value this_value, int forward, vm_value *result);

/*
 * Creates a NodeIterator with a weak subscription to its root's actual Document.
 */
int
bind_create_node_iterator(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *receiver;
	struct dom_node *root;
	struct dom_document *document;
	struct bind_window *window;
	struct vm_object *prototype;
	struct vm_object *wrapper;
	struct iterator_state *state;
	vm_value filter;
	vm_value snapshot;
	vm_value argument;
	uint32_t mask;
	int valid;
	int status;

	/* Receiver Document branding precedes every potentially user-defined conversion. */
	status = bind_this_node(realm, this_value, &receiver);
	if (status != 0)
		return status;

	/* Another native Node kind cannot act as a Document factory receiver. */
	if (receiver->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the incompatible operation is reported as a VM exception. */
		return 0;
	}

	/* The required root accepts any actual Node, including another Document's node. */
	argument = js_argument(args, count, 0);
	status = bind_argument_node(realm, argument, &root);
	if (status != 0)
		return status;

	/* Unsigned masks convert once, with undefined retaining the all-node default. */
	mask = UINT32_MAX;
	if (count > 1 && args[1] != VM_VALUE_UNDEFINED) {
		status = vm_to_uint32(realm, args[1], &mask);
		if (status != 0)
			return status;
	}

	/* Object callbacks stay uninspected until actual filtering, and undefined means null. */
	filter = js_argument(args, count, 2);
	if (filter == VM_VALUE_UNDEFINED) {
		filter = VM_VALUE_NULL;
	} else if (filter != VM_VALUE_NULL) {
		valid = vm_value_is_object(filter);
		if (!valid) {
			status = vm_throw_type_error(realm, "The filter is not an object.");
			if (status != 0)
				return status;

			/* Succeeded: the incompatible operation is reported as a VM exception. */
			return 0;
		}
	}

	/* XML Documents retain their relevant realm through the private prototype snapshot. */
	document = (struct dom_document *)receiver;
	if (document->binding_prototypes != NULL) {
		status = vm_object_get(document->binding_prototypes, vm_value_int32(BIND_NODE_ITERATOR), &snapshot);
		if (status != 0)
			return status;

		/* Uses the receiver's retained relevant-realm prototype. */
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Borrowed methods use the receiver Document owner rather than the root owner. */
		window = document->view;
		if (window == NULL)
			window = bind_window_of(realm);
		prototype = window->prototypes[BIND_NODE_ITERATOR];
	}

	/* Allocates a fully traced native graph before any weak token becomes visible. */
	state = vm_heap_alloc(document->heap, &iterator_type, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Initial traversal starts before the root and invokes no callback at construction. */
	state->root = root;
	state->reference.node = root;
	state->reference.before = 1;
	state->filter = filter;
	state->mask = mask;

	/* The relevant prototype and native state belong to the visible wrapper graph. */
	wrapper = vm_object_create(document->heap, prototype);
	if (wrapper == NULL)
		return ENOMEM;

	/* Publishes native state only through its correctly branded wrapper. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);

	/* Root ownership controls removal notifications even for cross-Document factories. */
	status = dom_removal_subscribe(root->document, state, iterator_removed, &state->subscription);
	if (status != 0)
		return status;

	/* This optional weak association follows the actual root through checked Document adoption. */
	dom_removal_set_root(state->subscription, root);
	*result = vm_value_cell(wrapper);

	/* Succeeded: the collectible iterator owns exactly one weak removal token. */
	return 0;
}

/* Traces permanent and in-flight nodes independently of the weak registration. */
static void
iterator_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct iterator_state *state;

	/* Root and permanent reference retain their real Document and managed context. */
	state = (struct iterator_state *)cell;
	vm_heap_mark(heap, &state->root->cell);
	vm_heap_mark(heap, &state->reference.node->cell);
	vm_heap_mark_word(heap, state->filter);

	/* A removal callback may adjust the candidate before user filtering finishes. */
	if (state->candidate.node != NULL)
		vm_heap_mark(heap, &state->candidate.node->cell);

	/* The original callback argument remains independent of its adjusted position. */
	if (state->pending != NULL)
		vm_heap_mark(heap, &state->pending->cell);

	/* Succeeded: all permanent and in-flight native edges are marked. */
	return;
}

/* Unsubscribes using only separately allocated tokens during any heap teardown order. */
static void
iterator_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct iterator_state *state;

	UNUSED_PARAMETER(heap);

	/* Neither Document nor neighbouring iterator GC cells are read by token cleanup. */
	state = (struct iterator_state *)cell;
	dom_removal_unsubscribe(state->subscription);

	/* Succeeded: no weak removal callback can reach this reclaimed native state. */
	return;
}

/* Repairs both positions before original DOM links disappear, without running script. */
static void
iterator_removed(
	void *context,
	struct dom_node *removed)
{
	struct iterator_state *state;

	/* The permanent reference always reflects every relevant tree removal. */
	state = context;
	iterator_adjust(state, &state->reference, removed);

	/* An active candidate has a separate position and receives the same repair. */
	if (state->candidate.node != NULL)
		iterator_adjust(state, &state->candidate, removed);

	/* Succeeded: iterator positions refer to surviving nodes inside their actual root. */
	return;
}

/* Adjusts a cursor around a removed subtree using its original live relatives. */
static void
iterator_adjust(
	struct iterator_state *state,
	struct iterator_pointer *pointer,
	struct dom_node *removed)
{
	struct dom_node *node;
	int inside;

	/* Removals that do not contain this position cannot affect its reference. */
	inside = dom_is_inclusive_ancestor(removed, pointer->node);
	if (!inside)
		return;

	/* Removing the root or its ancestor leaves the iterator's own tree intact. */
	inside = dom_is_inclusive_ancestor(removed, state->root);
	if (inside)
		return;

	/* A before-position prefers the first surviving following node inside the root. */
	if (pointer->before) {
		node = iterator_following(removed, state->root, 0);
		if (node != NULL) {
			pointer->node = node;
			return;
		}
	}

	/* Otherwise the new position follows the preceding subtree or the parent itself. */
	node = removed->previous;
	if (node == NULL) {
		node = removed->parent;
	} else {
		/* The last preorder descendant precedes the removed subtree immediately. */
		while (node->last_child != NULL)
			node = node->last_child;
	}

	/* Both fallback paths represent the cursor after its new surviving node. */
	pointer->node = node;
	pointer->before = 0;

	/* Succeeded: the repaired position remains in the iterator collection. */
	return;
}

/* Finds the next live preorder node, optionally omitting a removed subtree's children. */
static struct dom_node *
iterator_following(
	struct dom_node *node,
	struct dom_node *root,
	int descendants)
{
	/* Ordinary forward traversal visits children before siblings. */
	if (descendants && node->first_child != NULL)
		return node->first_child;

	/* An exhausted branch climbs until it finds a sibling below the root boundary. */
	while (node != NULL && node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}

	/* Succeeded: no following node remains inside the actual root. */
	return NULL;
}

/* Finds the previous live preorder node while retaining the root boundary. */
static struct dom_node *
iterator_preceding(
	struct dom_node *node,
	struct dom_node *root)
{
	/* The actual root has no predecessor in this iterator collection. */
	if (node == root)
		return NULL;

	/* Without a preceding sibling, the parent immediately precedes this node. */
	if (node->previous == NULL)
		return node->parent;

	/* Reverse preorder starts at the deepest final descendant of the preceding sibling. */
	node = node->previous;
	while (node->last_child != NULL)
		node = node->last_child;

	/* Succeeded: the immediately preceding live node is available. */
	return node;
}

/* Requires the private iterator native brand instead of prototype inheritance. */
static int
iterator_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct iterator_state **state)
{
	struct vm_object *wrapper;
	struct vm_cell *cell;
	int valid;
	int status;

	/* Primitive values cannot carry a native platform state. */
	valid = vm_value_is_object(this_value);
	if (!valid) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the incompatible operation is reported as a VM exception. */
		return 0;
	}

	/* Fake prototype inheritors and other object kinds have no iterator brand. */
	wrapper = (struct vm_object *)vm_value_as_cell(this_value);
	valid = vm_value_is_cell(wrapper->internal);
	if (wrapper->kind != VM_KIND_PLATFORM || !valid) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the incompatible operation is reported as a VM exception. */
		return 0;
	}

	/* Other native platform objects cannot substitute their own internal cell. */
	cell = vm_value_as_cell(wrapper->internal);
	if (cell->type != &iterator_type) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the incompatible operation is reported as a VM exception. */
		return 0;
	}

	/* Succeeded: this wrapper owns the genuine native iterator state. */
	*state = (struct iterator_state *)cell;
	return 0;
}

/* Applies the mask and callback while retaining the original potentially detached node. */
static int
iterator_filter(
	struct vm_realm *realm,
	struct iterator_state *state,
	struct dom_node *node,
	uint32_t *accepted)
{
	uint32_t bit;
	int status;

	/* Hidden node kinds are skipped without invoking user callbacks. */
	bit = 1U << (node->type - 1U);
	if ((state->mask & bit) == 0U) {
		*accepted = 3U;
		return 0;
	}

	/* Selected kinds without a callback are accepted directly. */
	if (state->filter == VM_VALUE_NULL) {
		*accepted = 1U;
		return 0;
	}

	/* The active guard covers operation lookup, invocation and unsigned-short conversion. */
	state->active = 1;
	status = bind_filter_callback(realm, state->filter, node, accepted);
	if (status != 0) {
		state->active = 0;
		return status;
	}

	/* Unwinds the guard after a successful callback and conversion. */
	state->active = 0;

	/* Succeeded: the shared callback operation produced a converted decision. */
	return 0;
}

/* Traverses one direction with deletion-adjusted candidate state and exact exception cleanup. */
static int
iterator_move(
	struct vm_realm *realm,
	vm_value this_value,
	int forward,
	vm_value *result)
{
	struct iterator_state *state;
	struct dom_node *node;
	uint32_t accepted;
	int status;

	/* A native receiver is required before touching its cursor protocol. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Nested traversal cannot overwrite an outer operation's candidate and pending edge. */
	if (state->active) {
		status = bind_throw_dom(realm, "InvalidStateError", "The iterator filter is active.");
		if (status != 0)
			return status;

		/* Succeeded: the incompatible operation is reported as a VM exception. */
		return 0;
	}

	/* Each attempt begins with a distinct copy of the permanent reference position. */
	state->candidate = state->reference;
	while (state->candidate.node != NULL) {
		node = state->candidate.node;
		if (forward) {
			/* A before-position visits this node; an after-position advances in live preorder. */
			if (!state->candidate.before)
				node = iterator_following(node, state->root, 1);
			state->candidate.before = 0;
		} else {
			/* Reverse traversal visits this after-position or its preceding live node. */
			if (state->candidate.before)
				node = iterator_preceding(node, state->root);
			state->candidate.before = 1;
		}

		/* Exhaustion preserves the permanent reference including its original pointer side. */
		if (node == NULL)
			break;

		/* The original argument remains traced while pre-removal repair can replace candidate.node. */
		state->candidate.node = node;
		state->pending = node;
		status = iterator_filter(realm, state, node, &accepted);
		if (status != 0) {
			state->candidate.node = NULL;
			state->pending = NULL;
			return status;
		}

		/* Wrapping the accepted original node happens before publishing adjusted reference state. */
		if (accepted == 1U) {
			status = bind_wrap(bind_window_of(realm), node, result);
			if (status != 0) {
				state->candidate.node = NULL;
				state->pending = NULL;
				return status;
			}

			/* Publishes the repaired reference only after wrapping succeeds. */
			state->reference = state->candidate;
			state->candidate.node = NULL;
			state->pending = NULL;

			/* Succeeded: a possibly detached original node and its repaired cursor are available. */
			return 0;
		}

		/* Rejected and skipped nodes both leave their children visible in NodeIterator. */
		state->pending = NULL;
	}

	/* No accepted result leaves any stale in-flight graph or permanent cursor change. */
	state->candidate.node = NULL;
	state->pending = NULL;
	*result = VM_VALUE_NULL;

	/* Succeeded: traversal exhausted the actual root tree in this direction. */
	return 0;
}

/* Reports the immutable iterator root. */
static int
iterator_root(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native branding prevents prototype-only objects from exposing cursor state. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The attribute exposes its current native state without applying any filter. */
	status = bind_wrap(bind_window_of(realm), state->root, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual readonly iterator component is available. */
	return 0;
}

/* Reports the live permanent reference node. */
static int
iterator_reference(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native branding prevents prototype-only objects from exposing cursor state. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The attribute exposes its current native state without applying any filter. */
	status = bind_wrap(bind_window_of(realm), state->reference.node, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual readonly iterator component is available. */
	return 0;
}

/* Reports which side of the reference the permanent cursor occupies. */
static int
iterator_before(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native branding prevents prototype-only objects from exposing cursor state. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The attribute exposes its current native state without applying any filter. */
	*result = VM_VALUE_FALSE;
	if (state->reference.before)
		*result = VM_VALUE_TRUE;

	/* Succeeded: the actual readonly iterator component is available. */
	return 0;
}

/* Reports the full unsigned node-kind mask. */
static int
iterator_mask(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native branding prevents prototype-only objects from exposing cursor state. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The attribute exposes its current native state without applying any filter. */
	*result = vm_value_number((double)state->mask);

	/* Succeeded: the actual readonly iterator component is available. */
	return 0;
}

/* Reports the original callback-interface object or null. */
static int
iterator_callback(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native branding prevents prototype-only objects from exposing cursor state. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The attribute exposes its current native state without applying any filter. */
	*result = state->filter;

	/* Succeeded: the actual readonly iterator component is available. */
	return 0;
}

/* Finds the next accepted node in the selected live-tree direction. */
static int
iterator_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared directional algorithm retains exact cursor and exception semantics. */
	status = iterator_move(realm, this_value, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual accepted node or null is available. */
	return 0;
}

/* Finds the next accepted node in the selected live-tree direction. */
static int
iterator_previous(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared directional algorithm retains exact cursor and exception semantics. */
	status = iterator_move(realm, this_value, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual accepted node or null is available. */
	return 0;
}

/* Preserves the iterator and its removal subscription after a legacy detach call. */
static int
iterator_detach(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct iterator_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Legacy compatibility still requires a genuine iterator receiver. */
	status = iterator_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Detach has no effect on traversal state, filtering or deletion repair. */
	*result = VM_VALUE_UNDEFINED;

	/* Succeeded: the legacy operation leaves the entire iterator usable. */
	return 0;
}
