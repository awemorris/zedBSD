/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The nodes of the DOM: making them, linking them into a tree and the
 * collector's view of them (what each refers to, and what each owns).
 */

#include "dom/dom.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void node_trace(struct vm_heap *heap, struct vm_cell *cell);
static void document_finalize(struct vm_heap *heap, struct vm_cell *cell);
static void element_trace(struct vm_heap *heap, struct vm_cell *cell);
static void element_finalize(struct vm_heap *heap, struct vm_cell *cell);
static void character_data_finalize(struct vm_heap *heap, struct vm_cell *cell);
static void character_data_trace(struct vm_heap *heap, struct vm_cell *cell);
static void doctype_trace(struct vm_heap *heap, struct vm_cell *cell);

/* A document, which refers to its children like any node. */
static const struct vm_cell_type document_type = { "document", node_trace, document_finalize };

/* A document fragment (a template's contents, a fragment being parsed). */
static const struct vm_cell_type fragment_type = { "document-fragment", node_trace, NULL };

/* An element, which also refers to its names, attributes and template contents. */
static const struct vm_cell_type element_type = { "element", element_trace, element_finalize };

/* A native character node owns its C data buffer and traces its optional PI target. */
static const struct vm_cell_type character_data_type = { "character-data", character_data_trace, character_data_finalize };

/* A DOCTYPE node, which also refers to its strings. */
static const struct vm_cell_type doctype_type = { "document-type", doctype_trace, NULL };

static void node_style_reset(struct dom_node *node);
static void node_style_reset_tree(struct dom_node *root);
static struct dom_node *node_alloc(struct dom_document *document, const struct vm_cell_type *type, size_t size, int node_type);
static struct dom_node *character_data_create(struct dom_document *document, int node_type, const uint16_t *units, size_t length);
static int node_set_attribute(struct dom_element *element, struct vm_string *name, struct vm_string *value, int *committed);
static int node_attribute_notify(struct dom_element *element, struct vm_string *name, struct vm_string *old_value);

/*
 * Makes an empty document in a heap, in no-quirks mode.
 */
struct dom_document *
dom_document_create(
	struct vm_heap *heap)
{
	struct dom_document *document;

	/* Allocates the document's cell. */
	document = vm_heap_alloc(heap, &document_type, sizeof(*document));
	if (document == NULL)
		return NULL;

	/* The document is its own document. */
	document->node.type = DOM_DOCUMENT;
	document->node.document = document;
	document->heap = heap;
	document->quirks = DOM_NO_QUIRKS;
	document->content = DOM_CONTENT_HTML;

	/* A binding owner installs mutation notification only after attaching its Window. */
	document->attribute_changed = NULL;

	/* Succeeded: the document is empty. */
	return document;
}

/*
 * Makes an element of a namespace with a local name (an atom) and, for
 * foreign content, a prefix.
 */
struct dom_element *
dom_element_create(
	struct dom_document *document,
	int ns,
	struct vm_string *local_name,
	struct vm_string *prefix)
{
	struct dom_element *element;
	struct wb_units lower;
	uint16_t unit;
	size_t index;
	int error;

	/* Allocates the element's cell. */
	element = (struct dom_element *)node_alloc(document, &element_type, sizeof(*element), DOM_ELEMENT);
	if (element == NULL)
		return NULL;

	/* Records the names, and when the element was made. */
	element->local_name = local_name;
	element->prefix = prefix;
	element->ns = (uint16_t)ns;
	element->created = document->generation;

	/* Finds the tag number from the name folded to lower case (SVG names keep their case). */
	wb_units_init(&lower);
	error = wb_units_reserve(&lower, local_name->length);
	if (error == 0) {
		for (index = 0; index < local_name->length; index++) {
			unit = vm_string_at(local_name, index);
			if (unit >= 'A' && unit <= 'Z')
				unit = (uint16_t)(unit + 0x20U);
			lower.data[index] = unit;
		}

		/* The folded name is complete; its tag number is looked up. */
		lower.length = local_name->length;
		element->tag = (uint16_t)dom_tag_lookup(lower.data, lower.length);
	}

	/* Frees the folded copy. */
	wb_units_release(&lower);

	/* Succeeded: the element is detached and has no attributes. */
	return element;
}

/*
 * Makes a text node holding length units.
 */
struct dom_node *
dom_text_create(
	struct dom_document *document,
	const uint16_t *units,
	size_t length)
{
	struct dom_node *node;

	/* Makes a character data node of the text kind. */
	node = character_data_create(document, DOM_TEXT, units, length);
	if (node == NULL)
		return NULL;

	/* Succeeded: the text is detached. */
	return node;
}

