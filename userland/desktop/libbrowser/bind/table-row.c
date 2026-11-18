/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Row operations use actual live membership after conversion and retain pending mutations across GC. */

#include "bind/internal.h"

#include <errno.h>

/* Each invocation selects one native table or section mutation contract. */
enum row_action {
	ROW_TABLE_INSERT,
	ROW_TABLE_DELETE,
	ROW_SECTION_INSERT,
	ROW_SECTION_DELETE,
	ROW_BODY_CREATE
};

static int row_html(const struct dom_node *node, int tag);
static int row_section(const struct dom_node *node);
static struct dom_node *row_next(struct dom_node *owner, struct dom_node *previous, int table);
static struct dom_node *row_last_body(struct dom_node *table);
static int row_mutate(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, enum row_action action, vm_value *result);
static int row_execute(struct vm_realm *realm, struct dom_node *owner, const vm_value *args, unsigned count, enum row_action action, struct vm_cell **roots, vm_value *result);
static int row_create(struct vm_realm *realm, struct dom_node *owner, int tag, struct vm_cell **slot, struct dom_node **created);
static int row_insert(struct vm_realm *realm, struct dom_node *owner, struct dom_node *reference, struct dom_node *last, int table, struct vm_cell **roots, vm_value *result);
static int row_get_index(struct vm_realm *realm, vm_value receiver, int section, vm_value *result);
static void row_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);

/*
 * Creates a fresh tbody after the last actual direct tbody.
 */
