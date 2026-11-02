/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Live collections derive membership and virtual properties from current DOM links. */

#include "bind/internal.h"

#include <errno.h>
#include <stdio.h>

/* Native live lists represent direct children, document descendants, owned controls or ordered table rows. */
enum collection_kind {
	COLLECTION_CHILDREN,
	COLLECTION_LINKS,
	COLLECTION_FORMS,
	COLLECTION_CONTROLS,
	COLLECTION_RADIO,
	COLLECTION_TABLE_BODIES,
	COLLECTION_TABLE_ROWS,
	COLLECTION_SECTION_ROWS,
	COLLECTION_ROW_CELLS,
	COLLECTION_OPTIONS,
	COLLECTION_IMAGES,
	COLLECTION_SCRIPTS,
	COLLECTION_EMPTY_NODE_LIST
};

/* A wrapper retains its actual root and therefore its relevant Document owner. */
struct collection_state {
	struct vm_cell cell;
	struct dom_node *root;
	enum collection_kind kind;
	/* Duplicate lists retain their original exact name across attribute mutations. */
	struct vm_string *name;
};

static void collection_trace(struct vm_heap *heap, struct vm_cell *cell);
static int collection_get_own(struct vm_object *object, vm_value key, struct vm_property *property);
static int collection_own_keys(struct vm_heap *heap, struct vm_object *object, struct wb_vector *keys);
static int collection_define(struct vm_realm *realm, struct vm_object *object, vm_value key, const struct vm_descriptor *descriptor, int *handled, int *done);
static int collection_delete(struct vm_heap *heap, struct vm_object *object, vm_value key, int *handled, int *deleted);
static int collection_prevent_extensions(struct vm_object *object, int *allowed);
static int collection_length(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int collection_item(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int collection_named_item(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int controls_named_item(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int node_list_length(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int node_list_item(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int form_elements(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int form_length(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int radio_value_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int radio_value_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int table_bodies(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int table_rows(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int section_rows(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int row_cells(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* No raw realm or Window lifetime participates in collection reachability. */
static const struct vm_cell_type collection_type = { "html-collection", collection_trace, NULL };

/* Legacy data properties remain virtual and collection objects remain extensible. */
static const struct vm_native_operations collection_native = {
	collection_get_own,
	collection_own_keys,
	collection_define,
	collection_delete,
	collection_prevent_extensions
};

/* Length is inherited, leaving named-member visibility to the legacy algorithm. */
static const struct bind_attribute collection_attributes[] = {
	{ "length", collection_length, NULL },
	{ NULL, NULL, NULL }
};

/* Missing members report null through methods and undefined through property access. */
static const struct bind_operation collection_operations[] = {
	{ "item", 1, collection_item },
	{ "namedItem", 1, collection_named_item },
	{ NULL, 0, NULL }
};

/* Only native DOM accessors can construct genuine collection wrappers. */
const struct bind_interface bind_html_collection_interface = {
	"HTMLCollection", BIND_NO_PARENT, 0, NULL, collection_attributes, collection_operations, NULL
};

/* Options inherit the existing native read API; mutable setters are a separate increment. */
const struct bind_interface bind_html_options_collection_interface = {
	"HTMLOptionsCollection", BIND_HTML_COLLECTION, 0, NULL, NULL, NULL, NULL
};

/* Controls use the inherited indexed API but replace duplicate-name selection. */
static const struct bind_operation controls_operations[] = {
	{ "namedItem", 1, controls_named_item },
	{ NULL, 0, NULL }
};

/* Indexed NodeList core stays separate from HTMLCollection's legacy named getter. */
static const struct bind_attribute node_list_attributes[] = {
	{ "length", node_list_length, NULL },
	{ NULL, NULL, NULL }
};

/* Radio duplicate lists inherit branded indexed access from NodeList. */
static const struct bind_operation node_list_operations[] = {
	{ "item", 1, node_list_item },
	{ NULL, 0, NULL }
};

/* Each form caches its elements wrapper while its length observes that live list. */
static const struct bind_attribute form_attributes[] = {
	{ "elements", form_elements, NULL },
	{ "length", form_length, NULL },
	{ NULL, NULL, NULL }
};

/* Only an actual form can produce an owner-filtered native collection. */
const struct bind_interface bind_html_form_element_interface = {
	"HTMLFormElement", BIND_HTML_ELEMENT, 0, NULL, form_attributes, NULL, NULL
};

/* Ordinary collections retain their base API through this more specific prototype. */
const struct bind_interface bind_html_form_controls_collection_interface = {
	"HTMLFormControlsCollection", BIND_HTML_COLLECTION, 0, NULL, NULL, controls_operations, NULL
};

/* The indexed core is native; iterable methods are a separate recorded increment. */
const struct bind_interface bind_node_list_interface = {
	"NodeList", BIND_NO_PARENT, 0, NULL, node_list_attributes, node_list_operations, NULL
};

/* List values use actual radio checkedness and content values, never member expandos. */
static const struct bind_attribute radio_attributes[] = {
	{ "value", radio_value_get, radio_value_set },
	{ NULL, NULL, NULL }
};

/* Duplicate lists are live even when their original name later has zero matches. */
const struct bind_interface bind_radio_node_list_interface = {
	"RadioNodeList", BIND_NODE_LIST, 0, NULL, radio_attributes, NULL, NULL
};

/* Each native table has distinct cached wrappers for bodies and logically ordered rows. */
static const struct bind_attribute table_attributes[] = {
	{ "caption", bind_table_caption_get, bind_table_caption_set },
	{ "tHead", bind_table_head_get, bind_table_head_set },
	{ "tFoot", bind_table_foot_get, bind_table_foot_set },
	{ "tBodies", table_bodies, NULL },
	{ "rows", table_rows, NULL },
	{ NULL, NULL, NULL }
};

/* Native structural methods preserve current first-child identity and canonical insertion positions. */
static const struct bind_operation table_operations[] = {
	{ "createTBody", 0, bind_table_body_create },
	{ "insertRow", 0, bind_table_row_insert },
	{ "deleteRow", 1, bind_table_row_delete },
	{ "createCaption", 0, bind_table_caption_create },
	{ "deleteCaption", 0, bind_table_caption_delete },
	{ "createTHead", 0, bind_table_head_create },
	{ "deleteTHead", 0, bind_table_head_delete },
	{ "createTFoot", 0, bind_table_foot_create },
	{ "deleteTFoot", 0, bind_table_foot_delete },
	{ NULL, 0, NULL }
};

/* Every actual table section exposes the same direct-child row contract. */
static const struct bind_attribute section_attributes[] = {
	{ "rows", section_rows, NULL },
	{ NULL, NULL, NULL }
};

/* Native section mutations operate on direct rows rather than script-visible expandos. */
static const struct bind_operation section_operations[] = {
	{ "insertRow", 0, bind_section_row_insert },
	{ "deleteRow", 1, bind_section_row_delete },
	{ NULL, 0, NULL }
};

/* Cells include actual direct td/th children without following nested tables. */
static const struct bind_attribute row_attributes[] = {
	{ "rowIndex", bind_row_index, NULL },
	{ "sectionRowIndex", bind_row_section_index, NULL },
	{ "cells", row_cells, NULL },
	{ NULL, NULL, NULL }
};

/* Table construction remains native to DOM creation rather than direct script construction. */
const struct bind_interface bind_html_table_element_interface = {
	"HTMLTableElement", BIND_HTML_ELEMENT, 0, NULL, table_attributes, table_operations, NULL
};

/* Thead, tbody and tfoot share one exact native section brand. */
const struct bind_interface bind_html_table_section_element_interface = {
	"HTMLTableSectionElement", BIND_HTML_ELEMENT, 0, NULL, section_attributes, section_operations, NULL
};

/* Actual tr nodes inherit ordinary HTMLElement behavior and native live cells. */
const struct bind_interface bind_html_table_row_element_interface = {
	"HTMLTableRowElement", BIND_HTML_ELEMENT, 0, NULL, row_attributes, NULL, NULL
};

static int collection_create(struct vm_realm *realm, struct dom_node *root, enum collection_kind kind, struct vm_string *name, struct vm_object **out);
static int collection_this(struct vm_realm *realm, vm_value receiver, int list, struct collection_state **out);
static struct dom_node *collection_next(struct collection_state *state, struct dom_node *node);
static int collection_key_index(vm_value key, uint32_t *index);
static int collection_matches(struct collection_state *state, struct dom_node *node);
static struct dom_node *collection_index(struct collection_state *state, uint32_t index);
static struct dom_node *collection_named(struct collection_state *state, struct vm_string *name);
static int collection_visible(struct vm_object *object, vm_value key);
static int collection_name_key(struct vm_heap *heap, struct vm_object *object, struct vm_string *name, struct wb_vector *keys);
static struct dom_node *collection_root(struct collection_state *state);
static int collection_name_matches(struct dom_node *node, struct vm_string *name);
static int collection_named_value(struct collection_state *state, struct vm_string *name, vm_value *result);
static int collection_read_length(struct collection_state *state, vm_value *result);
static int collection_read_item(struct vm_realm *realm, struct collection_state *state, const vm_value *args, unsigned count, vm_value *result);
static int collection_table_html(const struct dom_node *node, int tag);
static int collection_section_rank(const struct dom_node *node);
static struct dom_node *collection_table_next(struct collection_state *state, struct dom_node *node);
static int collection_table_get(struct vm_realm *realm, vm_value receiver, enum collection_kind kind, vm_value *result);

/*
 * Reports the next actual row using the live table collection's native logical order.
 * Native mutation/index algorithms use this bridge without observing script properties.
 */
struct dom_node *
bind_table_row_next(
	struct dom_node *table,
	struct dom_node *previous)
{
	struct collection_state state = {0};
	struct dom_node *node;

	/* The ordered walker reads only its current root, without storing or allocating traversal state. */
	state.root = table;
	node = collection_table_next(&state, previous);

	/* Succeeded: the next current native row, or exhaustion, is available. */
	return node;
}

/*
 * Reports the same live direct-element collection on every ParentNode observation.
 */
int
bind_children(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* ParentNode is implemented by Documents, Elements and DocumentFragments only. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT &&
	    node->type != DOM_ELEMENT &&
	    node->type != DOM_DOCUMENT_FRAGMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* Publish the cache only after its complete native wrapper has been allocated. */
	if (node->children_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_CHILDREN, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		node->children_collection = wrapper;
	}

	/* Succeeded: the node retains the identity of its live element list. */
	*result = vm_value_cell(node->children_collection);
	return 0;
}

/*
 * Reports the same live collection of descendant HTML anchors and areas with href.
 */
int
bind_document_links(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Borrowing the getter never permits an Element to masquerade as a Document. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A Document owns its SameObject cache without retaining unrelated query lists. */
	document = (struct dom_document *)node;
	if (document->links_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_LINKS, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		document->links_collection = wrapper;
	}

	/* Succeeded: the current links remain observable through this saved wrapper. */
	*result = vm_value_cell(document->links_collection);
	return 0;
}

/*
 * Reports the same live collection of descendant HTML forms.
 */
int
bind_document_forms(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Borrowing the getter never permits an Element to masquerade as a Document. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A Document owns its SameObject cache without retaining unrelated query lists. */
	document = (struct dom_document *)node;
	if (document->forms_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_FORMS, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		document->forms_collection = wrapper;
	}

	/* Succeeded: the current forms remain observable through this saved wrapper. */
	*result = vm_value_cell(document->forms_collection);
	return 0;
}

/*
 * Reports the same live collection of descendant HTML img elements.
 */
int
bind_document_images(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Borrowing the getter never permits an Element to masquerade as a Document. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A Document owns its SameObject cache without retaining unrelated query lists. */
	document = (struct dom_document *)node;
	if (document->images_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_IMAGES, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		document->images_collection = wrapper;
	}

	/* Succeeded: the current images remain observable through this saved wrapper. */
	*result = vm_value_cell(document->images_collection);
	return 0;
}

/*
 * Reports the same live collection of descendant HTML script elements.
 */
int
bind_document_scripts(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct vm_object *wrapper;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Borrowing the getter never permits an Element to masquerade as a Document. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A Document owns its SameObject cache without retaining unrelated query lists. */
	document = (struct dom_document *)node;
	if (document->scripts_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_SCRIPTS, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		document->scripts_collection = wrapper;
	}

	/* Succeeded: the current scripts remain observable through this saved wrapper. */
	*result = vm_value_cell(document->scripts_collection);
	return 0;
}

/*
 * Returns the select's traced SameObject live native options collection.
 */
int
bind_select_options(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	struct vm_object *wrapper;
	int actual;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Actual namespace and local case establish the getter brand before cache access. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	actual = collection_table_html(node, DOM_TAG_SELECT);
	if (!actual) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* The validated select owns the cache, independently of its current wrapper prototype. */
	element = (struct dom_element *)node;

	/* Publish only a fully constructed native wrapper into the root's traced cache. */
	if (element->options_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_OPTIONS, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		element->options_collection = wrapper;
	}

	/* Succeeded: subsequent reads preserve wrapper identity while membership stays live. */
	*result = vm_value_cell(element->options_collection);
	return 0;
}

/*
 * Creates a genuine immutable-empty NodeList core in the owning Document's realm.
 */
int
bind_empty_node_list(
	struct bind_window *window,
	vm_value *result)
{
	struct vm_object *wrapper;
	int status;

	/* The native backing keeps the relevant owner and never traverses later DOM changes. */
	status = collection_create(window->realm, &window->document->node, COLLECTION_EMPTY_NODE_LIST, NULL, &wrapper);
	if (status != 0)
		return status;

	/* Succeeded: prototype, brand, indexed access and item belong to native NodeList storage. */
	*result = vm_value_cell(wrapper);
	return 0;
}

/* Marks the root even when no external variable retains a Node or Document wrapper. */
static void
collection_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct collection_state *state;

	/* Its Document edge also retains managed owners and XML prototype snapshots. */
	state = (struct collection_state *)cell;
	vm_heap_mark(heap, &state->root->cell);

	/* Only duplicate lists retain a string filter in addition to their owning form. */
	if (state->name != NULL)
		vm_heap_mark(heap, &state->name->cell);

	/* Succeeded: every collection-owned DOM edge is traced. */
	return;
}

/* Builds the wrapper in its root's relevant realm rather than a borrowed getter's realm. */
static int
collection_create(
	struct vm_realm *realm,
	struct dom_node *root,
	enum collection_kind kind,
	struct vm_string *name,
	struct vm_object **out)
{
	struct dom_document *document;
	struct vm_heap *heap;
	struct bind_window *window;
	struct collection_state *state;
	struct vm_object *prototype;
	struct vm_object *wrapper;
	struct vm_cell *roots[3];
	unsigned registered;
	unsigned slot;
	vm_value snapshot;
	int index;
	int status;

	/* Native construction owns its root, captured name and unpublished state until wrapper publication. */
	heap = root->document->heap;
	roots[0] = &root->cell;
	roots[1] = NULL;
	if (name != NULL)
		roots[1] = &name->cell;
	roots[2] = NULL;
	registered = 0;
	status = 0;
	for (slot = 0; slot < 3U; slot++) {
		status = vm_heap_add_root(heap, &roots[slot]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The root's owner supplies the specific interface for each native collection kind. */
	index = BIND_HTML_COLLECTION;
	if (kind == COLLECTION_CONTROLS)
		index = BIND_HTML_FORM_CONTROLS_COLLECTION;
	if (kind == COLLECTION_RADIO)
		index = BIND_RADIO_NODE_LIST;

	/* Attribute mutations own empty NodeLists with the genuine base prototype. */
	if (kind == COLLECTION_EMPTY_NODE_LIST)
		index = BIND_NODE_LIST;

	/* Option collections retain their existing native interface. */
	if (kind == COLLECTION_OPTIONS)
		index = BIND_HTML_OPTIONS_COLLECTION;

	/* XML nodes retain the original interface prototypes independently of any live view. */
	document = root->document;
	if (document->binding_prototypes != NULL) {
		status = vm_object_get(document->binding_prototypes, vm_value_int32(index), &snapshot);
		if (status != 0)
			goto cleanup;
		if (snapshot == VM_VALUE_UNDEFINED) {
			status = EINVAL;
			goto cleanup;
		}

		/* The checked owner snapshot supplies the exact relevant interface prototype. */
		prototype = (struct vm_object *)vm_value_as_cell(snapshot);
	} else {
		/* Parser Documents retain an actual binding owner while their graph is usable. */
		window = document->view;
		if (window == NULL && realm != NULL)
			window = bind_window_of(realm);

		/* An embedding with neither view nor supplied realm cannot create a wrapper. */
		if (window == NULL) {
			status = EINVAL;
			goto cleanup;
		}

		/* A real binding owner supplies this interface without observing script properties. */
		prototype = window->prototypes[index];
	}

	/* Allocate the independent state while its owner and captured name remain rooted. */
	state = vm_heap_alloc(document->heap, &collection_type, sizeof(*state));
	if (state == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Initialize the complete state before rooting it across wrapper allocation. */
	state->root = root;
	state->kind = kind;
	state->name = name;
	roots[2] = &state->cell;

	/* A normal object trace preserves its internal cell and relevant prototype. */
	wrapper = vm_object_create(document->heap, prototype);
	if (wrapper == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publish the complete internal edge before releasing native construction ownership. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);
	wrapper->native_operations = &collection_native;

	/* Return storage receives only a completely constructed genuine wrapper. */
	*out = wrapper;

cleanup:
	/* No completed or failed constructor leaves a root pointing into its returned C stack. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Unpublished construction failures never supply a usable output wrapper. */
	if (status != 0)
		return status;

	/* Succeeded: only the complete wrapper can pass the native collection brand check. */
	return 0;
}

/* Rejects prototype lookalikes and every other kind of platform object. */
static int
collection_this(
	struct vm_realm *realm,
	vm_value receiver,
	int list,
	struct collection_state **out)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int valid;
	int status;

	/* Check the actual object before inspecting its private native cell. */
	valid = vm_value_is_object(receiver);
	if (!valid) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* Only a genuine native operation table permits internal-cell inspection. */
	object = (struct vm_object *)vm_value_as_cell(receiver);
	if (object->native_operations != &collection_native) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* The private cell type independently confirms the collection brand. */
	valid = vm_value_is_cell(object->internal);
	if (!valid) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: an absent native private cell is rejected by a real exception. */
		return VM_THROWN;
	}

	/* Inspect only a verified native cell before comparing its private type. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &collection_type) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* NodeList and HTMLCollection are independent brands despite shared storage helpers. */
	if (list) {
		/* NodeList operations accept both duplicate live lists and immutable empty lists. */
		if (((struct collection_state *)cell)->kind != COLLECTION_RADIO &&
		    ((struct collection_state *)cell)->kind != COLLECTION_EMPTY_NODE_LIST) {
			status = bind_throw_illegal(realm);
			if (status != VM_THROWN)
				return status;

			/* Succeeded: the requested native exception is installed. */
			return VM_THROWN;
		}

		/* Radio-only accessors reject a genuine base NodeList as well as lookalikes. */
		if (list == 2 && ((struct collection_state *)cell)->kind != COLLECTION_RADIO) {
			status = bind_throw_illegal(realm);
			if (status != VM_THROWN)
				return status;

			/* Succeeded: the distinct RadioNodeList brand is enforced. */
			return VM_THROWN;
		}
	} else {
		/* HTMLCollection methods cannot read either genuine NodeList kind. */
		if (((struct collection_state *)cell)->kind == COLLECTION_RADIO ||
		    ((struct collection_state *)cell)->kind == COLLECTION_EMPTY_NODE_LIST) {
			status = bind_throw_illegal(realm);
			if (status != VM_THROWN)
				return status;

			/* Succeeded: the requested native exception is installed. */
			return VM_THROWN;
		}
	}

	/* Succeeded: native methods may read this genuine collection state. */
	*out = (struct collection_state *)cell;
	return 0;
}

/* Advances through current sibling links or descendant preorder without entering templates. */
static struct dom_node *
collection_next(
	struct collection_state *state,
	struct dom_node *node)
{
	struct dom_node *root;

	/* A static empty NodeList never gains members when its owner Document changes. */
	if (state->kind == COLLECTION_EMPTY_NODE_LIST)
		return NULL;

	/* Native options use the same current membership as typed numeric addition. */
	if (state->kind == COLLECTION_OPTIONS) {
		node = dom_select_option_next(state->root, node);

		/* Succeeded: the pure iterator supplied the next eligible member or exhaustion. */
		return node;
	}

	/* Table rows follow logical group order rather than an unrestricted descendant traversal. */
	if (state->kind == COLLECTION_TABLE_ROWS) {
		node = collection_table_next(state, node);

		/* Succeeded: the pure native walker reports the next current row or exhaustion. */
		return node;
	}

	/* Owner-based lists include outside controls in the form's current tree root. */
	root = collection_root(state);
	if (node == NULL) {
		if (state->kind == COLLECTION_CONTROLS || state->kind == COLLECTION_RADIO)
			return root;
		return root->first_child;
	}

	/* Children never includes descendants below a direct child. */
	if (state->kind == COLLECTION_CHILDREN ||
	    state->kind == COLLECTION_TABLE_BODIES ||
	    state->kind == COLLECTION_SECTION_ROWS ||
	    state->kind == COLLECTION_ROW_CELLS)
		return node->next;
	if (node->first_child != NULL)
		return node->first_child;

	/* Climb until a following subtree appears, stopping at the actual root. */
	while (node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}

	/* Succeeded: traversal is exhausted within this rooted collection. */
	return NULL;
}

/* Classifies all canonical uint32 indices even when the VM stores larger ones as strings. */
static int
collection_key_index(
	vm_value key,
	uint32_t *index)
{
	struct vm_string *string;
	uint64_t number;
	size_t offset;
	uint16_t unit;
	int indexed;
	int is_string;

	/* Common indices use the VM's already normalized nonnegative int32 keys. */
	indexed = vm_value_is_array_index(key, index);
	if (indexed)
		return 1;
	is_string = vm_value_is_string(key);
	if (!is_string)
		return 0;
	string = (struct vm_string *)vm_value_as_cell(key);
	if (string->length == 0 || string->length > 10U)
		return 0;

	/* Leading zeroes are names, while the single zero is a valid canonical index. */
	unit = vm_string_at(string, 0);
	if (string->length > 1U && unit == '0')
		return 0;

	/* Ten decimal digits fit the wide accumulator before checking the array-index ceiling. */
	number = 0;
	for (offset = 0; offset < string->length; offset++) {
		unit = vm_string_at(string, offset);
		if (unit < '0' || unit > '9')
			return 0;
		number = number * 10U + (uint64_t)(unit - '0');
	}

	/* UINT32_MAX itself is a supported name candidate rather than an array index. */
	if (number >= UINT32_MAX)
		return 0;

	/* Succeeded: the complete unsigned index has no signed-int32 alias. */
	*index = (uint32_t)number;
	return 1;
}

/* Selects elements without conflating XML case or foreign namespace identities. */
static int
collection_matches(
	struct collection_state *state,
	struct dom_node *node)
{
	struct dom_element *element;
	struct vm_string *href;
	struct dom_element *owner;
	int same;
	int member;

	/* Text, comments and doctypes cannot occur in an HTMLCollection. */
	if (node->type != DOM_ELEMENT)
		return 0;
	if (state->kind == COLLECTION_CHILDREN)
		return 1;

	/* Ordinary live form ownership can include controls outside the form subtree. */
	element = (struct dom_element *)node;
	if (state->kind == COLLECTION_CONTROLS || state->kind == COLLECTION_RADIO) {
		member = dom_form_control_member(element);
		if (!member)
			return 0;
		owner = dom_form_owner(element);
		if (owner != (struct dom_element *)state->root)
			return 0;

		/* Duplicate lists apply their captured exact name to current associated members. */
		if (state->kind == COLLECTION_RADIO) {
			same = collection_name_matches(node, state->name);
			if (!same)
				return 0;
		}

		/* Succeeded: this current member belongs to this form and optional name filter. */
		return 1;
	}

	/* The pure option iterator already excluded every foreign or blocked descendant. */
	if (state->kind == COLLECTION_OPTIONS)
		return 1;

	/* Local names are case-sensitive in XML even though the internal tag enum folds them. */
	element = (struct dom_element *)node;
	if (element->ns != DOM_NS_HTML)
		return 0;

	/* HTML forms do not require a control, name, id or particular content attribute. */
	if (state->kind == COLLECTION_FORMS) {
		same = vm_string_equal_ascii(element->local_name, "form");
		if (!same)
			return 0;

		/* Succeeded: this descendant is an HTML namespace form with exact local case. */
		return 1;
	}

	/* Image membership depends on exact native namespace and local name, independently of attributes. */
	if (state->kind == COLLECTION_IMAGES) {
		same = vm_string_equal_ascii(element->local_name, "img");
		if (!same)
			return 0;

		/* Succeeded: this current descendant is an actual HTML namespace img element. */
		return 1;
	}

	/* Script membership, like images, depends only on the HTML namespace and the exact local name. */
	if (state->kind == COLLECTION_SCRIPTS) {
		same = vm_string_equal_ascii(element->local_name, "script");
		if (!same)
			return 0;

		/* Succeeded: this current descendant is an actual HTML namespace script element. */
		return 1;
	}

	/* Table-family lists reuse native legacy properties with tightly scoped actual membership. */
	if (state->kind == COLLECTION_TABLE_BODIES) {
		same = collection_table_html(node, DOM_TAG_TBODY);
		if (!same)
			return 0;

		/* Succeeded: this actual native candidate satisfies the requested membership. */
		return 1;
	}

	/* The ordered iterator supplies only direct actual table rows from eligible sections. */
	if (state->kind == COLLECTION_TABLE_ROWS || state->kind == COLLECTION_SECTION_ROWS) {
		same = collection_table_html(node, DOM_TAG_TR);
		if (!same)
			return 0;

		/* Succeeded: this actual native candidate satisfies the requested membership. */
		return 1;
	}

	/* A row's direct cells can be td or th, but no other direct child or descendant. */
	if (state->kind == COLLECTION_ROW_CELLS) {
		same = collection_table_html(node, DOM_TAG_TD);
		if (same)
			return 1;
		same = collection_table_html(node, DOM_TAG_TH);
		if (!same)
			return 0;

		/* Succeeded: this actual native candidate satisfies the requested membership. */
		return 1;
	}

	/* Links independently require exact anchor or area local names. */
	same = vm_string_equal_ascii(element->local_name, "a");
	if (!same) {
		same = vm_string_equal_ascii(element->local_name, "area");
		if (!same)
			return 0;
	}

	/* Attribute presence, including an empty value, establishes link membership. */
	href = dom_attribute_ascii(element, "href");
	if (href == NULL)
		return 0;

	/* Succeeded: this HTML anchor or area currently carries a href attribute. */
	return 1;
}

/* Finds an indexed member without allocating or caching a snapshot of its tree. */
static struct dom_node *
collection_index(
	struct collection_state *state,
	uint32_t index)
{
	struct dom_node *node;
	int matches;

	/* Count only current members in their actual tree order. */
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			if (index == 0U)
				return node;
			index--;
		}

		/* Continue through the current rooted links after this candidate. */
		node = collection_next(state, node);
	}

	/* Succeeded: no indexed member exists, permitting ordinary property fallback. */
	return NULL;
}

/* Chooses the first id or HTML name match in tree order, without global id priority. */
static struct dom_node *
collection_named(
	struct collection_state *state,
	struct vm_string *name)
{
	struct dom_node *node;
	struct dom_element *element;
	struct vm_string *attribute;
	int matches;
	int same;

	/* An empty key never matches an empty id or name. */
	if (name->length == 0)
		return NULL;

	/* Attribute mutation is observed each time, including changes on detached roots. */
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			element = (struct dom_element *)node;
			attribute = dom_attribute_ascii(element, "id");
			if (attribute != NULL) {
				same = vm_string_equal(attribute, name);
				if (same)
					return node;
			}

			/* Only HTML namespace elements expose name attributes as member names. */
			if (element->ns == DOM_NS_HTML) {
				attribute = dom_attribute_ascii(element, "name");
				if (attribute != NULL) {
					same = vm_string_equal(attribute, name);
					if (same)
						return node;
				}
			}
		}

		/* Continue through the current rooted links after this candidate. */
		node = collection_next(state, node);
	}

	/* Succeeded: no current member carries this exact nonempty key. */
	return NULL;
}

/* Real own expandos and prototype properties hide legacy names without invoking getters. */
static int
collection_visible(
	struct vm_object *object,
	vm_value key)
{
	struct vm_object *prototype;
	struct vm_property property;
	int found;

	/* This explicit ordinary bypass avoids recursively dispatching this collection. */
	found = vm_object_get_own_ordinary(object, key, &property);
	if (found != 0)
		return 0;

	/* An own property anywhere on the prototype chain wins over a legacy member name. */
	for (prototype = object->prototype;
	     prototype != NULL;
	     prototype = prototype->prototype) {
		found = vm_object_get_own(prototype, key, &property);
		if (found < 0)
			return found;
		if (found != 0)
			return 0;
	}

	/* Succeeded: absent ordinary properties allow the legacy name to become visible. */
	return 1;
}

/* Supplies caller-owned readonly virtual data records for current indices and visible names. */
static int
collection_get_own(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	struct collection_state *state;
	struct dom_node *node;
	uint32_t index;
	uint32_t attributes;
	int is_index;
	int is_string;
	int visible;
	int status;

	/* Supported indices take precedence; missing numeric indices never fall through to names. */
	state = (struct collection_state *)vm_value_as_cell(object->internal);
	is_index = collection_key_index(key, &index);
	attributes = VM_PROPERTY_CONFIGURABLE;
	if (is_index) {
		node = collection_index(state, index);
		attributes |= VM_PROPERTY_ENUMERABLE;
	} else {
		/* Duplicate lists expose indices only, leaving every nonindex key ordinary. */
		if (state->kind == COLLECTION_RADIO)
			return 0;

		/* Symbols are ordinary expandos rather than legacy member names. */
		is_string = vm_value_is_string(key);
		if (!is_string)
			return 0;
		node = collection_named(state, (struct vm_string *)vm_value_as_cell(key));
		if (node == NULL)
			return 0;
		visible = collection_visible(object, key);
		if (visible <= 0)
			return visible;
	}

	/* Returning missing lets the VM inspect its actual ordinary own storage. */
	if (node == NULL)
		return 0;

	/* Node wrappers resolve the actual owner or private XML snapshot without a raw realm. */
	if (!is_index && state->kind == COLLECTION_CONTROLS) {
		status = collection_named_value(state, (struct vm_string *)vm_value_as_cell(key), &property->temporary);
	} else {
		status = bind_wrap(NULL, node, &property->temporary);
	}

	/* A failed member or duplicate wrapper never publishes a partial descriptor. */
	if (status != 0)
		return -status;
	property->holder = object;
	property->value = &property->temporary;
	property->attributes = attributes;

	/* Succeeded: this descriptor owns its temporary value until the caller consumes it. */
	return 1;
}

/* Appends a visible supported name; final native/ordinary key merging removes duplicates. */
static int
collection_name_key(
	struct vm_heap *heap,
	struct vm_object *object,
	struct vm_string *name,
	struct wb_vector *keys)
{
	vm_value key;
	int visible;
	int status;

	/* No empty attribute creates a legacy supported property name. */
	if (name == NULL || name->length == 0)
		return 0;
	status = vm_key_from_string(heap, name, &key);
	if (status != 0)
		return status;
	visible = collection_visible(object, key);
	if (visible < 0)
		return -visible;
	if (!visible)
		return 0;

	/* VM merging preserves the first appearance of each normalized key. */
	status = wb_vector_push(keys, &key);
	if (status != 0)
		return status;

	/* Succeeded: this visible name contributes to the ordered native key prefix. */
	return 0;
}

/* Enumerates current numeric indices before tree-ordered nonenumerable member names. */
static int
collection_own_keys(
	struct vm_heap *heap,
	struct vm_object *object,
	struct wb_vector *keys)
{
	struct collection_state *state;
	struct dom_node *node;
	struct dom_element *element;
	struct vm_string *name;
	vm_value key;
	uint32_t index;
	char text[11];
	int matches;
	int printed;
	int status;

	/* Decimal keys preserve full uint32 index identity instead of signed-int32 aliases. */
	state = (struct collection_state *)vm_value_as_cell(object->internal);
	index = 0;
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			if (index == UINT32_MAX)
				return EOVERFLOW;
			printed = snprintf(text, sizeof(text), "%u", (unsigned)index);
			if (printed < 0 || (size_t)printed >= sizeof(text))
				return EINVAL;

			/* Normalize common keys exactly as script access does, preserving native deduplication. */
			if (index <= INT32_MAX) {
				key = vm_value_int32((int32_t)index);
			} else {
				key = vm_key_from_ascii(heap, text);
				if (key == VM_VALUE_EMPTY)
					return ENOMEM;
			}

			/* The key prefix observes the same normalized identity as property lookup. */
			status = wb_vector_push(keys, &key);
			if (status != 0)
				return status;
			index++;
		}

		/* Continue through the current rooted links after this candidate. */
		node = collection_next(state, node);
	}

	/* NodeList has no legacy supported property names. */
	if (state->kind == COLLECTION_RADIO)
		return 0;

	/* Each current member contributes its id, followed by its HTML namespace name. */
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			element = (struct dom_element *)node;
			name = dom_attribute_ascii(element, "id");
			status = collection_name_key(heap, object, name, keys);
			if (status != 0)
				return status;

			/* Foreign names never contribute, although foreign ids still do. */
			if (element->ns == DOM_NS_HTML) {
				name = dom_attribute_ascii(element, "name");
				status = collection_name_key(heap, object, name, keys);
				if (status != 0)
					return status;
			}
		}

		/* Continue through the current rooted links after this candidate. */
		node = collection_next(state, node);
	}

	/* Succeeded: ordinary strings and symbols are appended by the VM afterward. */
	return 0;
}

/* Refuses indexed setters and unshadowed supported names, leaving ordinary expandos intact. */
static int
collection_define(
	struct vm_realm *realm,
	struct vm_object *object,
	vm_value key,
	const struct vm_descriptor *descriptor,
	int *handled,
	int *done)
{
	struct collection_state *state;
	struct dom_node *node;
	struct vm_property property;
	uint32_t index;
	int is_index;
	int is_string;
	int found;

	UNUSED_PARAMETER(realm);
	UNUSED_PARAMETER(descriptor);

	/* Every canonical numeric index refuses definition, even beyond the current list. */
	*handled = 0;
	*done = 0;
	is_index = collection_key_index(key, &index);
	if (is_index) {
		*handled = 1;
		return 0;
	}

	/* An ordinary own expando remains independent when DOM changes later add the name. */
	is_string = vm_value_is_string(key);
	if (!is_string)
		return 0;
	found = vm_object_get_own_ordinary(object, key, &property);
	if (found != 0)
		return 0;

	/* NodeList permits ordinary string expandos even when they match member ids or names. */
	state = (struct collection_state *)vm_value_as_cell(object->internal);
	if (state->kind == COLLECTION_RADIO)
		return 0;

	/* A supported member name has no named setter even when a prototype hides it. */
	node = collection_named(state, (struct vm_string *)vm_value_as_cell(key));
	if (node != NULL)
		*handled = 1;

	/* Succeeded: the VM can distinguish native refusal from ordinary fallback. */
	return 0;
}

/* Virtual configurable descriptors still have no legacy indexed or named deleter. */
static int
collection_delete(
	struct vm_heap *heap,
	struct vm_object *object,
	vm_value key,
	int *handled,
	int *deleted)
{
	struct collection_state *state;
	struct vm_property property;
	struct dom_node *node;
	uint32_t index;
	int is_index;
	int found;

	UNUSED_PARAMETER(heap);

	/* Missing numeric indices succeed, and currently supported numeric indices stay. */
	*handled = 1;
	*deleted = 1;
	state = (struct collection_state *)vm_value_as_cell(object->internal);
	is_index = collection_key_index(key, &index);
	if (is_index) {
		node = collection_index(state, index);
		if (node != NULL)
			*deleted = 0;
		return 0;
	}

	/* Only a currently visible virtual named descriptor refuses deletion. */
	found = collection_get_own(object, key, &property);
	if (found < 0)
		return -found;
	if (found != 0) {
		*deleted = 0;
		return 0;
	}

	/* A hidden or absent name leaves deletion to ordinary own storage. */
	*handled = 0;

	/* Succeeded: hidden names and ordinary properties use ordinary deletion rules. */
	return 0;
}

/* Legacy platform objects cannot be sealed while their membership remains live. */
static int
collection_prevent_extensions(
	struct vm_object *object,
	int *allowed)
{
	UNUSED_PARAMETER(object);

	/* Refusal precedes every integrity operation's flag or descriptor mutation. */
	*allowed = 0;

	/* Succeeded: the VM observes the specified refusal without altering the object. */
	return 0;
}

/* Counts members from the current tree rather than a stored array length. */
static int
collection_length(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The HTMLCollection brand excludes duplicate NodeList wrappers. */
	status = collection_this(realm, receiver, 0, &state);
	if (status != 0)
		return status;

	/* Count current members without a cached snapshot. */
	status = collection_read_length(state, result);
	if (status != 0)
		return status;

	/* Succeeded: this collection reports its live unsigned length. */
	return 0;
}

/* Counts native members independently of the public interface brand. */
static int
collection_read_length(
	struct collection_state *state,
	vm_value *result)
{
	struct dom_node *node;
	uint32_t length;
	int matches;

	/* Overflow is an error rather than an unsigned wrap into an empty collection. */
	length = 0;
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			if (length == UINT32_MAX)
				return EOVERFLOW;
			length++;
		}

		/* Continue through the current rooted links after this candidate. */
		node = collection_next(state, node);
	}

	/* Succeeded: the full unsigned member count is returned without int32 truncation. */
	*result = vm_value_number((double)length);
	return 0;
}

/* Applies one unsigned conversion after branding, then observes the current indexed member. */
static int
collection_item(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	int status;

	/* Native receiver identity is established before user argument conversion. */
	status = collection_this(realm, receiver, 0, &state);
	if (status != 0)
		return status;

	/* The common indexed algorithm converts once and then reads current members. */
	status = collection_read_item(realm, state, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: this HTMLCollection returned its current member or null. */
	return 0;
}

/* Applies one unsigned conversion after branding and then observes live membership. */
static int
collection_read_item(
	struct vm_realm *realm,
	struct collection_state *state,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	uint32_t index;
	struct vm_cell *state_root;
	int status;

	/* A required argument is checked before invoking its conversion. */
	if (count == 0) {
		status = vm_throw_type_error(realm, "HTMLCollection.item requires an index.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* The native callee retains genuine state through user conversion and result publication. */
	state_root = &state->cell;
	status = vm_heap_add_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Conversion may mutate membership and therefore precedes the live search. */
	status = vm_to_uint32(realm, args[0], &index);
	if (status != 0)
		goto cleanup;

	/* Missing members are null through this method, unlike missing own properties. */
	node = collection_index(state, index);
	if (node == NULL) {
		*result = VM_VALUE_NULL;
	} else {
		/* Resolve the member in its actual owning Document prototype graph. */
		status = bind_wrap(NULL, node, result);
		if (status != 0)
			goto cleanup;
	}

cleanup:
	/* Conversion errors, missing members and complete publication release the same native ownership. */
	vm_heap_remove_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Succeeded: conversion-time DOM changes are reflected in the returned Node. */
	return 0;
}

/* Applies one string conversion after branding, then finds the current first named member. */
static int
collection_named_item(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	struct dom_node *node;
	struct vm_string *name;
	struct vm_cell *state_root;
	int status;

	/* Actual native branding precedes a potentially side-effecting DOMString conversion. */
	status = collection_this(realm, receiver, 0, &state);
	if (status != 0)
		return status;
	if (count == 0) {
		status = vm_throw_type_error(realm, "HTMLCollection.namedItem requires a name.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* The native callee retains genuine state through user conversion and result publication. */
	state_root = &state->cell;
	status = vm_heap_add_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Conversion may mutate names and therefore precedes the live search. */
	status = vm_to_string(realm, args[0], &name);
	if (status != 0)
		goto cleanup;

	/* No global id priority overrides an earlier matching HTML name attribute. */
	node = collection_named(state, name);
	if (node == NULL) {
		*result = VM_VALUE_NULL;
	} else {
		/* Resolve the member in its actual owning Document prototype graph. */
		status = bind_wrap(NULL, node, result);
		if (status != 0)
			goto cleanup;
	}

cleanup:
	/* Conversion errors, missing members and complete publication release the same native ownership. */
	vm_heap_remove_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Succeeded: this first current member carries the exact requested id or HTML name. */
	return 0;
}

/* Owner-based membership follows the form after it moves to a different tree root. */
static struct dom_node *
collection_root(
	struct collection_state *state)
{
	struct dom_node *root;

	/* Ordinary descendant collections keep their original explicit root. */
	root = state->root;
	if (state->kind != COLLECTION_CONTROLS && state->kind != COLLECTION_RADIO)
		return root;

	/* Connected external controls and detached subtree controls share this actual root. */
	while (root->parent != NULL)
		root = root->parent;

	/* Succeeded: membership is bounded by the form's current tree. */
	return root;
}

/* Counts a member once even when both its id and its name match the same key. */
static int
collection_name_matches(
	struct dom_node *node,
	struct vm_string *name)
{
	struct dom_element *element;
	struct vm_string *attribute;
	int same;

	/* The empty string never denotes a supported member name. */
	if (name->length == 0)
		return 0;

	/* Actual members are elements; no-namespace ids take part independently of names. */
	element = (struct dom_element *)node;
	attribute = dom_attribute_ascii(element, "id");
	if (attribute != NULL) {
		same = vm_string_equal(attribute, name);
		if (same)
			return 1;
	}

	/* Only actual HTML members expose name content attributes as collection names. */
	if (element->ns != DOM_NS_HTML)
		return 0;
	attribute = dom_attribute_ascii(element, "name");
	if (attribute == NULL)
		return 0;
	same = vm_string_equal(attribute, name);
	if (!same)
		return 0;

	/* Succeeded: this one member matches at least one of its two attributes. */
	return 1;
}

/* Resolves a controls name to null, one member, or a new live duplicate list. */
static int
collection_named_value(
	struct collection_state *state,
	struct vm_string *name,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_node *first;
	struct vm_object *wrapper;
	int matches;
	int status;

	/* Stop at the second distinct matching element without double-counting attributes. */
	first = NULL;
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches)
			matches = collection_name_matches(node, name);
		if (matches) {
			if (first != NULL) {
				/* The list captures the name, retaining no snapshot of its current members. */
				status = collection_create(NULL, state->root, COLLECTION_RADIO, name, &wrapper);
				if (status != 0)
					return status;
				*result = vm_value_cell(wrapper);
				return 0;
			}

			/* Keep the first member until absence of a second match is established. */
			first = node;
		}

		/* Continue through live membership in current tree order. */
		node = collection_next(state, node);
	}

	/* Methods distinguish an empty match from a missing virtual named property. */
	if (first == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* A single match resolves through its actual Document's cached Node wrapper. */
	status = bind_wrap(NULL, first, result);
	if (status != 0)
		return status;

	/* Succeeded: exactly one current associated member has this name. */
	return 0;
}

/* Overrides HTMLCollection's first-match method with controls' duplicate-list result. */
static int
controls_named_item(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	struct vm_string *name;
	struct vm_cell *state_root;
	int status;

	/* The more specific brand precedes argument count and every user conversion. */
	status = collection_this(realm, receiver, 0, &state);
	if (status != 0)
		return status;
	if (state->kind != COLLECTION_CONTROLS) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A required DOMString must be supplied before conversion can run. */
	if (count == 0) {
		status = vm_throw_type_error(realm, "HTMLFormControlsCollection.namedItem requires a name.");
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* The native callee retains genuine state through user conversion and result publication. */
	state_root = &state->cell;
	status = vm_heap_add_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Conversion-time DOM changes are observed by the subsequent current-member search. */
	status = vm_to_string(realm, args[0], &name);
	if (status != 0)
		goto cleanup;
	status = collection_named_value(state, name, result);
	if (status != 0)
		goto cleanup;

cleanup:
	/* Conversion errors, missing members and complete publication release the same native ownership. */
	vm_heap_remove_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Succeeded: this lookup returned the current single, duplicate or empty result. */
	return 0;
}

/* Reports the live length of a genuine duplicate NodeList, rejecting HTMLCollections. */
static int
node_list_length(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A NodeList prototype lookalike cannot access native collection storage. */
	status = collection_this(realm, receiver, 1, &state);
	if (status != 0)
		return status;
	status = collection_read_length(state, result);
	if (status != 0)
		return status;

	/* Succeeded: even a saved list with zero or one remaining member stays live. */
	return 0;
}

/* Reuses indexed conversion while preserving NodeList's independent native brand. */
static int
node_list_item(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	int status;

	/* Rejecting a borrowed receiver precedes the caller's conversion side effects. */
	status = collection_this(realm, receiver, 1, &state);
	if (status != 0)
		return status;
	status = collection_read_item(realm, state, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: this duplicate list returned its current member or null. */
	return 0;
}

/* Publishes a SameObject controls wrapper only for an actual HTML form element. */
static int
form_elements(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	struct vm_object *wrapper;
	int form;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Namespace and local case establish this interface independently of prototypes. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* A foreign or uppercase XML form cannot borrow this branded getter. */
	element = (struct dom_element *)node;
	form = vm_string_equal_ascii(element->local_name, "form");
	if (element->ns != DOM_NS_HTML || !form) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the requested native exception is installed. */
		return VM_THROWN;
	}

	/* Allocate a complete native wrapper before publishing the form's traced cache. */
	if (element->controls_collection == NULL) {
		status = collection_create(realm, node, COLLECTION_CONTROLS, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		element->controls_collection = wrapper;
	}

	/* Succeeded: subsequent observations of this form return the same live collection. */
	*result = vm_value_cell(element->controls_collection);
	return 0;
}

/* Uses the same membership as elements while preserving the form getter's brand. */
static int
form_length(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_object *wrapper;
	struct collection_state *state;
	vm_value collection;
	int status;

	/* The actual form getter checks namespace, local name and complete cache identity. */
	status = form_elements(realm, receiver, args, count, &collection);
	if (status != 0)
		return status;
	wrapper = (struct vm_object *)vm_value_as_cell(collection);
	state = (struct collection_state *)vm_value_as_cell(wrapper->internal);

	/* Count directly without reading a script-shadowed length or invoking user getters. */
	status = collection_read_length(state, result);
	if (status != 0)
		return status;

	/* Succeeded: form.length uses the same current controls as the native elements getter. */
	return 0;
}

/* Finds the first current checked actual radio and exposes its raw value or on default. */
static int
radio_value_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	struct dom_node *node;
	struct dom_element *element;
	struct vm_string *value;
	int matches;
	int radio;
	int checked;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The indexed NodeList brand currently denotes genuine RadioNodeList state. */
	status = collection_this(realm, receiver, 2, &state);
	if (status != 0)
		return status;

	/* Current member order and actual DOM state determine the first checked radio. */
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			element = (struct dom_element *)node;
			radio = dom_input_is_radio(element);
			if (radio) {
				checked = dom_control_checked(element);
				if (checked) {
					value = dom_attribute_ascii(element, "value");
					if (value != NULL) {
						*result = vm_value_cell(value);
						return 0;
					}

					/* An absent value differs from a present empty value. */
					status = bind_string(realm, "on", result);
					if (status != 0)
						return status;
					return 0;
				}
			}
		}

		/* Continue through current associated controls rather than a saved member snapshot. */
		node = collection_next(state, node);
	}

	/* A list with no checked actual radio exposes the empty string. */
	status = bind_string(realm, "", result);
	if (status != 0)
		return status;

	/* Succeeded: no current checked radio contributes a list value. */
	return 0;
}

/* Checks the first current actual radio whose content value matches one converted string. */
static int
radio_value_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct collection_state *state;
	struct dom_node *node;
	struct dom_element *element;
	struct vm_string *text;
	vm_value argument;
	struct vm_string *value;
	int matches;
	int radio;
	int same;
	struct vm_cell *state_root;
	int status;

	/* Native identity precedes DOMString conversion, which may mutate the entire live list. */
	*result = VM_VALUE_UNDEFINED;
	status = collection_this(realm, receiver, 2, &state);
	if (status != 0)
		return status;

	/* The native callee retains genuine state through user conversion and result publication. */
	state_root = &state->cell;
	status = vm_heap_add_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Convert once while preserving the callee-owned live collection state. */
	argument = js_argument(args, count, 0);
	status = bind_to_string(realm, argument, &text);
	if (status != 0)
		goto cleanup;

	/* The first matching actual radio wins even if another member has the same value. */
	node = collection_next(state, NULL);
	while (node != NULL) {
		matches = collection_matches(state, node);
		if (matches) {
			element = (struct dom_element *)node;
			radio = dom_input_is_radio(element);
			if (radio) {
				value = dom_attribute_ascii(element, "value");
				if (value == NULL) {
					same = vm_string_equal_ascii(text, "on");
				} else {
					same = vm_string_equal(text, value);
				}

				/* Direct checkedness assignment preserves the input's independent dirty flag. */
				if (same) {
					status = dom_input_set_checked(element, 1, 0);
					if (status != 0)
						goto cleanup;
					goto cleanup;
				}
			}
		}

		/* A conversion-time rename, removal or owner change is observed during this search. */
		node = collection_next(state, node);
	}

cleanup:
	/* Conversion errors, missing members and complete publication release the same native ownership. */
	vm_heap_remove_root(realm->heap, &state_root);
	if (status != 0)
		return status;

	/* Succeeded: an unmatched string leaves every current checkedness state unchanged. */
	return 0;
}

/* Recognizes exact actual HTML table-family nodes independently of folded tag numbers. */
static int
collection_table_html(
	const struct dom_node *node,
	int tag)
{
	const struct dom_element *element;
	const char *name;
	int same;

	/* Text, absent nodes and foreign namespaces cannot supply native table members. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;
	element = (const struct dom_element *)node;
	if (element->ns != DOM_NS_HTML || element->tag != tag)
		return 0;
	name = dom_tag_name(tag);
	same = vm_string_equal_ascii(element->local_name, name);
	if (!same)
		return 0;

	/* Succeeded: this actual native candidate satisfies the requested membership. */
	return 1;
}

/* Classifies actual sections by the order in which their direct rows occur in table.rows. */
static int
collection_section_rank(
	const struct dom_node *node)
{
	int section;

	/* Every direct thead contributes before ordinary body and direct table rows. */
	section = collection_table_html(node, DOM_TAG_THEAD);
	if (section)
		return 1;
	section = collection_table_html(node, DOM_TAG_TBODY);
	if (section)
		return 2;
	section = collection_table_html(node, DOM_TAG_TFOOT);
	if (section)
		return 3;

	/* Succeeded: an unrelated, foreign or uppercase XML section contributes no table rows. */
	return 0;
}

/* Walks eligible direct rows in thead, mixed body/direct and tfoot groups without snapshots. */
static struct dom_node *
collection_table_next(
	struct collection_state *state,
	struct dom_node *node)
{
	struct dom_node *root;
	struct dom_node *child;
	struct dom_node *row;
	struct dom_node *parent;
	int rank;
	int section;
	int actual;

	/* The first observation begins at the table's current first child in the header group. */
	root = state->root;
	child = root->first_child;
	rank = 1;

	/* A preceding eligible row fixes the current group and the next direct table child. */
	if (node != NULL) {
		parent = node->parent;
		if (parent == root) {
			/* Direct table rows share the middle group's actual table-child order. */
			rank = 2;
			child = node->next;
		} else {
			/* Remaining direct rows in this section precede the next eligible section. */
			for (row = node->next;
			     row != NULL;
			     row = row->next) {
				actual = collection_table_html(row, DOM_TAG_TR);
				if (actual)
					return row;
			}

			/* The completed actual section determines the group without storing traversal state. */
			rank = collection_section_rank(parent);
			child = parent->next;
		}
	}

	/* Each group scans only direct table children; nested or wrapped rows never participate. */
	for (;
	     rank <= 3;
	     rank++) {
		/* In-group table child order interleaves direct tr and tbody rows only for the middle group. */
		while (child != NULL) {
			if (rank == 2) {
				actual = collection_table_html(child, DOM_TAG_TR);
				if (actual)
					return child;
			}

			/* A matching actual section contributes only its direct actual tr children. */
			section = collection_section_rank(child);
			if (section == rank) {
				for (row = child->first_child;
				     row != NULL;
				     row = row->next) {
					actual = collection_table_html(row, DOM_TAG_TR);
					if (actual)
						return row;
				}
			}

			/* Empty sections, non-elements and unrelated children consume no row index. */
			child = child->next;
		}

		/* The next group restarts from the current table child list in actual tree order. */
		child = root->first_child;
	}

	/* Succeeded: all header, body/direct and footer rows in this current table are exhausted. */
	return NULL;
}

/* Publishes a complete native wrapper into the correctly branded root's separate cache. */
static int
collection_table_get(
	struct vm_realm *realm,
	vm_value receiver,
	enum collection_kind kind,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_element *element;
	struct vm_object **cache;
	struct vm_object *wrapper;
	int actual;
	int status;

	/* A receiver's actual DOM identity is checked before any cache or collection is observed. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the required illegal-receiver exception is installed. */
		return VM_THROWN;
	}

	/* Only the checked Element can own a specific table-family cache. */
	element = (struct dom_element *)node;

	/* Each selected collection kind has one exact root brand and one independently traced cache. */
	switch (kind) {
	case COLLECTION_TABLE_BODIES:
		actual = collection_table_html(node, DOM_TAG_TABLE);
		cache = &element->bodies_collection;
		break;
	case COLLECTION_TABLE_ROWS:
		actual = collection_table_html(node, DOM_TAG_TABLE);
		cache = &element->rows_collection;
		break;
	case COLLECTION_SECTION_ROWS:
		actual = collection_section_rank(node);
		cache = &element->rows_collection;
		break;
	case COLLECTION_ROW_CELLS:
		actual = collection_table_html(node, DOM_TAG_TR);
		cache = &element->cells_collection;
		break;
	default:
		return EINVAL;
	}

	/* Foreign elements and prototype forgeries cannot allocate a native table collection. */
	if (!actual) {
		status = bind_throw_illegal(realm);
		if (status != VM_THROWN)
			return status;

		/* Succeeded: the required illegal-receiver exception is installed. */
		return VM_THROWN;
	}

	/* Construct a wrapper only after the exact root brand and cache were selected. */
	if (*cache == NULL) {
		status = collection_create(realm, node, kind, NULL, &wrapper);
		if (status != 0)
			return status;

		/* Publish only the successfully constructed complete wrapper into its traced cache. */
		*cache = wrapper;
	}

	/* Succeeded: the cached wrapper observes current links in its root's relevant realm. */
	*result = vm_value_cell(*cache);
	return 0;
}

/* Returns this actual table's SameObject direct tbody collection. */
static int
table_bodies(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared cache path rejects another root brand before constructing the live collection. */
	status = collection_table_get(realm, receiver, COLLECTION_TABLE_BODIES, result);
	if (status != 0)
		return status;

	/* Succeeded: the correctly branded root supplies its complete SameObject live collection. */
	return 0;
}

/* Returns this actual table's SameObject logically ordered row collection. */
static int
table_rows(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared cache path rejects another root brand before constructing the live collection. */
	status = collection_table_get(realm, receiver, COLLECTION_TABLE_ROWS, result);
	if (status != 0)
		return status;

	/* Succeeded: the correctly branded root supplies its complete SameObject live collection. */
	return 0;
}

/* Returns this actual section's SameObject direct tr collection. */
static int
section_rows(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared cache path rejects another root brand before constructing the live collection. */
	status = collection_table_get(realm, receiver, COLLECTION_SECTION_ROWS, result);
	if (status != 0)
		return status;

	/* Succeeded: the correctly branded root supplies its complete SameObject live collection. */
	return 0;
}

/* Returns this actual row's SameObject direct td/th collection. */
static int
row_cells(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The shared cache path rejects another root brand before constructing the live collection. */
	status = collection_table_get(realm, receiver, COLLECTION_ROW_CELLS, result);
	if (status != 0)
		return status;

	/* Succeeded: the correctly branded root supplies its complete SameObject live collection. */
	return 0;
}