/*
 * Makes a comment node holding length units.
 */
struct dom_node *
dom_comment_create(
	struct dom_document *document,
	const uint16_t *units,
	size_t length)
{
	struct dom_node *node;

	/* Makes a character data node of the comment kind. */
	node = character_data_create(document, DOM_COMMENT, units, length);
	if (node == NULL)
		return NULL;

	/* Succeeded: the comment is detached. */
	return node;
}

/*
 * Makes a DOCTYPE node.
 */
struct dom_node *
dom_doctype_create(
	struct dom_document *document,
	struct vm_string *name,
	struct vm_string *public_id,
	struct vm_string *system_id)
{
	struct dom_doctype *doctype;
	struct vm_cell *roots[3];
	unsigned index;
	unsigned registered;
	int error;

	/* A collector during node allocation must retain all supplied identifiers. */
	roots[0] = &name->cell;
	roots[1] = &public_id->cell;
	roots[2] = &system_id->cell;
	doctype = NULL;
	registered = 0;
	error = 0;
	for (index = 0; index < 3U; index++) {
		error = vm_heap_add_root(document->heap, &roots[index]);
		if (error != 0)
			goto cleanup;
		registered++;
	}

	/* Allocates the node's cell. */
	doctype = (struct dom_doctype *)node_alloc(document, &doctype_type, sizeof(*doctype), DOM_DOCUMENT_TYPE);
	if (doctype == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Records the name and the identifiers. */
	doctype->name = name;
	doctype->public_id = public_id;
	doctype->system_id = system_id;

cleanup:
	/* The completed node traces its identifiers after these temporary roots leave. */
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(document->heap, &roots[registered]);
	}

	/* A failed root registration or node allocation publishes no DocumentType. */
	if (error != 0)
		return NULL;

	/* Succeeded: the DOCTYPE is detached. */
	return &doctype->node;
}

/*
 * Makes an empty document fragment.
 */
struct dom_node *
dom_fragment_create(
	struct dom_document *document)
{
	struct dom_node *fragment;

	/* Allocates the fragment's cell. */
	fragment = node_alloc(document, &fragment_type, sizeof(*fragment), DOM_DOCUMENT_FRAGMENT);
	if (fragment == NULL)
		return NULL;

	/* Succeeded: the fragment is empty. */
	return fragment;
}

/*
 * Appends a node as the last child of parent, taking it from where it was.
 */
void
dom_append_child(
	struct dom_node *parent,
	struct dom_node *child)
{
	/* Inserting before nothing appends. */
	dom_insert_before(parent, child, NULL);
}

/*
 * Inserts a node into parent before reference (at the end when reference
 * is NULL), taking it from where it was.
 */
void
dom_insert_before(
	struct dom_node *parent,
	struct dom_node *child,
	struct dom_node *reference)
{
	/* A node that has a parent leaves it first. */
	if (child->parent != NULL)
		dom_remove(child);

	/* Links the node between reference's previous sibling and reference. */
	child->parent = parent;
	child->next = reference;
	if (reference != NULL) {
		child->previous = reference->previous;
		reference->previous = child;
	} else {
		child->previous = parent->last_child;
		parent->last_child = child;
	}

	/* The node before it now leads to it, or it is the first child. */
	if (child->previous != NULL) {
		child->previous->next = child;
	} else {
		parent->first_child = child;
	}

	/* A child-list change or new connection retires current inline source identities. */
	node_style_reset(parent);
	node_style_reset_tree(child);

	/* Repair live boundaries before any option, layout or host observer sees the insertion. */
	dom_insertion_notify(parent->document, child);

	/* Reconcile option owners after the complete incoming subtree has its final links. */
	dom_select_tree_changed(child);

	/* The tree changed: the document's style and layout are out of date. */
	parent->document->generation++;
}

/*
 * Takes a node out of its parent (nothing happens when it has none).
 */
void
dom_remove(
	struct dom_node *child)
{
	struct dom_node *parent;

	/* A detached node stays as it is. */
	parent = child->parent;
	if (parent == NULL)
		return;

	/* Repairs weak traversal positions while every original DOM link remains available. */
	dom_removal_notify(child->document, child);

	/* Retires any child browsing contexts before their connection disappears. */
	if (child->document->removed != NULL)
		child->document->removed(child->document, child);

	/* Joins its neighbours, or moves the parent's ends past it. */
	if (child->previous != NULL) {
		child->previous->next = child->next;
	} else {
		parent->first_child = child->next;
	}

