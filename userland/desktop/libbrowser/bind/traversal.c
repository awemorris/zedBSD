/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TreeWalker filters actual DOM links rather than a saved collection. Current
 * nodes may leave and re-enter the root tree. Candidates remain traced while
 * a filter mutates their links or triggers collection.
 */

#include "bind/internal.h"

#include <errno.h>

/* Navigation names select the distinct DOM traversal algorithms. */
enum walker_direction {
	WALKER_PARENT,
	WALKER_FIRST,
	WALKER_LAST,
	WALKER_NEXT_SIBLING,
	WALKER_PREVIOUS_SIBLING,
	WALKER_NEXT,
	WALKER_PREVIOUS
};

/* One collectible walker retains its independent root, current node and filter. */
struct walker_state {
	struct vm_cell cell;
	struct dom_node *root;
	struct dom_node *current;
	struct dom_node *pending;
	vm_value filter;
	uint32_t mask;
	int active;
};

/* One callback-interface constant retains its unsigned DOM value. */
struct traversal_constant {
	const char *name;
	uint32_t number;
};

static void walker_trace(struct vm_heap *heap, struct vm_cell *cell);
static int walker_this(struct vm_realm *realm, vm_value this_value, struct walker_state **state);
static int walker_filter(struct vm_realm *realm, struct walker_state *state, struct dom_node *node, uint32_t *accepted);
static int walker_root(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_mask(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_callback(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_current(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_current_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_parent(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_first(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_last(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_next_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_previous_sibling(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_next(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_previous(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int walker_move(struct vm_realm *realm, vm_value this_value, enum walker_direction direction, vm_value *result);
static int walker_ancestor(struct vm_realm *realm, struct walker_state *state, struct dom_node **answer);
static int walker_children(struct vm_realm *realm, struct walker_state *state, int forward, struct dom_node **answer);
static int walker_siblings(struct vm_realm *realm, struct walker_state *state, int forward, struct dom_node **answer);
static int walker_following(struct vm_realm *realm, struct walker_state *state, struct dom_node **answer);
static int walker_preceding(struct vm_realm *realm, struct walker_state *state, struct dom_node **answer);

/* The private native brand traces callbacks and nodes while its wrapper is reachable. */
static const struct vm_cell_type walker_type = { "tree-walker", walker_trace, NULL };

/* Attributes expose live walker state, with only currentNode writable. */
static const struct bind_attribute walker_attributes[] = {
	{ "root", walker_root, NULL },
	{ "whatToShow", walker_mask, NULL },
	{ "filter", walker_callback, NULL },
	{ "currentNode", walker_current, walker_current_set },
	{ NULL, NULL, NULL }
};

/* Navigation methods commit only accepted results from the actual live tree. */
static const struct bind_operation walker_operations[] = {
	{ "parentNode", 0, walker_parent },
	{ "firstChild", 0, walker_first },
	{ "lastChild", 0, walker_last },
	{ "nextSibling", 0, walker_next_sibling },
	{ "previousSibling", 0, walker_previous_sibling },
	{ "nextNode", 0, walker_next },
	{ "previousNode", 0, walker_previous },
	{ NULL, 0, NULL }
};

/* Callback-interface constants belong to an ordinary namespace, not a constructor. */
static const struct traversal_constant traversal_constants[] = {
	{ "FILTER_ACCEPT", 1U },
	{ "FILTER_REJECT", 2U },
	{ "FILTER_SKIP", 3U },
	{ "SHOW_ALL", UINT32_MAX },
	{ "SHOW_ELEMENT", 1U },
	{ "SHOW_ATTRIBUTE", 2U },
	{ "SHOW_TEXT", 4U },
	{ "SHOW_CDATA_SECTION", 8U },
	{ "SHOW_ENTITY_REFERENCE", 16U },
	{ "SHOW_ENTITY", 32U },
	{ "SHOW_PROCESSING_INSTRUCTION", 64U },
	{ "SHOW_COMMENT", 128U },
	{ "SHOW_DOCUMENT", 256U },
	{ "SHOW_DOCUMENT_TYPE", 512U },
	{ "SHOW_DOCUMENT_FRAGMENT", 1024U },
	{ "SHOW_NOTATION", 2048U },
	{ NULL, 0U }
};

/* TreeWalker instances are created by a Document without a public constructor. */
const struct bind_interface bind_tree_walker_interface = {
	"TreeWalker", BIND_NO_PARENT, 0, NULL, walker_attributes, walker_operations, NULL
};

/*
 * Installs unsigned NodeFilter constants on the realm's callback-interface namespace.
 */
int
bind_traversal_install(
	struct bind_window *window)
{
	struct vm_realm *realm;
	struct vm_object *namespace;
	struct vm_cell *root;
	const struct traversal_constant *entry;
	vm_value number;
	int status;

	/* The global object owns the completed ordinary constant namespace. */
	realm = window->realm;
	namespace = vm_object_create(realm->heap, realm->object_prototype);
	if (namespace == NULL)
		return ENOMEM;
	root = &namespace->cell;
	status = vm_heap_add_root(realm->heap, &root);
	if (status != 0)
		return status;

	/* Numbers retain all unsigned bits instead of becoming negative int32 constants. */
	for (entry = traversal_constants; entry->name != NULL; entry++) {
		number = vm_value_number((double)entry->number);
		status = js_builtin_value(realm, namespace, entry->name, number, VM_PROPERTY_ENUMERABLE);
		if (status != 0)
			goto cleanup;
	}

	/* Publishes the complete namespace as an ordinary replaceable global binding. */
	status = js_builtin_value(realm, realm->global, "NodeFilter", vm_value_cell(namespace), VM_PROPERTY_DEFAULT);

cleanup:
	/* The published global retains the namespace after temporary ownership ends. */
	vm_heap_remove_root(realm->heap, &root);
	if (status != 0)
		return status;

	/* Succeeded: the realm exposes the unsigned callback-interface constants. */
	return 0;
}

/*
 * Creates a branded TreeWalker using the receiver Document's actual realm prototype.
 */
int
bind_create_tree_walker(
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
	struct walker_state *state;
	struct vm_cell *state_root;
	vm_value filter;
	vm_value snapshot;
	uint32_t mask;
	int valid;
	int status;

	/* Document branding precedes root, mask and filter conversions. */
	status = bind_this_node(realm, this_value, &receiver);
	if (status != 0)
		return status;

	/* Real nodes of another kind cannot stand for the receiver Document. */
	if (receiver->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Missing or primitive roots are rejected by actual Node branding. */
	status = bind_argument_node(realm, js_argument(args, count, 0), &root);
	if (status != 0)
		return status;

	/* Omitted and explicit undefined masks receive the unsigned default. */
	mask = UINT32_MAX;
	if (count > 1 && args[1] != VM_VALUE_UNDEFINED) {
		status = vm_to_uint32(realm, args[1], &mask);
		if (status != 0)
			return status;
	}

	/* Nullable callback-interface conversion preserves functions and callback objects. */
	filter = js_argument(args, count, 2);
	if (filter == VM_VALUE_UNDEFINED) {
		filter = VM_VALUE_NULL;
	} else if (filter != VM_VALUE_NULL) {
		valid = vm_value_is_object(filter);
		if (!valid) {
			status = vm_throw_type_error(realm, "The filter is not an object.");
			return status;
		}
	}

	/* Detached XML Documents preserve their relevant prototype in the private snapshot. */
	document = (struct dom_document *)receiver;
	if (document->binding_prototypes != NULL) {
		status = vm_object_get(document->binding_prototypes, vm_value_int32(BIND_TREE_WALKER), &snapshot);
		if (status != 0)
			return status;
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Borrowed methods resolve the actual Document's ordinary binding owner. */
		window = document->view;
		if (window == NULL)
			window = bind_window_of(realm);
		prototype = window->prototypes[BIND_TREE_WALKER];
	}

	/* Initializes the native graph before allocating its visible wrapper. */
	state = vm_heap_alloc(document->heap, &walker_type, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->root = root;
	state->current = root;
	state->filter = filter;
	state->mask = mask;
	state_root = &state->cell;
	status = vm_heap_add_root(document->heap, &state_root);
	if (status != 0)
		return status;

	/* The wrapper retains both the native state and the relevant realm prototype. */
	wrapper = vm_object_create(document->heap, prototype);
	if (wrapper == NULL) {
		vm_heap_remove_root(document->heap, &state_root);
		return ENOMEM;
	}

	/* The finished wrapper now owns the protected native graph. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);
	*result = vm_value_cell(wrapper);
	vm_heap_remove_root(document->heap, &state_root);

	/* Succeeded: the walker starts at its root without invoking any filter. */
	return 0;
}

/*
 * Invokes a callback-interface filter and converts its unsigned-short decision.
 */
int
bind_filter_callback(
	struct vm_realm *realm,
	vm_value filter,
	struct dom_node *node,
	uint32_t *accepted)
{
	vm_value callback;
	vm_value receiver;
	vm_value wrapped;
	vm_value answer;
	vm_value key;
	struct vm_cell *roots[3];
	unsigned index;
	unsigned registered;
	int callable;
	int value_cell;
	int status;

	/* The wrapped candidate, returned operation and conversion result may all allocate. */
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The callback receives the candidate's owner-realm Node wrapper. */
	status = bind_wrap(bind_window_of(realm), node, &wrapped);
	if (status != 0)
		goto cleanup;
	roots[0] = vm_value_as_cell(wrapped);

	/* Callable callback interfaces invoke the supplied function directly. */
	callback = filter;
	receiver = VM_VALUE_UNDEFINED;
	callable = vm_value_is_callable(callback);
	if (!callable) {
		/* Object filters resolve acceptNode for every invocation, including its getter. */
		key = vm_key_from_ascii(realm->heap, "acceptNode");
		if (key == VM_VALUE_EMPTY) {
			status = ENOMEM;
			goto cleanup;
		}

		/* Reading a user-defined getter may allocate or collect. */
		status = vm_get(realm, filter, key, &callback);
		if (status != 0)
			goto cleanup;

		/* Callback-interface operations must be callable before entering the VM call primitive. */
		callable = vm_value_is_callable(callback);
		if (!callable) {
			status = vm_throw_type_error(realm, "The acceptNode operation is not callable.");
			goto cleanup;
		}

		/* Object callbacks receive their original callback object as this. */
		receiver = filter;
	}

	/* A newly returned callback stays live until the invocation completes. */
	value_cell = vm_value_is_cell(callback);
	if (value_cell)
		roots[1] = vm_value_as_cell(callback);

	/* Callback exceptions retain their original value and cross-realm transport. */
	status = vm_call(realm, callback, receiver, &wrapped, 1, &answer);
	if (status != 0)
		goto cleanup;
	value_cell = vm_value_is_cell(answer);
	if (value_cell)
		roots[2] = vm_value_as_cell(answer);

	/* Unsigned short conversion remains within the active callback operation. */
	status = vm_to_uint32(realm, answer, accepted);
	if (status == 0)
		*accepted &= 0xffffU;

cleanup:
	/* Caller-owned filter state remains while transient callback values are released. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Any callback or conversion error propagates after releasing local roots. */
	if (status != 0)
		return status;

	/* Succeeded: the callback's converted filter decision is available. */
	return 0;
}

/* Traces the root, current node, callback and candidate during user execution. */
static void
walker_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct walker_state *state;

	/* DOM nodes retain their owner Documents and managed contexts. */
	state = (struct walker_state *)cell;
	vm_heap_mark(heap, &state->root->cell);
	vm_heap_mark(heap, &state->current->cell);
	vm_heap_mark_word(heap, state->filter);

	/* A callback can unlink its candidate before triggering collection. */
	if (state->pending != NULL)
		vm_heap_mark(heap, &state->pending->cell);
}

/* Requires the private walker cell rather than an inherited prototype. */
static int
walker_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct walker_state **state)
{
	struct vm_object *wrapper;
	struct vm_cell *cell;
	int valid;
	int status;

	/* Primitive values cannot carry the platform brand. */
	valid = vm_value_is_object(this_value);
	if (!valid) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Ordinary objects inheriting TreeWalker.prototype provide no native state. */
	wrapper = (struct vm_object *)vm_value_as_cell(this_value);
	valid = vm_value_is_cell(wrapper->internal);
	if (wrapper->kind != VM_KIND_PLATFORM || !valid) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Other platform cells cannot impersonate a walker. */
	cell = vm_value_as_cell(wrapper->internal);
	if (cell->type != &walker_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the actual walker state is available. */
	*state = (struct walker_state *)cell;
	return 0;
}

/* Filters a candidate, clearing active and tracing state on every fallible exit. */
static int
walker_filter(
	struct vm_realm *realm,
	struct walker_state *state,
	struct dom_node *node,
	uint32_t *accepted)
{
	uint32_t bit;
	int status;

	/* Reentrant filtering fails before even masking the nested candidate. */
	if (state->active) {
		status = bind_throw_dom(realm, "InvalidStateError", "The walker filter is active.");
		return status;
	}

	/* The mask excludes unselected node kinds without calling the filter. */
	bit = 1U << (node->type - 1U);
	if ((state->mask & bit) == 0U) {
		*accepted = 3U;
		return 0;
	}

	/* Without a filter, every selected node kind is accepted. */
	if (state->filter == VM_VALUE_NULL) {
		*accepted = 1U;
		return 0;
	}

	/* The candidate survives callbacks that mutate, reenter or explicitly collect. */
	state->active = 1;
	state->pending = node;
	status = bind_filter_callback(realm, state->filter, node, accepted);
	state->pending = NULL;
	state->active = 0;
	if (status != 0)
		return status;

	/* Succeeded: no active callback or pending candidate remains. */
	return 0;
}

/* Traverses ancestors until a selected node or the root boundary is reached. */
static int
walker_ancestor(
	struct vm_realm *realm,
	struct walker_state *state,
	struct dom_node **answer)
{
	struct dom_node *node;
	uint32_t accepted;
	int status;

	/* The root has no ancestor within this walker even if connected elsewhere. */
	*answer = NULL;
	node = state->current;
	while (node != NULL && node != state->root) {
		node = node->parent;

		/* An absent parent ends navigation without changing currentNode. */
		if (node == NULL)
			break;

		/* Each actual ancestor is independently filtered. */
		status = walker_filter(realm, state, node, &accepted);
		if (status != 0)
			return status;

		/* Ancestor rejection skips this node rather than excluding higher ancestors. */
		if (accepted == 1U) {
			*answer = node;
			return 0;
		}
	}

	/* Succeeded: no acceptable ancestor exists within the current root boundary. */
	return 0;
}

/* Traverses children through skipped nodes while pruning rejected subtrees. */
static int
walker_children(
	struct vm_realm *realm,
	struct walker_state *state,
	int forward,
	struct dom_node **answer)
{
	struct dom_node *node;
	struct dom_node *related;
	uint32_t accepted;
	int status;

	/* First and last navigation begin at the corresponding actual child. */
	*answer = NULL;
	node = state->current->first_child;
	if (!forward)
		node = state->current->last_child;

	/* Searches the filtered child subtree without crossing its starting current node. */
	while (node != NULL) {
		status = walker_filter(realm, state, node, &accepted);
		if (status != 0)
			return status;

		/* The first selected child completes navigation. */
		if (accepted == 1U) {
			*answer = node;
			return 0;
		}

		/* Skipped children remain transparent to descendant navigation. */
		if (accepted == 3U) {
			related = node->first_child;
			if (!forward)
				related = node->last_child;

			/* A directional descendant becomes the next candidate. */
			if (related != NULL) {
				node = related;
				continue;
			}
		}

		/* Rejected or exhausted branches advance sideways before climbing. */
		while (node != NULL) {
			related = node->next;
			if (!forward)
				related = node->previous;

			/* An available sibling continues the same filtered child search. */
			if (related != NULL) {
				node = related;
				break;
			}

			/* The live current node and actual root bound traversal after mutations. */
			related = node->parent;
			if (related == NULL || related == state->root || related == state->current)
				return 0;
			node = related;
		}
	}

	/* Succeeded: no acceptable child exists in this direction. */
	return 0;
}

/* Traverses filtered siblings, climbing only through transparent ancestors. */
static int
walker_siblings(
	struct vm_realm *realm,
	struct walker_state *state,
	int forward,
	struct dom_node **answer)
{
	struct dom_node *node;
	struct dom_node *sibling;
	uint32_t accepted;
	int status;

	/* The actual root has no sibling within this walker. */
	*answer = NULL;
	node = state->current;
	if (node == state->root)
		return 0;

	/* Searches sibling branches before checking whether their parent is transparent. */
	while (node != NULL) {
		sibling = node->next;
		if (!forward)
			sibling = node->previous;

		/* A skipped sibling exposes its directional children. */
		while (sibling != NULL) {
			node = sibling;
			status = walker_filter(realm, state, node, &accepted);
			if (status != 0)
				return status;

			/* An accepted sibling completes navigation. */
			if (accepted == 1U) {
				*answer = node;
				return 0;
			}

			/* Rejected or empty branches advance sideways instead of descending. */
			sibling = node->first_child;
			if (!forward)
				sibling = node->last_child;
			if (accepted == 2U || sibling == NULL) {
				sibling = node->next;
				if (!forward)
					sibling = node->previous;
			}
		}

		/* An absent parent or the actual root ends sibling navigation. */
		node = node->parent;
		if (node == NULL || node == state->root)
			return 0;

		/* Ancestor filtering decides whether a higher sibling level is visible. */
		status = walker_filter(realm, state, node, &accepted);
		if (status != 0)
			return status;
		if (accepted == 1U)
			return 0;
	}

	/* Succeeded: no acceptable sibling exists in this direction. */
	return 0;
}

/* Traverses forward in filtered tree order using links after every callback. */
static int
walker_following(
	struct vm_realm *realm,
	struct walker_state *state,
	struct dom_node **answer)
{
	struct dom_node *node;
	struct dom_node *temporary;
	struct dom_node *sibling;
	uint32_t accepted;
	int status;

	/* Current nodes begin as transparent positions, including nodes outside the root. */
	*answer = NULL;
	node = state->current;
	accepted = 1U;

	/* Forward tree order descends unless a filter rejects the branch. */
	while (node != NULL) {
		while (accepted != 2U && node->first_child != NULL) {
			node = node->first_child;
			status = walker_filter(realm, state, node, &accepted);
			if (status != 0)
				return status;

			/* The first accepted descendant ends forward navigation. */
			if (accepted == 1U) {
				*answer = node;
				return 0;
			}
		}

		/* Finds the next actual sibling without crossing the root. */
		sibling = NULL;
		for (temporary = node; temporary != NULL; temporary = temporary->parent) {
			if (temporary == state->root)
				return 0;
			sibling = temporary->next;
			if (sibling != NULL)
				break;
		}

		/* Exhausting the outermost branch leaves currentNode unchanged. */
		if (sibling == NULL)
			return 0;

		/* Each sideways candidate is independently filtered after its live-link search. */
		node = sibling;
		status = walker_filter(realm, state, node, &accepted);
		if (status != 0)
			return status;

		/* An accepted sideways node completes forward navigation. */
		if (accepted == 1U) {
			*answer = node;
			return 0;
		}
	}

	/* Succeeded: forward traversal found no accepted node. */
	return 0;
}

/* Traverses backward in filtered tree order, including regrafted current nodes. */
static int
walker_preceding(
	struct vm_realm *realm,
	struct walker_state *state,
	struct dom_node **answer)
{
	struct dom_node *node;
	struct dom_node *sibling;
	uint32_t accepted;
	int status;

	/* The actual root boundary is checked dynamically after every callback. */
	*answer = NULL;
	node = state->current;
	while (node != state->root) {
		sibling = node->previous;

		/* Reverse order descends to the last non-rejected descendant of each sibling. */
		while (sibling != NULL) {
			node = sibling;
			status = walker_filter(realm, state, node, &accepted);
			if (status != 0)
				return status;

			/* Accepted and skipped branches expose their deepest reverse-order descendants. */
			while (accepted != 2U && node->last_child != NULL) {
				node = node->last_child;
				status = walker_filter(realm, state, node, &accepted);
				if (status != 0)
					return status;
			}

			/* The first accepted reverse-order candidate completes navigation. */
			if (accepted == 1U) {
				*answer = node;
				return 0;
			}

			/* Unaccepted leaf candidates advance to their preceding sibling. */
			sibling = node->previous;
		}

		/* Detached candidates cannot climb past an absent parent. */
		if (node == state->root || node->parent == NULL)
			return 0;

		/* Ancestors are themselves filtered after all their preceding children. */
		node = node->parent;
		status = walker_filter(realm, state, node, &accepted);
		if (status != 0)
			return status;

		/* A selected parent completes reverse traversal. */
		if (accepted == 1U) {
			*answer = node;
			return 0;
		}
	}

	/* Succeeded: no accepted node precedes the actual root. */
	return 0;
}

/* Chooses the exact algorithm and publishes its accepted result atomically. */
static int
walker_move(
	struct vm_realm *realm,
	vm_value this_value,
	enum walker_direction direction,
	vm_value *result)
{
	struct walker_state *state;
	struct dom_node *answer;
	struct vm_cell *answer_root;
	int status;

	/* Every navigation method requires a genuine native walker state. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Each operation follows its own algorithm rather than a flat collection scan. */
	answer = NULL;
	switch (direction) {
	case WALKER_PARENT:
		status = walker_ancestor(realm, state, &answer);
		break;
	case WALKER_FIRST:
		status = walker_children(realm, state, 1, &answer);
		break;
	case WALKER_LAST:
		status = walker_children(realm, state, 0, &answer);
		break;
	case WALKER_NEXT_SIBLING:
		status = walker_siblings(realm, state, 1, &answer);
		break;
	case WALKER_PREVIOUS_SIBLING:
		status = walker_siblings(realm, state, 0, &answer);
		break;
	case WALKER_NEXT:
		status = walker_following(realm, state, &answer);
		break;
	default:
		status = walker_preceding(realm, state, &answer);
		break;
	}

	/* Callback failures preserve currentNode apart from explicit callback changes. */
	if (status != 0)
		return status;

	/* Fallible wrapper publication completes before committing the selected node. */
	answer_root = NULL;
	if (answer != NULL) {
		answer_root = &answer->cell;
		status = vm_heap_add_root(realm->heap, &answer_root);
		if (status != 0)
			return status;
	}

	/* The candidate remains protected while its script wrapper is published. */
	status = bind_wrap_or_null(bind_window_of(realm), answer, result);
	if (answer != NULL)
		vm_heap_remove_root(realm->heap, &answer_root);
	if (status != 0)
		return status;

	/* Null results never alter currentNode. */
	if (answer != NULL)
		state->current = answer;

	/* Succeeded: the selected Node wrapper or null is available. */
	return 0;
}

/* Reports the immutable walker root using its actual owner-realm wrapper. */
static int
walker_root(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct walker_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Prototype inheritance alone cannot provide this accessor's native state. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Exposes this independently retained component of the walker graph. */
	status = bind_wrap(bind_window_of(realm), state->root, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual walker state component is available. */
	return 0;
}

/* Reports the live current node without applying the filter. */
static int
walker_current(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct walker_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Prototype inheritance alone cannot provide this accessor's native state. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Exposes this independently retained component of the walker graph. */
	status = bind_wrap(bind_window_of(realm), state->current, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual walker state component is available. */
	return 0;
}

/* Reports the complete unsigned node-kind mask. */
static int
walker_mask(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct walker_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Prototype inheritance alone cannot provide this accessor's native state. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Exposes this independently retained component of the walker graph. */
	*result = vm_value_number((double)state->mask);

	/* Succeeded: the actual walker state component is available. */
	return 0;
}

/* Reports the original callback object or null. */
static int
walker_callback(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct walker_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Prototype inheritance alone cannot provide this accessor's native state. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Exposes this independently retained component of the walker graph. */
	*result = state->filter;

	/* Succeeded: the actual walker state component is available. */
	return 0;
}

/* Updates currentNode to any actual Node, including one outside the root. */
static int
walker_current_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct walker_state *state;
	struct dom_node *node;
	int status;

	/* Receiver branding precedes actual Node argument validation. */
	status = walker_this(realm, this_value, &state);
	if (status != 0)
		return status;
	status = bind_argument_node(realm, js_argument(args, count, 0), &node);
	if (status != 0)
		return status;

	/* An outside node is allowed; tracing retains its own separate Document graph. */
	state->current = node;
	*result = VM_VALUE_UNDEFINED;

	/* Succeeded: currentNode now identifies the supplied native Node. */
	return 0;
}

/* Finds the first selected ancestor. */
static int
walker_parent(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_PARENT, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Finds the first selected child. */
static int
walker_first(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_FIRST, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Finds the last selected child. */
static int
walker_last(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_LAST, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Finds the next selected sibling. */
static int
walker_next_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_NEXT_SIBLING, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Finds the preceding selected sibling. */
static int
walker_previous_sibling(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_PREVIOUS_SIBLING, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Advances in filtered tree order. */
static int
walker_next(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_NEXT, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}

/* Retreats in filtered tree order. */
static int
walker_previous(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Dispatches this navigation method through the branded live-tree algorithm. */
	status = walker_move(realm, this_value, WALKER_PREVIOUS, result);
	if (status != 0)
		return status;

	/* Succeeded: the chosen navigation produced a Node or null. */
	return 0;
}
