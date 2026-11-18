/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exact native expanded attribute names retain URI identity independently of fast namespace classes. */

#include "dom/dom.h"

#include <errno.h>

/* Immutable process-lifetime URIs preserve the legacy parser's exact namespace classes. */
static const char *const attribute_uris[] = {
	NULL,
	"http://www.w3.org/1999/xhtml",
	"http://www.w3.org/2000/svg",
	"http://www.w3.org/1998/Math/MathML",
	"http://www.w3.org/1999/xlink",
	"http://www.w3.org/XML/1998/namespace",
	"http://www.w3.org/2000/xmlns/"
};

static int attribute_namespace_id(const struct vm_string *uri);

/*
 * Adds one already validated native attribute only when its actual expanded name is absent.
 */
int
dom_element_add_attribute_uri(
	struct dom_element *element,
	struct vm_string *uri,
	struct vm_string *prefix,
	struct vm_string *name,
	struct vm_string *value)
{
	struct dom_attribute *existing;
	int ns;
	int status;

	/* Invalid embedding arguments cannot publish an unusable native record. */
	if (element == NULL ||
	    name == NULL ||
	    value == NULL)
		return EINVAL;

	/* Namespace absence and the empty URI are the same expanded-name identity. */
	if (uri != NULL) {
		if (uri->length == 0)
			uri = NULL;
	}

	/* Prefix aliases cannot introduce two records for the same URI and local name. */
	existing = dom_element_find_attribute_uri(element, uri, name);
	if (existing != NULL)
		return EEXIST;

	/* Allocate the native attribute using its compatible parser namespace class. */
	ns = attribute_namespace_id(uri);
	status = dom_element_add_attribute(element, ns, prefix, name, value);
	if (status != 0)
		return status;

	/* The array growth and select-state update allocate no VM cells before this owned edge is published. */
	element->attributes[element->attribute_count - 1U].namespace_uri = uri;

	/* Succeeded: classification, exact namespace and attribute strings share the owning element graph. */
	return 0;
}

/*
 * Finds one actual expanded attribute name by exact URI and local-name characters.
 */
struct dom_attribute *
dom_element_find_attribute_uri(
	const struct dom_element *element,
	const struct vm_string *uri,
	const struct vm_string *name)
{
	struct dom_attribute *attribute;
	size_t index;
	int ns;
	int same;

	/* Optional missing arguments cannot identify an actual attribute. */
	if (element == NULL || name == NULL)
		return NULL;

	/* Normalize empty namespace strings to the same absence used by insertion. */
	if (uri != NULL) {
		if (uri->length == 0)
			uri = NULL;
	}

	/* Classification narrows built-in lookups while exact strings distinguish all arbitrary namespaces. */
	ns = attribute_namespace_id(uri);
	for (index = 0; index < element->attribute_count; index++) {
		/* Skip parser classes which cannot denote the requested namespace. */
		attribute = &element->attributes[index];
		if (attribute->ns != ns)
			continue;

		/* Compare the local name before inspecting arbitrary namespace identities. */
		same = vm_string_equal(attribute->name, name);
		if (!same)
			continue;

		/* Ordinary parser attributes have implicit canonical identities for their built-in classes. */
		if (ns != DOM_NS_OTHER)
			return attribute;

		/* A foreign-class record without its exact URI cannot satisfy this lookup. */
		if (attribute->namespace_uri == NULL)
			continue;

		/* Distinguish foreign namespaces which share the parser's catch-all class. */
		same = vm_string_equal(attribute->namespace_uri, uri);
		if (same)
			return attribute;
	}

	/* Absent: no matching expanded name exists in the native ordered attribute array. */
	return NULL;
}

/* Classifies exact canonical namespace URIs without collapsing arbitrary identity. */
static int
attribute_namespace_id(
	const struct vm_string *uri)
{
	unsigned index;
	int same;

	/* Missing URI represents the absent namespace. */
	if (uri == NULL)
		return DOM_NS_NONE;

	/* Native built-in consumers continue to use their established exact classification. */
	for (index = DOM_NS_HTML; index <= DOM_NS_XMLNS; index++) {
		same = vm_string_equal_ascii(uri, attribute_uris[index]);
		if (same)
			return (int)index;
	}

	/* Succeeded: callers must compare the retained exact URI within this shared foreign class. */
	return DOM_NS_OTHER;
}