	/* The node after it now follows the node before it, or the node before it is the last child. */
	if (child->next != NULL) {
		child->next->previous = child->previous;
	} else {
		parent->last_child = child->previous;
	}

	/* The node is detached. */
	child->parent = NULL;
	child->previous = NULL;
	child->next = NULL;

	/* Removed styles and their old parent's child-list changes retire current source identities. */
	node_style_reset(parent);
	node_style_reset_tree(child);

	/* Repair old and new option owners after the complete removed subtree is detached. */
	dom_select_tree_changed(child);

	/* The tree changed: the document's style and layout are out of date. */
	parent->document->generation++;
}

/*
 * Appends characters to a text or comment node.
 */
int
dom_text_append(
	struct dom_node *node,
	const uint16_t *units,
	size_t length)
{
	struct dom_character_data *text;
	size_t previous_length;
	int error;

	/* Appends to the node's buffer. */
	text = (struct dom_character_data *)node;
	previous_length = text->data.length;
	error = wb_units_append(&text->data, units, length);
	if (error != 0)
		return error;

	/* Appending preserves equal endpoints while shifting only points beyond the replaced interval. */
	dom_data_notify(node->document, node, previous_length, 0, length);

	/* Text changes retire the current source association and invalidate document layout. */
	if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
		node_style_reset(node->parent);
	node->document->generation++;

	/* Succeeded: the node holds the characters. */
	return 0;
}

/*
 * Adds an attribute to an element (the caller checks it is not a
 * duplicate).
 */
int
dom_element_add_attribute(
	struct dom_element *element,
	int ns,
	struct vm_string *prefix,
	struct vm_string *name,
	struct vm_string *value)
{
	struct dom_attribute *attributes;
	struct dom_attribute *attribute;
	size_t capacity;
	int is_style;

	/* Grows the array when it is full. */
	if (element->attribute_count == element->attribute_capacity) {
		if (element->attribute_capacity > SIZE_MAX / 2U)
			return ENOMEM;
		capacity = element->attribute_capacity * 2U;
		if (capacity < 4U)
			capacity = 4U;
		if (capacity > SIZE_MAX / sizeof(*attributes))
			return ENOMEM;

		/* Moves the attributes to larger storage. */
		attributes = realloc(element->attributes, capacity * sizeof(*attributes));
		if (attributes == NULL)
			return ENOMEM;

		/* Publishes the larger array. */
		element->attributes = attributes;
		element->attribute_capacity = capacity;
	}

	/* Fills the next place. */
	attribute = &element->attributes[element->attribute_count];
	attribute->name = name;
	attribute->prefix = prefix;
	attribute->value = value;
	attribute->namespace_uri = NULL;
	attribute->ns = ns;
	element->attribute_count++;

	/* No-namespace additions outside a CSSOM update replace any prior precise inline model. */
	if (ns == DOM_NS_NONE &&
	    !element->inline_style_updating) {
		is_style = vm_string_equal_ascii(name, "style");
		if (is_style)
			element->inline_style_source = NULL;
	}

	/* Only no-namespace content attributes affect native select selectedness. */
	if (ns == DOM_NS_NONE)
		dom_select_attribute_changed(element, name, 0, 1);

	/* The attributes changed: the document's style is out of date. */
	element->node.document->generation++;

	/* Succeeded: the attribute is the element's last. */
	return 0;
}

/*
 * Finds an element's attribute by namespace and local name (an atom).
 */
struct dom_attribute *
dom_element_find_attribute(
	const struct dom_element *element,
	int ns,
	const struct vm_string *name)
{
	size_t index;

	/* Arbitrary URI identity cannot be recovered from the shared foreign classification alone. */
	if (ns == DOM_NS_OTHER)
		return NULL;

	/* Compares each attribute's name and namespace. */
	for (index = 0; index < element->attribute_count; index++) {
		/* Atoms compare by pointer. */
		if (element->attributes[index].name == name && element->attributes[index].ns == ns)
			return &element->attributes[index];
	}

	/* The element has no such attribute. */
	return NULL;
}

/*
 * Tells whether a node is an element of a namespace with a tag.
 */
int
dom_element_is(
	const struct dom_node *node,
	int ns,
	int tag)
{
	const struct dom_element *element;

	/* Only elements have tags. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return 0;

	/* Compares the namespace and the tag. */
	element = (const struct dom_element *)node;
	if (element->ns != ns || element->tag != tag)
		return 0;

	/* The element matches. */
	return 1;
}


/*
 * Tells whether a cell is a node of the DOM (of any kind).
 */