int
bind_table_body_create(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The rooted native algorithm converts arguments before observing current membership. */
	status = row_mutate(realm, receiver, args, count, ROW_BODY_CREATE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native mutation is visible through the actual live DOM. */
	return 0;
}

/*
 * Inserts a row using the table's current logical order.
 */
int
bind_table_row_insert(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The rooted native algorithm converts arguments before observing current membership. */
	status = row_mutate(realm, receiver, args, count, ROW_TABLE_INSERT, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native mutation is visible through the actual live DOM. */
	return 0;
}

/*
 * Deletes the selected current logical table row.
 */
int
bind_table_row_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The rooted native algorithm converts arguments before observing current membership. */
	status = row_mutate(realm, receiver, args, count, ROW_TABLE_DELETE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native mutation is visible through the actual live DOM. */
	return 0;
}

/*
 * Inserts an actual direct row into its native section.
 */
int
bind_section_row_insert(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The rooted native algorithm converts arguments before observing current membership. */
	status = row_mutate(realm, receiver, args, count, ROW_SECTION_INSERT, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native mutation is visible through the actual live DOM. */
	return 0;
}

/*
 * Deletes the selected actual direct section row.
 */
int
bind_section_row_delete(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The rooted native algorithm converts arguments before observing current membership. */
	status = row_mutate(realm, receiver, args, count, ROW_SECTION_DELETE, result);
	if (status != 0)
		return status;

	/* Succeeded: the requested native mutation is visible through the actual live DOM. */
	return 0;
}

/*
 * Reads the row's current native table index independently of script-visible collections.
 */
int
bind_row_index(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Current actual parent links decide which native ordered list supplies the index. */
	status = row_get_index(realm, receiver, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the current index or native absent-parent value is available. */
	return 0;
}

/*
 * Reads the row's current native section index independently of script-visible collections.
 */
int
bind_row_section_index(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Current actual parent links decide which native ordered list supplies the index. */
	status = row_get_index(realm, receiver, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the current index or native absent-parent value is available. */
	return 0;
}

/* Requires actual HTML namespace/tag/exact local case regardless of forged prototypes. */
static int
row_html(
	const struct dom_node *node,
	int tag)
{
	const struct dom_element *element;
	const char *name;
	int matches;

	/* Non-element receivers and foreign namespaces cannot acquire native row-family identity. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)node;
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;

	/* XML internal tag folding does not make an uppercase local name a genuine HTML row. */
	name = dom_tag_name(tag);
	matches = vm_string_equal_ascii(element->local_name, name);
	if (!matches)
		return 0;

	/* Succeeded: the exact actual HTML element has the required native brand. */
	return 1;
}

/* Recognizes each actual HTML table section without prototype or attribute coercion. */
static int
row_section(
	const struct dom_node *node)
{
	int matches;

	/* The three section names share one native interface but retain distinct table roles. */
	matches = row_html(node, DOM_TAG_THEAD);
	if (matches)
		return 1;
	matches = row_html(node, DOM_TAG_TBODY);
	if (matches)
		return 1;
	matches = row_html(node, DOM_TAG_TFOOT);
	if (matches)
		return 1;

	/* No actual HTML section identity is present. */
	return 0;
}

/* Returns the next native member in logical table order or direct section order. */
static struct dom_node *
row_next(
	struct dom_node *owner,
	struct dom_node *previous,
	int table)
{
	struct dom_node *node;
	int matches;

	/* Table membership shares the actual live collection's pure ordered traversal. */
	if (table) {
		node = bind_table_row_next(owner, previous);
		return node;
	}

	/* Sections contribute only their direct actual rows in sibling order. */
	node = owner->first_child;
	if (previous != NULL)
		node = previous->next;
	while (node != NULL) {
		matches = row_html(node, DOM_TAG_TR);
		if (matches)
			return node;
		node = node->next;
	}

	/* No later eligible direct section row exists. */
	return NULL;
}

/* Finds the last actual direct body without following descendants or virtual named properties. */
static struct dom_node *
row_last_body(
	struct dom_node *table)
{
	struct dom_node *node;
	struct dom_node *last;
	int matches;

	/* Multiple or misplaced actual bodies still use their physical direct-child order. */
	last = NULL;
	for (node = table->first_child; node != NULL; node = node->next) {
		matches = row_html(node, DOM_TAG_TBODY);
		if (matches)
			last = node;
	}

	/* Succeeded: this is the last actual direct body, or none. */
	return last;
}

/* Validates receiver identity and roots its complete pending conversion/mutation graph. */
static int
row_mutate(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	enum row_action action,
	vm_value *result)
{
	struct dom_node *owner;
	struct vm_cell *roots[6];
	vm_value argument;
	unsigned index;
	int matches;
	int cell;
	int status;

	/* Branding precedes required-argument checks and all user numeric conversion. */
	*result = VM_VALUE_UNDEFINED;
	status = bind_this_node(realm, receiver, &owner);
	if (status != 0)
		return status;
	if (action == ROW_SECTION_INSERT || action == ROW_SECTION_DELETE) {
		matches = row_section(owner);
	} else {
		matches = row_html(owner, DOM_TAG_TABLE);
	}

	/* Only actual table/section nodes can enter their native mutation algorithms. */
	if (!matches) {
		status = vm_throw_type_error(realm, "Row operation has the wrong native receiver.");
		return status;
	}

	/* A required deletion index cannot be supplied by an omitted argument. */
	if (count == 0 &&
	    (action == ROW_TABLE_DELETE || action == ROW_SECTION_DELETE)) {
		status = vm_throw_type_error(realm, "deleteRow requires an index.");
		return status;
	}

	/* Owner, new row/body, reference/parent and argument must survive arbitrary conversion/host GC. */
	roots[0] = &owner->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	roots[4] = NULL;
	roots[5] = NULL;
	argument = js_argument(args, count, 0);
	cell = vm_value_is_cell(argument);
	if (cell)
		roots[5] = vm_value_as_cell(argument);

	/* Every successful root registration is removed on partial registration failure. */
	for (index = 0; index < 6U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			row_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* User conversion and native mutation share exactly one complete temporary-root cleanup. */
	status = row_execute(realm, owner, args, count, action, roots, result);
	row_unroot(realm->heap, roots, 6U);
	if (status != 0)
		return status;

	/* Succeeded: current native state is published and every temporary slot has been removed. */
	return 0;
}

/* Applies one signed long conversion before reading live membership and performing the chosen mutation. */
static int
row_execute(
	struct vm_realm *realm,
	struct dom_node *owner,
	const vm_value *args,
	unsigned count,
	enum row_action action,
	struct vm_cell **roots,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_node *node;
	struct dom_node *selected;
	struct dom_node *last;
	struct dom_node *parent;
	struct dom_node *reference;
	vm_value argument;
	int32_t position;
	int32_t length;
	int table;
	int removing;
	int status;

	/* A body create ignores arguments and inserts a fresh body after the last actual one. */
	if (action == ROW_BODY_CREATE) {
		last = row_last_body(owner);
		reference = NULL;
		if (last != NULL)
			reference = last->next;
		if (reference != NULL)
			roots[3] = &reference->cell;
		status = row_create(realm, owner, DOM_TAG_TBODY, &roots[1], &node);
		if (status != 0)
			return status;
		status = bind_insert(realm, owner, node, reference);
		if (status != 0)
			return status;
		window = bind_window_of(realm);
		status = bind_wrap(window, node, result);
		if (status != 0)
			return status;

		/* Succeeded: a fresh actual body wrapper follows the last current direct body. */
		return 0;
	}

	/* Optional undefined means the insertion default, while explicit deletion undefined converts to zero. */
	table = 0;
	if (action == ROW_TABLE_INSERT || action == ROW_TABLE_DELETE)
		table = 1;
	removing = 0;
	if (action == ROW_TABLE_DELETE || action == ROW_SECTION_DELETE)
		removing = 1;
	position = -1;
	argument = js_argument(args, count, 0);
	if (removing || argument != VM_VALUE_UNDEFINED) {
		status = vm_to_int32(realm, argument, &position);
		if (status != 0)
			return status;
	}

	/* Current membership is read only after conversion, so callbacks can move or replace rows. */
	length = 0;
	selected = NULL;
	last = NULL;
	node = row_next(owner, NULL, table);
	while (node != NULL) {
		if (length == position)
			selected = node;
		if (length == INT32_MAX)
			return EOVERFLOW;
		length++;
		last = node;
		node = row_next(owner, node, table);
	}

	/* Invalid ranges cannot allocate or mutate native nodes. */
	if (position < -1 ||
	    position > length ||
	    (removing && position >= length)) {
		status = bind_throw_dom(realm, "IndexSizeError", "The row index is outside the current native row list.");
		return status;
	}

	/* Appending uses the last current row's actual parent rather than physical table order. */
	if (!removing) {
		status = row_insert(realm, owner, selected, last, table, roots, result);
		if (status != 0)
			return status;

		/* Succeeded: the new native row uses the current insertion parent. */
		return 0;
	}

	/* The legacy -1 deletion removes the current final row and is a no-op on an empty list. */
	if (position == -1)
		selected = last;
	if (selected == NULL)
		return 0;
	parent = selected->parent;
	roots[1] = &selected->cell;
	roots[4] = &parent->cell;
	window = bind_window_of(realm);
	if (parent->document->view != NULL)
		window = parent->document->view;
	dom_remove(selected);
	status = bind_environment_child_mutation(window, parent, NULL, selected);
	if (status != 0)
		return status;

	/* Succeeded: the selected row is removed while current weak iterators and environment are repaired. */
	return 0;
}

/* Creates an exact actual HTML element in the owner's current node Document without publishing a wrapper. */
static int
row_create(
	struct vm_realm *realm,
	struct dom_node *owner,
	int tag,
	struct vm_cell **slot,
	struct dom_node **created)
{
	struct dom_element *element;
	struct vm_string *name;
	const char *local;

	/* A temporary atom is retained until the new native node traces its name. */
	local = dom_tag_name(tag);
	name = vm_atom_from_ascii(realm->heap, local);
	if (name == NULL)
		return ENOMEM;
	*slot = &name->cell;
	element = dom_element_create(owner->document, DOM_NS_HTML, name, NULL);
	if (element == NULL)
		return ENOMEM;
	*created = &element->node;
	*slot = &element->node.cell;

	/* Succeeded: the caller's registered slot retains a complete unpublished native element. */
	return 0;
}

/* Inserts a rooted row before an indexed member or after the last member in its actual parent. */
static int
row_insert(
	struct vm_realm *realm,
	struct dom_node *owner,
	struct dom_node *reference,
	struct dom_node *last,
	int table,
	struct vm_cell **roots,
	vm_value *result)
{
	struct dom_node *row;
	struct dom_node *body;
	struct dom_node *parent;
	struct dom_node *insertion;
	struct bind_window *window;
	int status;

	/* New rows always use the current owner after any numeric conversion callback. */
	status = row_create(realm, owner, DOM_TAG_TR, &roots[1], &row);
	if (status != 0)
		return status;
	parent = owner;
	insertion = row;

	/* A zero-row table appends into its last actual body, creating a complete new body when absent. */
	if (table && last == NULL) {
		body = row_last_body(owner);
		if (body == NULL) {
			status = row_create(realm, owner, DOM_TAG_TBODY, &roots[2], &body);
			if (status != 0)
				return status;

			/* No observer sees a partially linked newly created body before its checked publication. */
			dom_append_child(body, row);
			insertion = body;
		} else {
			parent = body;
		}
	} else if (table) {
		/* An indexed logical row supplies its own section or direct-table parent. */
		if (reference != NULL) {
			parent = reference->parent;
		} else {
			parent = last->parent;
		}
	}

	/* Parent and anchor survive a host callback which removes the new subtree and its siblings. */
	roots[4] = &parent->cell;
	if (reference != NULL)
		roots[3] = &reference->cell;
	status = bind_insert(realm, parent, insertion, reference);
	if (status != 0)
		return status;
	window = bind_window_of(realm);
	status = bind_wrap(window, row, result);
	if (status != 0)
		return status;

	/* Succeeded: the inserted row wrapper uses its actual current Document prototype. */
	return 0;
}

/* Reads a readonly current row index from the required actual native parent list. */
static int
row_get_index(
	struct vm_realm *realm,
	vm_value receiver,
	int section,
	vm_value *result)
{
	struct dom_node *row;
	struct dom_node *owner;
	struct dom_node *node;
	int32_t position;
	int table;
	int matches;
	int status;

	/* Exact native row identity cannot be supplied by prototype-only or foreign elements. */
	status = bind_this_node(realm, receiver, &row);
	if (status != 0)
		return status;
	matches = row_html(row, DOM_TAG_TR);
	if (!matches) {
		status = vm_throw_type_error(realm, "Row index requires an actual HTML row.");
		return status;
	}

	/* A detached row or unrelated immediate parent has no native index. */
	*result = vm_value_int32(-1);
	owner = row->parent;
	table = row_html(owner, DOM_TAG_TABLE);
	if (!table) {
		matches = row_section(owner);
		if (!matches)
			return 0;

		/* The whole-table index additionally requires a direct actual table grandparent. */
		if (!section) {
			owner = owner->parent;
			table = row_html(owner, DOM_TAG_TABLE);
			if (!table)
				return 0;
		}
	}

	/* The same native ordered iterator supplies table indices and direct-table section indices. */
	position = 0;
	node = row_next(owner, NULL, table);
	while (node != NULL) {
		if (node == row) {
			*result = vm_value_int32(position);
			return 0;
		}

		/* Every preceding eligible native row consumes exactly one signed index. */
		if (position == INT32_MAX)
			return EOVERFLOW;
		position++;
		node = row_next(owner, node, table);
	}

	/* Succeeded: an absent current member retains the native -1 result. */
	return 0;
}

/* Removes every temporary slot registered by one native row invocation. */
static void
row_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	unsigned index;

	/* No conversion or host failure leaves a pointer into an expired native stack. */
	for (index = 0; index < count; index++)
		vm_heap_remove_root(heap, &roots[index]);

	/* Succeeded: no temporary row mutation root remains registered. */
	return;
}
