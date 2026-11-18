/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed select additions use current option membership and protect graphs across callbacks. */

#include "bind/internal.h"

#include <errno.h>
#include <stdint.h>

static int select_add(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

static int option_default_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int option_default_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int option_selected_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int option_selected_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int select_index_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int select_index_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* A select exposes its traced SameObject native options wrapper. */
static const struct bind_attribute select_attributes[] = {
    {"options", bind_select_options, NULL},
    {"selectedIndex", select_index_get, select_index_set},
    {NULL, NULL, NULL}};

/* Typed addition leaves option selectedness and mutable collection setters to their own increment. */
static const struct bind_operation select_operations[] = {
    {"add", 1, select_add},
    {NULL, 0, NULL}};

/* Owned selection remains distinct from the independently reflected default attribute. */
static const struct bind_attribute option_attributes[] = {
    {"defaultSelected", option_default_get, option_default_set},
    {"selected", option_selected_get, option_selected_set},
    {NULL, NULL, NULL}};

/* Native select prototypes follow actual owner Documents independently of method borrowers. */
const struct bind_interface bind_html_select_element_interface = {
    "HTMLSelectElement", BIND_HTML_ELEMENT, 0, NULL, select_attributes, select_operations, NULL};

/* Exact option identities establish the first typed argument of native addition. */
const struct bind_interface bind_html_option_element_interface = {
    "HTMLOptionElement", BIND_HTML_ELEMENT, 0, NULL, option_attributes, NULL, NULL};

/* Groups can move complete option subtrees through the same checked insertion path. */
const struct bind_interface bind_html_opt_group_element_interface = {
    "HTMLOptGroupElement", BIND_HTML_ELEMENT, 0, NULL, NULL, NULL, NULL};

static int select_insert(struct vm_realm *realm, struct dom_node *select, struct dom_node *incoming, vm_value before, struct vm_cell **roots);
static int select_html(struct dom_node *node, int tag);
static void select_unroot(struct vm_heap *heap, struct vm_cell **roots, unsigned count);
static int select_this(struct vm_realm *realm, vm_value receiver, int tag, struct dom_element **element);

/* Validates native brands and protects every conversion and insertion participant. */
static int
select_add(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *select;
	struct dom_node *incoming;
	struct vm_cell *roots[5];
	vm_value before;
	vm_value argument;
	unsigned index;
	int actual;
	int cell;
	int status;

	/* Receiver validation precedes required argument and numeric union conversion. */
	status = bind_this_node(realm, receiver, &select);
	if (status != 0)
		return status;
	actual = select_html(select, DOM_TAG_SELECT);
	if (!actual) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the VM accepted this native operation exception. */
		return 0;
	}

	/* Only actual option and optgroup instances satisfy the typed required union. */
	argument = js_argument(args, count, 0);
	incoming = bind_node_of(argument);
	actual = select_html(incoming, DOM_TAG_OPTION);
	if (!actual)
		actual = select_html(incoming, DOM_TAG_OPTGROUP);
	if (!actual) {
		status = vm_throw_type_error(realm, "The element must be an actual option or optgroup.");
		if (status != 0)
			return status;

		/* Succeeded: the VM accepted this native operation exception. */
		return 0;
	}

	/* Conversion can detach every node otherwise held by the calling script. */
	before = js_argument(args, count, 1);
	roots[0] = &select->cell;
	roots[1] = &incoming->cell;
	roots[2] = NULL;
	roots[3] = NULL;
	roots[4] = NULL;
	cell = vm_value_is_cell(before);
	if (cell)
		roots[2] = vm_value_as_cell(before);

	/* Register all mutable slots before invoking numeric conversion or embedding callbacks. */
	for (index = 0; index < 5; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			select_unroot(realm->heap, roots, index);
			return status;
		}
	}

	/* The shared checked insertion algorithm never consults script-visible collections. */
	status = select_insert(realm, select, incoming, before, roots);
	if (status != 0) {
		select_unroot(realm->heap, roots, 5);
		return status;
	}

	/* Releases the rooted graph after its complete insertion outcome. */
	select_unroot(realm->heap, roots, 5);

	/* Succeeded: typed addition has no script result beyond its current DOM mutation. */
	*result = VM_VALUE_UNDEFINED;
	return 0;
}

/* Converts the optional union before resolving current ancestry and insertion location. */
static int
select_insert(
	struct vm_realm *realm,
	struct dom_node *select,
	struct dom_node *incoming,
	vm_value before,
	struct vm_cell **roots)
{
	struct dom_node *reference;
	struct dom_node *parent;
	struct dom_node *node;
	struct dom_element *element;
	int32_t position;
	int32_t offset;
	int status;