int
dom_is_node(
	const struct vm_cell *cell)
{
	/* The node types are the cell types of this file. */
	if (cell->type == &document_type)
		return 1;
	if (cell->type == &fragment_type)
		return 1;
	if (cell->type == &element_type)
		return 1;
	if (cell->type == &character_data_type)
		return 1;
	if (cell->type == &doctype_type)
		return 1;

	/* Another kind of cell. */
	return 0;
}

/*
 * Sets an element's attribute of no namespace (name is an atom), adding
 * it when the element does not have it.
 */
int
dom_element_set_attribute(
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *value)
{
	int committed;
	int status;

	/* Ordinary attribute assignment shares the same commit and notification path as CSSOM. */
	status = node_set_attribute(element, name, value, &committed);
	if (status != 0)
		return status;

	/* Succeeded: the caller sees the committed ordinary attribute assignment. */
	return 0;
}

/*
 * Commits canonical inline text while preserving the exact specified declaration source.
 * A failed notification keeps a committed model; failure before commit restores its former source.
 */
int
dom_element_set_style_attribute(
	struct dom_element *element,
	struct vm_string *source,
	struct vm_string *value)
{
	struct vm_heap *heap;
	struct vm_string *old_source;
	struct vm_string *name;
	struct vm_cell *roots[5];
	unsigned index;
	unsigned registered;
	int updating;
	int committed;
	int status;

