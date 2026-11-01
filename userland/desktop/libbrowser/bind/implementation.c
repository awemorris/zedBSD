/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * DOMImplementation associates creation with a Document without confusing its
 * platform brand with a Node. Script-created Documents keep traced prototype
 * snapshots rather than raw pointers to an explicitly retired Window.
 */

#include "bind/internal.h"

#include <errno.h>

/* One collectible implementation retains its associated Document and realm prototypes. */
struct implementation_state {
	struct vm_cell cell;
	struct dom_document *document;
	struct vm_object *prototypes;
};

static void implementation_trace(struct vm_heap *heap, struct vm_cell *cell);
static int implementation_document(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int implementation_html_document(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int implementation_doctype(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* Implementation cells trace their associated graph until the last wrapper is dropped. */
static const struct vm_cell_type implementation_type = {
    "dom-implementation", implementation_trace, NULL};

/* The immutable operation table describes only the implemented creation foundation. */
static const struct bind_operation implementation_operations[] = {
    {"createDocument", 2, implementation_document},
    {"createHTMLDocument", 0, implementation_html_document},
    {"createDocumentType", 3, implementation_doctype},
    {NULL, 0, NULL}};

/* The interface object has no public constructor and cannot impersonate a Node. */
const struct bind_interface bind_dom_implementation_interface = {
    "DOMImplementation", BIND_NO_PARENT, 0, NULL, NULL, implementation_operations, NULL};

static int implementation_this(struct vm_realm *realm, vm_value this_value, struct implementation_state **state);
static int implementation_snapshot(struct vm_realm *realm, struct dom_document *document, struct vm_object **snapshot);

/*
 * Returns the actual Document's cached implementation with its own prototype graph.
 */
int
bind_document_implementation(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_node *node;
	struct dom_document *document;
	struct implementation_state *state;
	struct vm_object *prototypes;
	struct vm_object *wrapper;
	struct vm_cell *roots[2];
	vm_value prototype;
	unsigned index;
	unsigned registered;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Branding precedes allocation and prevents borrowed non-Document access. */
	status = bind_this_node(realm, this_value, &node);
	if (status != 0)
		return status;

	/* A real Element or DocumentType still cannot stand for a Document. */
	if (node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* SameObject caching preserves user expandos and repeated getter identity. */
	document = (struct dom_document *)node;
	if (document->implementation != NULL) {
		*result = vm_value_cell(document->implementation);
		return 0;
	}

	/* Builds a private prototype snapshot before publishing a cached wrapper. */
	status = implementation_snapshot(realm, document, &prototypes);
	if (status != 0)
		return status;

	/* Keep the unpublished snapshot and native brand live during wrapper construction. */
	roots[0] = &prototypes->cell;
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(document->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Allocates the independent native brand before creating its script wrapper. */
	state = vm_heap_alloc(document->heap, &implementation_type, sizeof(*state));
	if (state == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The private brand is unpublished until its script wrapper is complete. */
	roots[1] = &state->cell;

	/* Keeps the associated Document and prototype graph reachable through the brand. */
	state->document = document;
	state->prototypes = prototypes;

	/* Reads the receiver Document's own DOMImplementation prototype. */
	status = vm_object_get(prototypes, vm_value_int32(BIND_DOM_IMPLEMENTATION), &prototype);
	if (status != 0)
		goto cleanup;

	/* Gives the wrapper the receiver Document's own platform prototype. */
	wrapper = vm_object_create(document->heap, (struct vm_object *)vm_value_as_cell(prototype));
	if (wrapper == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publishes the complete graph only after all allocations succeed. */
	wrapper->kind = VM_KIND_PLATFORM;
	wrapper->internal = vm_value_cell(state);
	document->implementation = wrapper;
	*result = vm_value_cell(wrapper);

cleanup:
	/* The published Document now traces the wrapper and all private state. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(document->heap, &roots[registered]);
	}

	/* Failed construction leaves no cached implementation object. */
	if (status != 0)
		return status;

	/* Succeeded: the Document's stable implementation object is available. */
	return 0;
}

/* Retains the associated Document and every prototype needed by its factories. */
static void
implementation_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct implementation_state *state;

	/* The brand's graph contains no manual Window pointer or permanent root. */
	state = (struct implementation_state *)cell;
	vm_heap_mark(heap, &state->document->node.cell);
	vm_heap_mark(heap, &state->prototypes->cell);

	/* Succeeded: both retained graphs participate in this collection. */
	return;
}

/* Requires the native implementation brand before converting caller arguments. */
static int
implementation_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct implementation_state **state)
{
	struct vm_object *wrapper;
	struct vm_cell *cell;
	int valid;
	int status;

	/* Primitive values cannot carry the private platform brand. */
	valid = vm_value_is_object(this_value);
	if (!valid) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Prototype inheritance alone supplies no native state. */
	wrapper = (struct vm_object *)vm_value_as_cell(this_value);
	valid = vm_value_is_cell(wrapper->internal);
	if (wrapper->kind != VM_KIND_PLATFORM || !valid) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Node cells and other platform objects are distinct from implementations. */
	cell = vm_value_as_cell(wrapper->internal);
	if (cell->type != &implementation_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Publishes the validated native brand without converting caller arguments. */
	*state = (struct implementation_state *)cell;

	/* Succeeded: the native implementation supplies its associated Document. */
	return 0;
}

/* Snapshots prototypes as private data properties, without mutable global lookups. */
static int
implementation_snapshot(
	struct vm_realm *realm,
	struct dom_document *document,
	struct vm_object **snapshot)
{
	struct bind_window *window;
	struct vm_object *made;
	struct vm_cell *root;
	unsigned index;
	int written;
	int status;

	/* A factory-created Document already has an immutable internal realm snapshot. */
	if (document->binding_prototypes != NULL) {
		*snapshot = document->binding_prototypes;
		return 0;
	}

	/* An ordinary Document uses its actual owning view, even through a borrowed getter. */
	window = document->view;
	if (window == NULL)
		window = bind_window_of(realm);

	/* Allocates an unexposed holder for the originating realm's prototypes. */
	made = vm_object_create(document->heap, NULL);
	if (made == NULL)
		return ENOMEM;
	root = &made->cell;
	status = vm_heap_add_root(document->heap, &root);
	if (status != 0)
		return status;

	/* Each independent data property retains an existing realm prototype. */
	for (index = 0; index < BIND_INTERFACES; index++) {
		status = vm_object_set(document->heap, made, vm_value_int32((int32_t)index), vm_value_cell(window->prototypes[index]), &written);
		if (status != 0)
			goto cleanup;

		/* A fresh private object must accept every prototype entry. */
		if (!written) {
			status = EINVAL;
			goto cleanup;
		}
	}

	/* Lends the completed snapshot to the implementation's traced state. */
	*snapshot = made;

cleanup:
	/* The caller roots the returned snapshot before another VM allocation. */
	vm_heap_remove_root(document->heap, &root);
	if (status != 0)
		return status;

	/* Succeeded: no script can mutate the snapshot object through the public API. */
	return 0;
}

/* Creates a detached XML Document, adopting a supplied doctype only after validation. */
static int
implementation_document(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct implementation_state *state;
	struct dom_document *document;
	struct dom_element *element;
	struct dom_node *doctype;
	struct vm_string *uri;
	struct vm_string *qualified;
	struct vm_cell *roots[3];
	vm_value given;
	unsigned index;
	unsigned registered;
	int matches;
	int status;

	/* The receiver's native brand determines factory prototype identity. */
	status = implementation_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* Namespace and qualified name are required, even for an empty Document. */
	if (count < 2) {
		status = vm_throw_type_error(realm, "createDocument requires two arguments.");
		return status;
	}

	/* Converted input and the unpublished Document stay live across later allocations. */
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

	/* Converts a nullable namespace once, before the qualified name. */
	uri = NULL;
	given = args[0];
	if (given != VM_VALUE_NULL && given != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, given, &uri);
		if (status != 0)
			goto cleanup;

		/* An empty namespace is normalized to no namespace. */
		if (uri->length == 0)
			uri = NULL;
		if (uri != NULL)
			roots[0] = &uri->cell;
	}

	/* LegacyNullToEmptyString preserves null's historical empty-root meaning. */
	given = args[1];
	if (given == VM_VALUE_NULL) {
		qualified = vm_atom_from_ascii(realm->heap, "");
		if (qualified == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
	} else {
		status = bind_to_atom(realm, given, 0, &qualified);
		if (status != 0)
			goto cleanup;
	}

	/* Nullable DocumentType conversion rejects nodes of any other native kind. */
	doctype = NULL;
	given = js_argument(args, count, 2);
	if (given != VM_VALUE_NULL && given != VM_VALUE_UNDEFINED) {
		doctype = bind_node_of(given);
		if (doctype == NULL || doctype->type != DOM_DOCUMENT_TYPE) {
			status = vm_throw_type_error(realm, "The doctype is not a DocumentType.");
			goto cleanup;
		}
	}

	/* A detached Document has no view, but its prototypes retain its relevant realm. */
	document = dom_document_create(state->document->heap);
	if (document == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Protect the new Document until its wrapper and root are installed. */
	roots[1] = &document->node.cell;
	document->binding_prototypes = state->prototypes;
	document->context = state->document->context;
	document->content = DOM_CONTENT_XML;

	/* Only the exact HTML/SVG namespace identities select their XML MIME types. */
	if (uri != NULL) {
		matches = vm_string_equal_ascii(uri, "http://www.w3.org/1999/xhtml");
		if (matches) {
			document->content = DOM_CONTENT_XHTML;
		} else {
			matches = vm_string_equal_ascii(uri, "http://www.w3.org/2000/svg");
			if (matches)
				document->content = DOM_CONTENT_SVG;
		}
	}

	/* Empty names leave the Document empty without running namespace extraction. */
	element = NULL;
	if (qualified->length != 0) {
		status = bind_create_element_ns(realm, document, uri, qualified, &element);
		if (status != 0)
			goto cleanup;
		roots[2] = &element->node.cell;
	}

	/* All fallible result allocation precedes detachment of an existing doctype. */
	status = bind_wrap(bind_window_of(realm), &document->node, result);
	if (status != 0)
		goto cleanup;

	/* Moving the only supplied doctype retains wrapper identity and updates ownership. */
	if (doctype != NULL) {
		dom_remove(doctype);
		doctype->document = document;
		dom_append_child(&document->node, doctype);
	}

	/* The already-created root follows the doctype in the new document tree. */
	if (element != NULL)
		dom_append_child(&document->node, &element->node);

cleanup:
	/* The published wrapper or DOM tree now owns each completed cell. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Failed construction leaves the caller's result unpublished. */
	if (status != 0)
		return status;

	/* Succeeded: the detached XML Document is fully assembled. */
	return 0;
}

/* Appends a new HTML element of a name to a parent node, reporting it (NULL when the heap is exhausted). */
static struct dom_element *
implementation_html_child(
	struct dom_document *document,
	struct dom_node *parent,
	const char *name)
{
	struct vm_string *local;
	struct dom_element *element;

	local = vm_atom_from_ascii(document->heap, name);
	if (local == NULL)
		return NULL;
	element = dom_element_create(document, DOM_NS_HTML, local, NULL);
	if (element == NULL)
		return NULL;
	dom_append_child(parent, &element->node);
	return element;
}

/* Creates a detached HTML Document: a doctype, html, head, the title when one is given, and body. */
static int
implementation_html_document(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct implementation_state *state;
	struct dom_document *document;
	struct dom_element *root;
	struct dom_element *head;
	struct dom_element *title;
	struct dom_node *doctype;
	struct dom_node *text;
	struct vm_string *name;
	struct vm_string *empty;
	struct vm_string *given;
	struct vm_cell *roots[3];
	struct wb_units units;
	unsigned index;
	unsigned registered;
	int status;

	/* The receiver's native brand determines factory prototype identity. */
	status = implementation_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* The unpublished Document and the converted title stay live across later allocations. */
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

	/* The title is converted first, when there is one (undefined means none). */
	given = NULL;
	if (count > 0 && args[0] != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, args[0], &given);
		if (status != 0)
			goto cleanup;
		roots[0] = &given->cell;
	}

	/* A detached HTML Document has no view, but its prototypes retain its relevant realm. */
	document = dom_document_create(state->document->heap);
	if (document == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	roots[1] = &document->node.cell;
	document->binding_prototypes = state->prototypes;
	document->context = state->document->context;

	/* The doctype, then the root with its head (and title) and body. */
	name = vm_atom_from_ascii(document->heap, "html");
	empty = vm_atom_from_ascii(document->heap, "");
	if (name == NULL || empty == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	doctype = dom_doctype_create(document, name, empty, empty);
	if (doctype == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	dom_append_child(&document->node, doctype);
	root = implementation_html_child(document, &document->node, "html");
	if (root == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	head = implementation_html_child(document, &root->node, "head");
	if (head == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	if (given != NULL) {
		title = implementation_html_child(document, &head->node, "title");
		if (title == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
		wb_units_init(&units);
		status = vm_string_append_units(given, &units);
		if (status != 0) {
			wb_units_release(&units);
			goto cleanup;
		}
		text = dom_text_create(document, units.data, units.length);
		wb_units_release(&units);
		if (text == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
		dom_append_child(&title->node, text);
	}
	if (implementation_html_child(document, &root->node, "body") == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publishes the Document's wrapper. */
	status = bind_wrap(bind_window_of(realm), &document->node, result);

cleanup:
	/* The published wrapper or DOM tree now owns each completed cell. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	return status;
}

/* Creates a branded DocumentType with once-only ordered DOMString conversions. */
static int
implementation_doctype(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct implementation_state *state;
	struct dom_node *doctype;
	struct vm_string *name;
	struct vm_string *public_id;
	struct vm_string *system_id;
	struct vm_cell *roots[3];
	size_t index;
	unsigned slot;
	unsigned registered;
	uint16_t unit;
	int status;

	/* Native branding takes precedence over argument conversion. */
	status = implementation_this(realm, this_value, &state);
	if (status != 0)
		return status;

	/* All three IDL strings are required. */
	if (count < 3) {
		status = vm_throw_type_error(realm, "createDocumentType requires three arguments.");
		return status;
	}

	/* Each converted string survives the next conversion and native allocation. */
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	for (slot = 0; slot < 3U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Converts the name before either identifier can run script conversion. */
	status = bind_to_string(realm, args[0], &name);
	if (status != 0)
		goto cleanup;
	roots[0] = &name->cell;

	/* Converts the public identifier before evaluating the system identifier. */
	status = bind_to_string(realm, args[1], &public_id);
	if (status != 0)
		goto cleanup;
	roots[1] = &public_id->cell;

	/* Converts the system identifier only after both preceding conversions succeed. */
	status = bind_to_string(realm, args[2], &system_id);
	if (status != 0)
		goto cleanup;
	roots[2] = &system_id->cell;

	/* Current DOM doctype syntax allows empty names but forbids these code units. */
	for (index = 0; index < name->length; index++) {
		unit = vm_string_at(name, index);
		if (unit == 0 ||
		    unit == '\t' ||
		    unit == '\n' ||
		    unit == '\f' ||
		    unit == '\r' ||
		    unit == ' ' ||
		    unit == '>') {
			status = bind_throw_dom(realm, "InvalidCharacterError", "The doctype name is not valid.");
			goto cleanup;
		}
	}

	/* Native node allocation retains the associated Document and all converted strings. */
	doctype = dom_doctype_create(state->document, name, public_id, system_id);
	if (doctype == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publishes the receiver Document's ordinary DocumentType wrapper. */
	status = bind_wrap(bind_window_of(realm), doctype, result);
	if (status != 0)
		goto cleanup;

cleanup:
	/* The published DocumentType wrapper traces the three converted strings. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A conversion or wrapper error leaves no completed DocumentType result. */
	if (status != 0)
		return status;

	/* Succeeded: the new detached DocumentType is available. */
	return 0;
}
