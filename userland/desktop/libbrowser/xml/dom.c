/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies complete validated XML records into actual owning native DOM without resource or script effects.
 */

#include "xml/dom.h"

#include <errno.h>

/* One conversion roots its owning Document, pending node and temporary native name/value strings. */
struct xml_projection {
	struct vm_heap *heap;
	struct dom_document *document;
	struct vm_cell *roots[6];
};

/* Immutable process-lifetime URIs identify the exact namespaces with native built-in classifications. */
static const char *const projection_uris[] = {
	NULL,
	"http://www.w3.org/1999/xhtml",
	"http://www.w3.org/2000/svg",
	"http://www.w3.org/1998/Math/MathML",
	"http://www.w3.org/1999/xlink",
	"http://www.w3.org/XML/1998/namespace",
	"http://www.w3.org/2000/xmlns/"
};

static int projection_tree(struct xml_projection *projection, const struct xml_document *model);
static int projection_node(struct xml_projection *projection, const struct xml_node *source, struct dom_node **created);
static int projection_element(struct xml_projection *projection, const struct xml_node *source, struct dom_node **created);
static int projection_attributes(struct xml_projection *projection, struct dom_element *element, const struct xml_attribute *source);
static int projection_string(struct xml_projection *projection, struct xml_units input, int atom, int optional, unsigned slot, struct vm_string **created);
static int projection_namespace(const struct vm_string *uri);
static void projection_unroot(struct xml_projection *projection, unsigned count);

/*
 * Publishes a complete owning native XML Document only after every actual record is copied.
 */
int
xml_document_project(
	struct vm_heap *heap,
	const struct xml_document *model,
	enum dom_document_content content,
	struct dom_document **created)
{
	struct xml_projection projection;
	unsigned index;
	int error;

	/* Invalid embedding arguments cannot create or publish a partially converted Document. */
	if (created == NULL)
		return EINVAL;

	/* Failed projection cannot leave the caller holding a partially converted Document. */
	*created = NULL;

	/* Both allocation storage and a complete source model are required for projection. */
	if (heap == NULL || model == NULL)
		return EINVAL;

	/* Only declared XML processing kinds use this strict native model projection. */
	if (content != DOM_CONTENT_XML &&
	    content != DOM_CONTENT_XHTML &&
	    content != DOM_CONTENT_SVG)
		return EINVAL;

	/* A completed model must have an actual Document root with at least one child. */
	if (model->node.type != XML_DOCUMENT || model->node.first_child == NULL)
		return EINVAL;

	/* Every nullable owner and temporary slot is registered before the first VM allocation. */
	projection.heap = heap;
	projection.document = NULL;

	/* Register each nullable owner slot, unwinding only the preceding successful registrations. */
	for (index = 0; index < 6U; index++) {
		projection.roots[index] = NULL;
		error = vm_heap_add_root(heap, &projection.roots[index]);
		if (error != 0) {
			projection_unroot(&projection, index);
			return error;
		}
	}

	/* This initially unpublished Document owns every connected native node built during conversion. */
	projection.document = dom_document_create(heap);
	if (projection.document == NULL) {
		projection_unroot(&projection, 6);
		return ENOMEM;
	}

	/* Caller response policy supplies the content kind; root namespace never invents MIME metadata. */
	projection.roots[0] = &projection.document->node.cell;
	projection.document->content = content;

	/* Copy every model record while the newly created native Document remains precisely rooted. */
	error = projection_tree(&projection, model);
	if (error != 0) {
		projection_unroot(&projection, 6);
		return error;
	}

	/* The complete graph needs no parser input or arena storage after publication. */
	*created = projection.document;
	projection_unroot(&projection, 6);

	/* Succeeded: caller takes native ownership without an intervening allocation or activation effect. */
	return 0;
}