	/* Bare native callers retain the owner and both fresh strings before key creation can collect. */
	heap = element->node.document->heap;
	old_source = element->inline_style_source;
	roots[0] = &element->node.cell;
	roots[1] = &source->cell;
	roots[2] = &value->cell;
	roots[3] = NULL;
	roots[4] = NULL;
	if (old_source != NULL)
		roots[3] = &old_source->cell;
	registered = 0;
	for (index = 0; index < 5U; index++) {
		status = vm_heap_add_root(heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Only successful root registrations belong to cleanup. */
		registered++;
	}

	/* The style key is owned before native mutation can allocate observer records. */
	name = vm_atom_from_ascii(heap, "style");
	if (name == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The source is authoritative before the attribute hook observes the new serialized text. */
	roots[4] = &name->cell;
	updating = element->inline_style_updating;
	element->inline_style_source = source;
	element->inline_style_updating = 1;
	status = node_set_attribute(element, name, value, &committed);
	element->inline_style_updating = updating;

	/* Reentrant committed CSSOM changes own their later source; only an uncommitted failure restores the old model. */
	if (!committed)
		element->inline_style_source = old_source;

cleanup:
	/* No native frame keeps a retired source or a registry slot after return. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(heap, &roots[registered]);
	}

	/* Preserve the actual allocation or post-commit notification error. */
	if (status != 0)
		return status;

	/* Succeeded: precise native declaration state and canonical DOM text were published together. */
	return 0;
}

/*
 * Removes an element's attribute of no namespace (name is an atom);
 * nothing happens when it has none.
 */
int
dom_element_remove_attribute(
	struct dom_element *element,
	struct vm_string *name)
{
	struct dom_attribute *attribute;
	struct vm_string *old_name;
	struct vm_string *old_value;
	size_t index;
	size_t after;
	int error;

	/* Removing an absent attribute has no mutation to report. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL)
		return 0;

	/* Array compaction and select-state updates allocate no VM cells before the hook roots these snapshots. */
	old_name = attribute->name;
	old_value = attribute->value;
	index = (size_t)(attribute - element->attributes);
	after = element->attribute_count - index - 1U;
	memmove(&element->attributes[index], &element->attributes[index + 1U], after * sizeof(*attribute));
	element->attribute_count--;
	dom_select_attribute_changed(element, old_name, 1, 0);
	element->node.document->generation++;

	/* The retired strings are borrowed only until the hook establishes its temporary roots. */
	error = node_attribute_notify(element, old_name, old_value);
	if (error != 0)
		return error;

	/* Succeeded: the attribute is absent and its prior value was reported. */
	return 0;
}

/*
 * Replaces the characters of a text or comment node.
 */
int
dom_text_set(
	struct dom_node *node,
	const uint16_t *units,
	size_t length)
{
	struct dom_character_data *text;
	struct wb_units replacement;
	struct wb_units previous;
	int error;

	/* Prepare an independent buffer so failed or aliased input cannot discard the old characters. */
	text = (struct dom_character_data *)node;
	wb_units_init(&replacement);
	error = wb_units_append(&replacement, units, length);
	if (error != 0) {
		wb_units_release(&replacement);
		return error;
	}

	/* Publish complete characters before pure repair observes the native replacement. */
	previous = text->data;
	text->data = replacement;
	dom_data_notify(node->document, node, 0, previous.length, length);
	wb_units_release(&previous);

	/* Text changes retire the current source association and invalidate document layout. */
	if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
		node_style_reset(node->parent);
	node->document->generation++;

	/* Succeeded: the node holds the new text. */
	return 0;
}

/*
 * Deletes a CharacterData suffix without allocating or discarding the retained prefix.
 */
int
dom_text_truncate(
	struct dom_node *node,
	size_t length)
{
	struct dom_character_data *text;
	size_t previous_length;
	int character_data;

	/* Invalid native types cannot expose a CharacterData buffer. */
	character_data = dom_is_character_data(node);
	if (!character_data)
		return EINVAL;

	/* A suffix deletion cannot extend the old data or publish an invalid length. */
	text = (struct dom_character_data *)node;
	previous_length = text->data.length;
	if (length > previous_length)
		return EINVAL;

	/* Commit the retained prefix before native repair observes the deleted interval. */
	text->data.length = length;
	dom_data_notify(node->document, node, length, previous_length - length, 0);

	/* Text data replacement runs the actual parent's style child-change lifecycle. */
	if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
		node_style_reset(node->parent);
	node->document->generation++;

	/* Succeeded: the original buffer retains its prefix and all live offsets are repaired. */
	return 0;
}

/*
 * Replaces a checked native CharacterData interval with complete atomic UTF16 data.
 *
 * Pure deletion retains the original buffer and needs no allocation.
 */
int
dom_text_replace(
	struct dom_node *node,
	size_t offset,
	size_t count,
	const uint16_t *units,
	size_t length)
{
	struct dom_character_data *text;
	struct wb_units replacement;
	struct wb_units previous;
	size_t retained;
	size_t suffix;
	size_t total;
	int character_data;

	/* Invalid native kinds cannot expose or modify CharacterData storage. */
	character_data = dom_is_character_data(node);
	if (!character_data)
		return EINVAL;

	/* Nonempty replacement data must have an actual readable source. */
	if (length != 0 && units == NULL)
		return EINVAL;

	/* Bounds are checked before subtraction or interval clamping can overflow. */
	text = (struct dom_character_data *)node;
	if (offset > text->data.length)
		return EINVAL;

	/* Oversized counts delete only the actual remaining suffix. */
	retained = text->data.length - offset;
	if (count > retained)
		count = retained;
	suffix = retained - count;
	retained = text->data.length - count;

	/* Replacement size overflow cannot read the supplied input or change the old data. */
	if (length > SIZE_MAX - retained)
		return ENOMEM;
	total = retained + length;

	/* Pure deletion cannot fail after native content extraction has moved earlier siblings. */
	if (length == 0) {
		/* Empty suffixes avoid pointer arithmetic on an empty native buffer. */
		if (suffix != 0) {
			memmove(text->data.data + offset,
				text->data.data + offset + count,
				suffix * sizeof(uint16_t));
		}

		/* Complete retained data is visible before weak native endpoint repair. */
		text->data.length = total;
		dom_data_notify(node->document, node, offset, count, 0);

		/* Text data replacement runs the actual parent's style child-change lifecycle. */
		if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
			node_style_reset(node->parent);
		node->document->generation++;

		/* Succeeded: prefix and shifted suffix remain in the original owned buffer. */
		return 0;
	}

	/* Exact byte bounds avoid geometric capacity rounding beyond representable storage. */
	if (total > SIZE_MAX / sizeof(uint16_t))
		return ENOMEM;

	/* Allocate exactly the complete independent buffer before reading any aliased input. */
	wb_units_init(&replacement);
	replacement.data = malloc(total * sizeof(uint16_t));
	if (replacement.data == NULL)
		return ENOMEM;

	/* Exact capacity describes owned storage without an unchecked doubling operation. */
	replacement.capacity = total;

	/* A nonempty prefix belongs to the unchanged original buffer until publication. */
	if (offset != 0)
		memcpy(replacement.data, text->data.data, offset * sizeof(uint16_t));

	/* Replacement units may alias any part of the still-owned original data. */
	memcpy(replacement.data + offset, units, length * sizeof(uint16_t));

	/* Only a nonempty retained suffix needs source or destination pointer arithmetic. */
	if (suffix != 0) {
		memcpy(replacement.data + offset + length,
			text->data.data + offset + count,
			suffix * sizeof(uint16_t));
	}

	/* Publish complete data before optional callbacks inspect repaired native points. */
	replacement.length = total;
	previous = text->data;
	text->data = replacement;
	dom_data_notify(node->document, node, offset, count, length);
	wb_units_release(&previous);

	/* Text data replacement runs the actual parent's style child-change lifecycle. */
	if (node->type == DOM_TEXT || node->type == DOM_CDATA_SECTION)
		node_style_reset(node->parent);
	node->document->generation++;

	/* Succeeded: the new owned buffer contains prefix, replacement and retained suffix. */
	return 0;
}

/*
 * Tells whether ancestor is node or one of node's ancestors.
 */
int
dom_is_inclusive_ancestor(
	const struct dom_node *ancestor,
	const struct dom_node *node)
{
	const struct dom_node *walk;

	/* Walks up from the node to its root. */
	for (walk = node; walk != NULL; walk = walk->parent) {
		if (walk == ancestor)
			return 1;
	}

	/* The ancestor is not on the way up. */
	return 0;
}

/* Releases C registry ownership without accessing already finalized GC subscribers. */
static void
document_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_document *document;

	UNUSED_PARAMETER(heap);

	/* Subscription tokens independently keep their C registry alive until their cleanup. */
	document = (struct dom_document *)cell;
	dom_removal_release(document);

	/* Succeeded: this Document no longer owns a removal registry. */
	return;
}

/* Clears only the opaque current source identity of a genuine HTML style element. */
static void
node_style_reset(
	struct dom_node *node)
{
	struct dom_element *element;