	/* A real HTMLElement selects the interface branch; other non-null values convert to long. */
	reference = NULL;
	position = -1;
	if (before != VM_VALUE_UNDEFINED && before != VM_VALUE_NULL) {
		reference = bind_node_of(before);
		if (reference != NULL) {
			/* Foreign elements and non-element Nodes take the numeric union branch. */
			if (reference->type != DOM_ELEMENT) {
				reference = NULL;
			} else {
				element = (struct dom_element *)reference;
				if (element->ns != DOM_NS_HTML)
					reference = NULL;
			}
		}

		/* Number conversion runs once and may replace or adopt the select's entire tree. */
		if (reference == NULL) {
			status = vm_to_int32(realm, before, &position);
			if (status != 0)
				return status;
		}
	}

	/* An incoming ancestor is rejected before testing the supplied reference's membership. */
	node = select->parent;
	while (node != NULL) {
		if (node == incoming) {
			status = bind_throw_dom(realm, "HierarchyRequestError", "The incoming element contains this select.");
			if (status != 0)
				return status;

			/* Succeeded: the VM accepted this native operation exception. */
			return 0;
		}

		/* Continue checking the select's current ancestor chain. */
		node = node->parent;
	}

	/* An element reference must be any current descendant, rather than only an option member. */
	if (reference != NULL) {
		node = reference->parent;
		while (node != NULL && node != select)
			node = node->parent;

		/* A sibling, detached node or the select itself does not supply a descendant reference. */
		if (node != select) {
			status = bind_throw_dom(realm, "NotFoundError", "The reference is not a descendant of this select.");
			if (status != 0)
				return status;

			/* Succeeded: the VM accepted this native operation exception. */
			return 0;
		}

		/* Re-adding an element before itself is a successful mutation-free operation. */
		if (incoming == reference)
			return 0;
	}

	/* Resolve numeric indices against the current native list only after conversion. */
	if (reference == NULL && position >= 0) {
		offset = 0;
		reference = dom_select_option_next(select, NULL);
		while (reference != NULL && offset < position) {
			reference = dom_select_option_next(select, reference);
			offset++;
		}
	}

	/* A group option reference inserts into that option's actual parent; missing indices append. */
	parent = select;
	if (reference != NULL)
		parent = reference->parent;
	roots[4] = &parent->cell;
	if (reference != NULL)
		roots[3] = &reference->cell;
	status = bind_insert(realm, parent, incoming, reference);
	if (status != 0)
		return status;

	/* Succeeded: actual owner adoption and host notifications completed through canonical insertion. */
	return 0;
}

/* Validates actual HTML local identity independently of mutable wrapper prototypes. */
static int
select_html(
	struct dom_node *node,
	int tag)
{
	struct dom_element *element;
	const char *name;
	int actual;

	/* Invalid union operands and non-elements cannot establish a native select-family brand. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;
	element = (struct dom_element *)node;

	/* Namespace and exact case take precedence over the parser's internal folded tag code. */
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;
	name = dom_tag_name(tag);
	actual = vm_string_equal_ascii(element->local_name, name);

	/* Succeeded: the native brand check has no conversion or callback effects. */
	return actual;
}

/* Releases exactly the root slots registered by the current invocation. */
static void
select_unroot(
	struct vm_heap *heap,
	struct vm_cell **roots,
	unsigned count)
{
	unsigned index;

	/* Root slots protect only the synchronous invocation, leaving no lasting ownership cycle. */
	for (index = 0; index < count; index++) {
		vm_heap_remove_root(heap, &roots[index]);
	}

	/* Succeeded: normal, conversion-failure and host-failure exits share complete cleanup. */
	return;
}

/* Reads default attribute presence while retaining the actual option brand. */
static int
option_default_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *option;
	struct vm_string *attribute;
	int present;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Default reflection observes attribute presence independently of dirty owned state. */
	status = select_this(realm, receiver, DOM_TAG_OPTION, &option);
	if (status != 0)
		return status;
	attribute = dom_attribute_ascii(option, "selected");
	present = 0;
	if (attribute != NULL)
		present = 1;

	/* Succeeded: only the content attribute determines defaultSelected. */
	*result = vm_value_boolean(present);
	return 0;
}

/* Reflects default selection with no user-defined Boolean conversion callbacks. */
static int
option_default_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *option;
	struct vm_string *name;
	struct vm_string *empty;
	int selected;
	int status;

	/* Actual option brands are checked before conversion or allocating attribute atoms. */
	*result = VM_VALUE_UNDEFINED;
	status = select_this(realm, receiver, DOM_TAG_OPTION, &option);
	if (status != 0)
		return status;
	selected = vm_to_boolean(js_argument(args, count, 0));
	name = vm_atom_from_ascii(realm->heap, "selected");
	if (name == NULL)
		return ENOMEM;

	/* False removes the attribute without changing dirty owned selectedness. */
	if (!selected) {
		status = dom_element_remove_attribute(option, name);
		if (status != 0)
			return status;

		/* Succeeded: the absent default passes through ordinary attribute hooks. */
		return 0;
	}

	/* Fully allocate the empty Boolean attribute before publishing its committed presence. */
	empty = vm_atom_from_ascii(realm->heap, "");
	if (empty == NULL)
		return ENOMEM;

	/* Publishes the completely allocated default attribute value. */
	status = dom_element_set_attribute(option, name, empty);
	if (status != 0)
		return status;

	/* Succeeded: default presence and clean-state normalization reflect the same DOM mutation. */
	return 0;
}