/* Walks actual source preorder and corresponding native parents without C recursion or a truncating depth cap. */
static int
projection_tree(
	struct xml_projection *projection,
	const struct xml_document *model)
{
	const struct xml_node *source;
	struct dom_node *parent;
	struct dom_node *node;
	unsigned index;
	int error;

	/* The two parent positions always refer to the same actual ancestor in the source and copied graph. */
	source = model->node.first_child;
	parent = &projection->document->node;
	while (source != NULL) {
		/* Reuse only temporary slots; the connected graph remains rooted through its Document. */
		for (index = 1; index < 6U; index++)
			projection->roots[index] = NULL;

		/* Create one complete unlinked native record before adding it to the protected owner graph. */
		error = projection_node(projection, source, &node);
		if (error != 0)
			return error;

		/* Protect the newly allocated record until its native parent owns the connected edge. */
		projection->roots[1] = &node->cell;
		dom_append_child(parent, node);

		/* Actual child edges descend into the complete newly linked native parent. */
		if (source->first_child != NULL) {
			source = source->first_child;
			parent = node;
			continue;
		}

		/* Completed last-child branches ascend until the next actual sibling or Document boundary. */
		while (source->next == NULL && source->parent != &model->node) {
			source = source->parent;
			parent = parent->parent;
		}

		/* The next actual sibling shares this same retained native parent. */
		source = source->next;
	}

	/* Succeeded: every real model record has exactly one corresponding native record. */
	return 0;
}

/* Copies one actual node while preserving its exact character kind and immutable PI target. */
static int
projection_node(
	struct xml_projection *projection,
	const struct xml_node *source,
	struct dom_node **created)
{
	struct vm_string *target;
	struct dom_node *node;
	int error;

	/* Element names and attributes have separate checked temporary string ownership. */
	*created = NULL;
	if (source->type == XML_ELEMENT) {
		error = projection_element(projection, source, created);
		return error;
	}

	/* The native factories copy caller-owned C data; model storage remains valid throughout this call. */
	node = NULL;
	switch (source->type) {
	case XML_TEXT:
		/* Copy normalized model text into the native Document's own character storage. */
		node = dom_text_create(projection->document, source->value.data, source->value.length);
		break;
	case XML_COMMENT:
		/* Preserve comments as native records without borrowing their parser arena. */
		node = dom_comment_create(projection->document, source->value.data, source->value.length);
		break;
	case XML_CDATA:
		/* The CDATA factory validates this strict character kind before creating native storage. */
		error = dom_cdata_create(projection->document, source->value.data, source->value.length, &node);
		if (error != 0)
			return error;
		break;
	case XML_PI:
		/* Retain the immutable instruction target before allocating the native instruction node. */
		error = projection_string(projection, source->name, 0, 0, 2, &target);
		if (error != 0)
			return error;

		/* Copy the instruction body while its independently allocated target is rooted. */
		error = dom_pi_create(projection->document, target, source->value.data, source->value.length, &node);
		if (error != 0)
			return error;
		break;
	default:
		return EINVAL;
	}

	/* Failed native allocation cannot publish a successful partial model projection. */
	if (node == NULL)
		return ENOMEM;

	/* Publish only a complete independently owned native character record. */
	*created = node;

	/* Succeeded: no native character buffer or target string borrows model data. */
	return 0;
}

/* Copies actual element identity before attributing its complete native expanded-name records. */
static int
projection_element(
	struct xml_projection *projection,
	const struct xml_node *source,
	struct dom_node **created)
{
	struct dom_element *element;
	struct vm_string *local;
	struct vm_string *prefix;
	struct vm_string *uri;
	int ns;
	int error;

	/* Temporary name strings survive all subsequent allocating conversions and native node creation. */
	error = projection_string(projection, source->local, 1, 0, 2, &local);

	/* Copy the optional prefix only when the required local name was retained successfully. */
	if (error == 0)
		error = projection_string(projection, source->prefix, 1, 1, 3, &prefix);

	/* Preserve the actual namespace URI independently of canonical prefix aliases. */
	if (error == 0)
		error = projection_string(projection, source->uri, 0, 1, 4, &uri);
	if (error != 0)
		return error;

	/* Allocate the native element only after all of its expanded-name strings are rooted. */
	ns = projection_namespace(uri);
	element = dom_element_create(projection->document, ns, local, prefix);
	if (element == NULL)
		return ENOMEM;

	/* The complete unlinked native element owns its names and URI before any attribute VM allocation. */
	projection->roots[1] = &element->node.cell;
	element->namespace_uri = uri;