	/* A null parent or non-element cannot own an inline sheet. */
	if (node == NULL || node->type != DOM_ELEMENT)
		return;
	element = (struct dom_element *)node;

	/* DOM owns no CSS model storage and therefore never finalizes a retired binding state. */
	if (element->ns == DOM_NS_HTML && element->tag == DOM_TAG_STYLE)
		element->style_sheet = NULL;

	/* Succeeded: any current inline source can be rebuilt without affecting saved old handles. */
	return;
}

/* Retires style identities across a changed connected subtree without allocation or recursion. */
static void
node_style_reset_tree(
	struct dom_node *root)
{
	struct dom_node *walk;

	/* Visit actual tree children only; template contents and separate child Documents are not children. */
	walk = root;
	while (walk != NULL) {
		node_style_reset(walk);

		/* Descend before following siblings so every nested style observes its connection change. */
		if (walk->first_child != NULL) {
			walk = walk->first_child;
			continue;
		}

		/* Ascend only within this changed subtree when the current branch has no sibling. */
		while (walk != root && walk->next == NULL)
			walk = walk->parent;
		if (walk == root)
			break;
		walk = walk->next;
	}

	/* Succeeded: all actual style descendants lost their previous current association. */
	return;
}

/* Marks the nodes a node is linked to. */
static void
node_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_node *node;

	/* Marks the script's object for the node and its listeners. */
	node = (struct dom_node *)cell;
	if (node->wrapper != NULL)
		vm_heap_mark(heap, &node->wrapper->cell);
	if (node->listeners != NULL)
		vm_heap_mark(heap, node->listeners);

	/* Cached live children must preserve identity even without a saved script list. */
	if (node->children_collection != NULL)
		vm_heap_mark(heap, &node->children_collection->cell);

	/* Marks the document, the parent, the neighbours and the children's ends. */
	if (node->document != NULL) {
		vm_heap_mark(heap, &node->document->node.cell);
		if (node->document->context != NULL)
			vm_heap_mark(heap, node->document->context);
	}

	/* Script-created Documents retain cached binding objects without raw Window owners. */
	if (node->type == DOM_DOCUMENT) {
		/* Loaded metadata survives with any node retaining its owning Document. */
		if (node->document->resource_url != NULL)
			vm_heap_mark(heap, &node->document->resource_url->cell);
		if (node->document->resource_mime != NULL)
			vm_heap_mark(heap, &node->document->resource_mime->cell);
		if (node->document->target != NULL)
			vm_heap_mark(heap, &node->document->target->cell);

		/* Both fields are cells whose traces preserve prototype and implementation graphs. */
		if (node->document->binding_prototypes != NULL)
			vm_heap_mark(heap, &node->document->binding_prototypes->cell);

		/* SameObject caching must survive a collection between getter observations. */
		if (node->document->implementation != NULL)
			vm_heap_mark(heap, &node->document->implementation->cell);

		/* Document links is another SameObject cache with a traced rooted query. */
		if (node->document->links_collection != NULL)
			vm_heap_mark(heap, &node->document->links_collection->cell);

		/* Form collection identity survives while the Document remains reachable. */
		if (node->document->forms_collection != NULL)
			vm_heap_mark(heap, &node->document->forms_collection->cell);

		/* Image collection identity participates in the same collectible Document cache graph. */
		if (node->document->images_collection != NULL)
			vm_heap_mark(heap, &node->document->images_collection->cell);

		/* A Document alone preserves its current live stylesheet query identity. */
		if (node->document->style_sheets != NULL)
			vm_heap_mark(heap, &node->document->style_sheets->cell);
		if (node->document->font_set != NULL)
			vm_heap_mark(heap, &node->document->font_set->cell);
		if (node->document->font_ready != NULL)
			vm_heap_mark(heap, node->document->font_ready);
	}