/* Reads actual owned selectedness without changing a clean default or explicit empty index. */
static int
option_selected_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *option;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native owned state remains observable after removal or adoption. */
	status = select_this(realm, receiver, DOM_TAG_OPTION, &option);
	if (status != 0)
		return status;

	/* Succeeded: dirty selectedness need not match the default attribute. */
	*result = vm_value_boolean(option->option_selected);
	return 0;
}

/* Writes dirty option selectedness and normalizes actual single-select peers. */
static int
option_selected_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *option;
	int selected;
	int status;

	/* The Boolean conversion cannot invoke script or relocate the actual branded option. */
	*result = VM_VALUE_UNDEFINED;
	status = select_this(realm, receiver, DOM_TAG_OPTION, &option);
	if (status != 0)
		return status;
	selected = vm_to_boolean(js_argument(args, count, 0));
	dom_option_set_selected(option, selected, 1);

	/* Succeeded: selection, dirtiness and current peers are updated without default reflection. */
	return 0;
}

/* Reads the first selected member of the current native list without causing fallback. */
static int
select_index_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *select;
	struct dom_node *node;
	struct dom_element *option;
	int32_t index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Exact receiver identity precedes all membership and owned-state observations. */
	status = select_this(realm, receiver, DOM_TAG_SELECT, &select);
	if (status != 0)
		return status;

	/* Native membership supplies tree order, regardless of script-visible list overrides. */
	index = 0;
	node = dom_select_option_next(&select->node, NULL);
	while (node != NULL) {
		option = (struct dom_element *)node;
		if (option->option_selected) {
			*result = vm_value_int32(index);
			return 0;
		}

		/* Refuse an unrepresentable host index rather than overflowing signed arithmetic. */
		if (index == INT32_MAX)
			return EOVERFLOW;
		index++;
		node = dom_select_option_next(&select->node, node);
	}

	/* No current member is selected, including explicitly cleared single-select lists. */
	*result = vm_value_int32(-1);
	return 0;
}

/* Converts the proposed index once while explicitly retaining target and conversion argument. */
static int
select_index_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *select;
	struct dom_element *option;
	struct dom_node *node;
	struct vm_cell *roots[2];
	vm_value argument;
	size_t offset;
	int32_t index;
	int cell;
	int status;

	/* Receiver brands precede numeric conversion and every reentrant tree mutation. */
	*result = VM_VALUE_UNDEFINED;
	status = select_this(realm, receiver, DOM_TAG_SELECT, &select);
	if (status != 0)
		return status;
	argument = js_argument(args, count, 0);
	roots[0] = &select->node.cell;
	roots[1] = NULL;
	cell = vm_value_is_cell(argument);
	if (cell)
		roots[1] = vm_value_as_cell(argument);

	/* Retain both native participants before a valueOf callback can detach and collect them. */
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &roots[0]);
		return status;
	}

	/* Current membership is selected only after the single WebIDL long conversion completes. */
	status = vm_to_int32(realm, argument, &index);
	if (status != 0) {
		select_unroot(realm->heap, roots, 2);
		return status;
	}

	/* Explicit index assignment clears all states and only dirties the matching selected member. */
	offset = 0;
	node = dom_select_option_next(&select->node, NULL);
	while (node != NULL) {
		option = (struct dom_element *)node;
		option->option_selected = 0;
		if (index >= 0 && offset == (size_t)index) {
			option->option_selected = 1;
			option->option_dirty = 1;
		}

		/* The pure traversal performs no allocation or callbacks after conversion. */
		offset++;
		node = dom_select_option_next(&select->node, node);
	}

	/* Deliberately skip fallback: invalid or negative indices preserve an empty selected state. */
	dom_note_change(&select->node, DOM_CHANGE_STATE, NULL, NULL);
	select_unroot(realm->heap, roots, 2);

	/* Succeeded: current owned selection is published and both temporary root slots are released. */
	return 0;
}

/* Establishes actual native identity independently of wrapper prototypes and expandos. */
static int
select_this(
	struct vm_realm *realm,
	vm_value receiver,
	int tag,
	struct dom_element **element)
{
	struct dom_node *node;
	int actual;
	int status;

	/* A forged or foreign wrapper cannot establish an exact select-family identity. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	actual = select_html(node, tag);
	if (!actual) {
		status = bind_throw_illegal(realm);
		if (status != 0)
			return status;

		/* Succeeded: the VM accepted this native operation exception. */
		return 0;
	}

	/* Succeeded: this exact node supports the requested owned-state accessor. */
	*element = (struct dom_element *)node;
	return 0;
}