	/* Attribute allocation can now collect without losing the complete pending element. */
	error = projection_attributes(projection, element, source->attributes);
	if (error != 0)
		return error;

	/* Expose the native element only after its full attribute list has been copied. */
	*created = &element->node;

	/* Succeeded: all actual source attributes belong to this complete pending native element. */
	return 0;
}

/* Copies every normalized attribute using actual URI/local expanded identity rather than prefix aliases. */
static int
projection_attributes(
	struct xml_projection *projection,
	struct dom_element *element,
	const struct xml_attribute *source)
{
	struct vm_string *local;
	struct vm_string *prefix;
	struct vm_string *uri;
	struct vm_string *value;
	int error;

	/* Pending element ownership protects each earlier attribute while temporary slots are reused. */
	while (source != NULL) {
		/* Retain the required local name before copying this attribute's remaining identity. */
		error = projection_string(projection, source->local, 1, 0, 2, &local);

		/* An optional prefix preserves the source spelling without defining namespace identity. */
		if (error == 0)
			error = projection_string(projection, source->prefix, 1, 1, 3, &prefix);

		/* The copied URI supplies the exact native expanded-name namespace. */
		if (error == 0)
			error = projection_string(projection, source->uri, 0, 1, 4, &uri);

		/* Copy the complete normalized value after every name component is retained. */
		if (error == 0)
			error = projection_string(projection, source->value, 0, 0, 5, &value);
		if (error != 0)
			return error;

		/* Link this fully prepared attribute using the ordinary native ownership path. */
		error = dom_element_add_attribute_uri(element, uri, prefix, local, value);
		if (error != 0)
			return error;

		/* Earlier linked attributes stay reachable while the next model record reuses temporary slots. */
		source = source->next;
	}

	/* Succeeded: namespace declarations and ordinary attributes preserve source order and exact values. */
	return 0;
}

/* Copies one model view into an atom or ordinary VM string, retaining it before the next allocation. */
static int
projection_string(
	struct xml_projection *projection,
	struct xml_units input,
	int atom,
	int optional,
	unsigned slot,
	struct vm_string **created)
{
	struct vm_string *string;

	/* Missing prefix/namespace is distinct from a required empty attribute or character string. */
	*created = NULL;
	projection->roots[slot] = NULL;
	if (optional && input.length == 0)
		return 0;

	/* Atomize names while preserving ordinary owned storage for values and namespace URIs. */
	if (atom) {
		string = vm_atom_from_units(projection->heap, input.data, input.length);
	} else {
		string = vm_string_from_units(projection->heap, input.data, input.length);
	}

	/* Every allocating string operation has explicit failure and precise temporary ownership. */
	if (string == NULL)
		return ENOMEM;

	/* Protect the independent copy before any subsequent native allocation can collect it. */
	projection->roots[slot] = &string->cell;
	*created = string;

	/* Succeeded: the copied string is independent of the parser model lifetime. */
	return 0;
}

/* Resolves only exact canonical URI characters to native fast namespace classification. */
static int
projection_namespace(
	const struct vm_string *uri)
{
	unsigned index;
	int same;

	/* Absent namespace remains distinct from every nonempty URI. */
	if (uri == NULL)
		return DOM_NS_NONE;

	/* Compare every built-in namespace by exact retained URI text rather than prefix spelling. */
	for (index = DOM_NS_HTML; index <= DOM_NS_XMLNS; index++) {
		same = vm_string_equal_ascii(uri, projection_uris[index]);
		if (same)
			return (int)index;
	}

	/* Succeeded: exact retained URI distinguishes arbitrary namespaces within this shared class. */
	return DOM_NS_OTHER;
}

/* Releases only this invocation's successfully registered native ownership slots. */
static void
projection_unroot(
	struct xml_projection *projection,
	unsigned count)
{
	unsigned index;

	/* Unpublished partial graphs become normally collectible after the last precise root is removed. */
	for (index = 0; index < count; index++)
		vm_heap_remove_root(projection->heap, &projection->roots[index]);

	/* Succeeded: neither normal nor failed projection leaves a permanent native root. */
	return;
}