	/* Connected relatives retain the live tree around a saved node. */
	if (node->parent != NULL)
		vm_heap_mark(heap, &node->parent->cell);
	if (node->previous != NULL)
		vm_heap_mark(heap, &node->previous->cell);
	if (node->next != NULL)
		vm_heap_mark(heap, &node->next->cell);
	if (node->first_child != NULL)
		vm_heap_mark(heap, &node->first_child->cell);
	if (node->last_child != NULL)
		vm_heap_mark(heap, &node->last_child->cell);
}

/* Marks what an element refers to: its links, names, attributes and template contents. */
static void
element_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_element *element;
	size_t index;

	/* Marks the links every node has. */
	node_trace(heap, cell);

	/* Marks the names and the template contents. */
	element = (struct dom_element *)cell;
	vm_heap_mark(heap, &element->local_name->cell);
	if (element->prefix != NULL)
		vm_heap_mark(heap, &element->prefix->cell);

	/* Arbitrary script namespace URIs remain alive with their element graph. */
	if (element->namespace_uri != NULL)
		vm_heap_mark(heap, &element->namespace_uri->cell);

	/* Template and frame ownership remain independent of namespace identity. */
	if (element->content != NULL)
		vm_heap_mark(heap, &element->content->cell);
	if (element->child_context != NULL)
		vm_heap_mark(heap, element->child_context);

	/* Pending source identity is independent of the published child context. */
	if (element->child_source != NULL)
		vm_heap_mark(heap, &element->child_source->cell);
	if (element->child_attribute != NULL)
		vm_heap_mark(heap, &element->child_attribute->cell);
	if (element->child_navigation != NULL)
		vm_heap_mark(heap, &element->child_navigation->cell);

	/* The exact inline source remains live with its Element, independently of the rounded attribute. */
	if (element->inline_style_source != NULL)
		vm_heap_mark(heap, &element->inline_style_source->cell);

	/* A reachable native style element retains its current binding-owned source model. */
	if (element->style_sheet != NULL)
		vm_heap_mark(heap, element->style_sheet);

	/* A saved SVG rect retains its binding-owned stable scalar length handles. */
	if (element->svg_width != NULL)
		vm_heap_mark(heap, element->svg_width);

	/* A form alone retains its cached live controls list across collection. */
	if (element->controls_collection != NULL)
		vm_heap_mark(heap, &element->controls_collection->cell);
	if (element->attributes_collection != NULL)
		vm_heap_mark(heap, &element->attributes_collection->cell);

	/* The cached current select remains valid during option-owner reconciliation. */
	if (element->option_select != NULL)
		vm_heap_mark(heap, &element->option_select->cell);

	/* Native select caches survive while the actual select alone remains reachable. */
	if (element->options_collection != NULL)
		vm_heap_mark(heap, &element->options_collection->cell);

	/* Table collection caches survive when only their native root remains reachable. */
	if (element->bodies_collection != NULL)
		vm_heap_mark(heap, &element->bodies_collection->cell);
	if (element->rows_collection != NULL)
		vm_heap_mark(heap, &element->rows_collection->cell);
	if (element->cells_collection != NULL)
		vm_heap_mark(heap, &element->cells_collection->cell);

	/* Marks each attribute's strings. */
	for (index = 0; index < element->attribute_count; index++) {
		vm_heap_mark(heap, &element->attributes[index].name->cell);
		vm_heap_mark(heap, &element->attributes[index].value->cell);
		if (element->attributes[index].prefix != NULL)
			vm_heap_mark(heap, &element->attributes[index].prefix->cell);
		if (element->attributes[index].namespace_uri != NULL)
			vm_heap_mark(heap, &element->attributes[index].namespace_uri->cell);
	}
}

/* Frees the attribute array of a dead element. */
static void
element_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_element *element;

	UNUSED_PARAMETER(heap);

	/* The strings are cells of their own; only the array and a control's state are the element's. */
	element = (struct dom_element *)cell;
	free(element->attributes);
	dom_control_free(element);
}

/* Marks actual character-node ownership and the optional ProcessingInstruction target. */
static void
character_data_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_character_data *data;

	/* Ordinary node links retain the actual owner and connected tree. */
	node_trace(heap, cell);
	data = (struct dom_character_data *)cell;
	if (data->target != NULL)
		vm_heap_mark(heap, &data->target->cell);

	/* Succeeded: the target has no independent permanent root. */
	return;
}

/* Frees the C character buffer of a dead Text, CDATA, Comment or PI node. */
static void
character_data_finalize(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_character_data *text;

	UNUSED_PARAMETER(heap);

	/* Frees the buffer. */
	text = (struct dom_character_data *)cell;
	wb_units_release(&text->data);
}

/* Marks what a DOCTYPE refers to: its links and strings. */
static void
doctype_trace(
	struct vm_heap *heap,
	struct vm_cell *cell)
{
	struct dom_doctype *doctype;

	/* Marks the links every node has. */
	node_trace(heap, cell);

	/* Marks the strings. */
	doctype = (struct dom_doctype *)cell;
	vm_heap_mark(heap, &doctype->name->cell);
	vm_heap_mark(heap, &doctype->public_id->cell);
	vm_heap_mark(heap, &doctype->system_id->cell);
}

/* Allocates a node of a kind in its document's heap. */
static struct dom_node *
node_alloc(
	struct dom_document *document,
	const struct vm_cell_type *type,
	size_t size,
	int node_type)
{
	struct dom_node *node;

	/* Allocates the cell, zeroed. */
	node = vm_heap_alloc(document->heap, type, size);
	if (node == NULL)
		return NULL;

	/* Records the kind and the document. */
	node->type = (uint16_t)node_type;
	node->document = document;

	/* Succeeded: the node is detached. */
	return node;
}

/* Makes a text or comment node holding length units. */
static struct dom_node *
character_data_create(
	struct dom_document *document,
	int node_type,
	const uint16_t *units,
	size_t length)
{
	struct dom_character_data *text;
	int error;

	/* Allocates the node's cell. */
	text = (struct dom_character_data *)node_alloc(document, &character_data_type, sizeof(*text), node_type);
	if (text == NULL)
		return NULL;

	/* Copies the characters into the node's buffer (an empty node owns no buffer). */
	wb_units_init(&text->data);
	error = wb_units_append(&text->data, units, length);
	if (error != 0)
		return NULL;

	/* Succeeded: the node is detached. */
	return &text->node;
}

/* Assign one ordinary no-namespace attribute and report publication separately from notification success. */
static int
node_set_attribute(
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *value,
	int *committed)
{
	struct dom_attribute *attribute;
	struct vm_string *old_value;
	int error;

	/* Existing attributes lend their retired value until the notification hook roots it. */
	*committed = 0;
	old_value = NULL;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute != NULL) {
		/* These native value, select-state and generation updates allocate no VM cells. */
		old_value = attribute->value;
		name = attribute->name;
		attribute->value = value;
		dom_select_attribute_changed(element, name, 1, 1);
		element->node.document->generation++;
	} else {
		/* Append publishes all native attribute edges before any notification can allocate. */
		error = dom_element_add_attribute(element, DOM_NS_NONE, NULL, name, value);
		if (error != 0)
			return error;
	}

	/* Publish commit before notifications can fail or reenter the declaration model. */
	*committed = 1;

	/* The ordinary commit is complete, including a same-value assignment. */
	error = node_attribute_notify(element, name, old_value);
	if (error != 0)
		return error;

	/* Succeeded: the attribute and every interested observer see the committed change. */
	return 0;
}

/* Delivers committed ordinary attribute snapshots through an opaque binding-owner hook. */
static int
node_attribute_notify(
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *old_value)
{
	struct dom_document *document;
	int error;
	int is_style;

	/* External style changes reparse even when the exact serialized string object is assigned again. */
	is_style = vm_string_equal_ascii(name, "style");
	if (is_style &&
	    !element->inline_style_updating)
		element->inline_style_source = NULL;

	/* Parser and standalone Documents have no attached notification owner. */
	document = element->node.document;
	if (document->attribute_changed == NULL)
		return 0;

	/* The hook must root every borrowed snapshot before performing a VM allocation. */
	error = document->attribute_changed(document, element, name, old_value);
	if (error != 0)
		return error;

	/* Succeeded: all interested owner registrations accepted this change. */
	return 0;
}
