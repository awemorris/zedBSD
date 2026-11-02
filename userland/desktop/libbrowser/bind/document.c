/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Document interface: the document's parts (its root element, head,
 * body and title), finding elements, and making nodes.
 */

#include "bind/internal.h"
#include "html/html.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int document_this(struct vm_realm *realm, vm_value this_value, struct dom_document **document);
static int document_write(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_writeln(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_write_text(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result, int newline);
static int document_open(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_close(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_stream_open(struct bind_window *window);
static int document_stream_finish(struct bind_window *window);
static int document_stream_script(void *context, struct dom_element *script);
static struct dom_element *document_html_child(const struct dom_document *document, int tag);
static struct dom_node *document_find_title(const struct dom_document *document);
static int document_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_head(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_body(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_doctype(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_title_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_title_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_ready_state(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_fonts(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_default_view(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_get_element_by_id(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_create_element(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_create_element_ns(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_element_name_valid(const struct vm_string *name);
static int document_prefix_valid(const struct vm_string *prefix);
static int document_namespace_id(const struct vm_string *uri);
static int document_create_text_node(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_create_comment(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_create_fragment(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_element_from_point(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_elements_from_point(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_cookie_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_cookie_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_url(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_domain(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_location(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_referrer(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_hidden(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_visibility_state(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_compat_mode(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_content_type(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_current_script(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_character_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int document_constant_string(struct vm_realm *realm, vm_value this_value, const char *text, vm_value *result);
static int document_create_character_data(struct vm_realm *realm, vm_value this_value, int type, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of Document, with ParentNode's.  The table is constant
 * for the life of the program.
 */
static const struct bind_attribute document_attributes[] = {
	{ "implementation", bind_document_implementation, NULL },
	{ "contentType", document_content_type, NULL },
	{ "documentElement", document_element, NULL },
	{ "head", document_head, NULL },
	{ "body", document_body, NULL },
	{ "doctype", document_doctype, NULL },
	{ "title", document_title_get, document_title_set },
	{ "readyState", document_ready_state, NULL },
	{ "currentScript", document_current_script, NULL },
	{ "fonts", document_fonts, NULL },
	{ "cookie", document_cookie_get, document_cookie_set },
	{ "URL", document_url, NULL },
	{ "documentURI", document_url, NULL },
	{ "domain", document_domain, NULL },
	{ "location", document_location, NULL },
	{ "referrer", document_referrer, NULL },
	{ "hidden", document_hidden, NULL },
	{ "visibilityState", document_visibility_state, NULL },
	{ "compatMode", document_compat_mode, NULL },
	{ "characterSet", document_character_set, NULL },
	{ "charset", document_character_set, NULL },
	{ "inputEncoding", document_character_set, NULL },
	{ "defaultView", document_default_view, NULL },
	{ "forms", bind_document_forms, NULL },
	{ "links", bind_document_links, NULL },
	{ "children", bind_children, NULL },
	{ "firstElementChild", bind_first_element_child_get, NULL },
	{ "lastElementChild", bind_last_element_child_get, NULL },
	{ "childElementCount", bind_child_element_count, NULL },
	{ "images", bind_document_images, NULL },
	{ "scripts", bind_document_scripts, NULL },
	{ "styleSheets", bind_cssom_document_sheets, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Document, with ParentNode's.  The table is constant
 * for the life of the program.
 */
static const struct bind_operation document_operations[] = {
	{ "write", 0, document_write },
	{ "writeln", 0, document_writeln },
	{ "getElementById", 1, document_get_element_by_id },
	{ "getElementsByTagName", 1, bind_get_elements_by_tag_name },
	{ "getElementsByClassName", 1, bind_get_elements_by_class_name },
	{ "createElement", 1, document_create_element },
	{ "createElementNS", 2, document_create_element_ns },
	{ "createTextNode", 1, document_create_text_node },
	{ "createComment", 1, document_create_comment },
	{ "createDocumentFragment", 0, document_create_fragment },
	{ "createEvent", 1, bind_document_create_event },
	{ "createTreeWalker", 1, bind_create_tree_walker },
	{ "createNodeIterator", 1, bind_create_node_iterator },
	{ "createRange", 0, bind_create_range },
	{ "elementFromPoint", 2, document_element_from_point },
	{ "elementsFromPoint", 2, document_elements_from_point },
	{ "append", 0, bind_append },
	{ "prepend", 0, bind_prepend },
	{ "replaceChildren", 0, bind_replace_children },
	{ "querySelector", 1, bind_query_selector },
	{ "querySelectorAll", 1, bind_query_selector_all },
	{ "open", 0, document_open },
	{ "close", 0, document_close },
	{ NULL, 0, NULL }
};

/*
 * Creates a namespaced element from already-converted namespace and name strings.
 *
 * Both Document creation APIs use the same validation before exposing any node.
 */
int
bind_create_element_ns(
	struct vm_realm *realm,
	struct dom_document *document,
	struct vm_string *uri,
	struct vm_string *qualified,
	struct dom_element **created)
{
	struct dom_element *element;
	struct vm_string *prefix;
	struct vm_string *local;
	struct vm_cell *roots[4];
	struct wb_units units;
	size_t colon;
	unsigned registered;
	int ns;
	int valid;
	int xml_prefix;
	int xmlns_name;
	int status;

	/* Retains caller strings and the new name slices until the element owns them. */
	roots[0] = NULL;
	if (uri != NULL)
		roots[0] = &uri->cell;
	roots[1] = &qualified->cell;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	while (registered < 4U) {
		status = vm_heap_add_root(document->heap, &roots[registered]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Copies both string representations into checked UTF-16 slice storage. */
	wb_units_init(&units);
	status = vm_string_append_units(qualified, &units);
	if (status != 0) {
		wb_units_release(&units);
		goto cleanup;
	}

	/* The first colon separates the prefix from the remaining local name. */
	prefix = NULL;
	local = qualified;
	for (colon = 0; colon < units.length; colon++) {
		if (units.data[colon] == ':')
			break;
	}

	/* Allocates and checks each name slice before consuming its contents. */
	if (colon < units.length) {
		prefix = vm_atom_from_units(realm->heap, units.data, colon);
		if (prefix == NULL) {
			wb_units_release(&units);
			status = ENOMEM;
			goto cleanup;
		}

		/* The prefix survives allocation of the local-name atom. */
		roots[2] = &prefix->cell;

		/* The local name excludes the separator but preserves all later code units. */
		local = vm_atom_from_units(realm->heap, units.data + colon + 1U, units.length - colon - 1U);
		if (local == NULL) {
			wb_units_release(&units);
			status = ENOMEM;
			goto cleanup;
		}

		/* The local name survives validation and native element allocation. */
		roots[3] = &local->cell;
	}

	/* Name atoms now own their data independently of temporary slice storage. */
	wb_units_release(&units);

	/* Prefix syntax errors precede local-name and namespace-binding errors. */
	if (prefix != NULL) {
		valid = document_prefix_valid(prefix);
		if (!valid) {
			status = bind_throw_dom(realm, "InvalidCharacterError", "The namespace prefix is not valid.");
			goto cleanup;
		}
	}

	/* Rejects malformed local names before checking reserved namespace bindings. */
	valid = document_element_name_valid(local);
	if (!valid) {
		status = bind_throw_dom(realm, "InvalidCharacterError", "The local name is not valid.");
		goto cleanup;
	}

	/* A nonempty prefix cannot be bound to a missing namespace. */
	ns = document_namespace_id(uri);
	if (prefix != NULL && ns == DOM_NS_NONE) {
		status = bind_throw_dom(realm, "NamespaceError", "A prefix requires a namespace.");
		goto cleanup;
	}

	/* The xml prefix is reserved for the exact XML namespace URI. */
	xml_prefix = 0;
	if (prefix != NULL)
		xml_prefix = vm_string_equal_ascii(prefix, "xml");

	/* A reserved XML prefix cannot name any unrelated namespace. */
	if (xml_prefix && ns != DOM_NS_XML) {
		status = bind_throw_dom(realm, "NamespaceError", "The xml prefix requires the XML namespace.");
		goto cleanup;
	}

	/* The xmlns name or prefix is reserved for the XMLNS namespace URI. */
	xmlns_name = vm_string_equal_ascii(qualified, "xmlns");
	if (prefix != NULL) {
		valid = vm_string_equal_ascii(prefix, "xmlns");
		if (valid)
			xmlns_name = 1;
	}

	/* Namespace declarations cannot be represented in an unrelated namespace. */
	if (xmlns_name && ns != DOM_NS_XMLNS) {
		status = bind_throw_dom(realm, "NamespaceError", "The xmlns name requires the XMLNS namespace.");
		goto cleanup;
	}

	/* The XMLNS namespace is reserved exclusively for xmlns names. */
	if (ns == DOM_NS_XMLNS && !xmlns_name) {
		status = bind_throw_dom(realm, "NamespaceError", "The XMLNS namespace requires the xmlns name.");
		goto cleanup;
	}

	/* Records the complete namespace identity before allocating its wrapper. */
	element = dom_element_create(document, ns, local, prefix);
	if (element == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The newly allocated element retains the full namespace identity. */
	element->namespace_uri = uri;

	/* Succeeded: the validated element retains the complete namespace identity. */
	*created = element;
	status = 0;

cleanup:
	/* The caller or new element owns every name after this factory returns. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(document->heap, &roots[registered]);
	}

	/* A failed validation or allocation leaves no published element. */
	if (status != 0)
		return status;

	/* Succeeded: the new element is available for publication. */
	return 0;
}

/*
 * The Document interface.
 */
const struct bind_interface bind_document_interface = {
	"Document", BIND_NODE, 0, NULL, document_attributes, document_operations, NULL
};

/*
 * The XMLDocument interface shares Document operations and keeps a distinct prototype.
 */
const struct bind_interface bind_xml_document_interface = {
	"XMLDocument", BIND_DOCUMENT, 0, NULL, NULL, NULL, NULL
};

/*
 * Executes supported inline classic text in its actual child realm.
 */
int
bind_run_inline_script(
	struct bind_window *window,
	struct dom_element *script)
{
	struct dom_attribute *attribute;
	struct dom_node *child;
	struct dom_character_data *data;
	struct wb_units text;
	size_t index;
	int runs;
	int same;
	int status;

	/* Mark parser preparation once, while treating external source as separate loading work. */
	script->node.flags |= DOM_NODE_SCRIPT_STARTED;
	runs = 1;
	for (index = 0; index < script->attribute_count; index++) {
		attribute = &script->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;
		same = vm_string_equal_ascii(attribute->name, "src");
		if (same)
			return 0;
		same = vm_string_equal_ascii(attribute->name, "type");
		if (!same || attribute->value->length == 0)
			continue;

		/* Supported classic MIME names are checked independently before parsing source. */
		runs = vm_string_equal_ascii(attribute->value, "text/javascript");
		if (!runs)
			runs = vm_string_equal_ascii(attribute->value, "application/javascript");
		if (!runs)
			runs = vm_string_equal_ascii(attribute->value, "text/ecmascript");
		if (!runs)
			runs = vm_string_equal_ascii(attribute->value, "application/ecmascript");
	}

	/* Modules and nonclassic data do not execute in this child parser subset. */
	if (!runs)
		return 0;

	/* Copy only native text children before script execution can mutate the element. */
	wb_units_init(&text);
	status = 0;
	for (child = script->node.first_child; child != NULL; child = child->next) {
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;
		data = (struct dom_character_data *)child;
		status = wb_units_append(&text, data->data.data, data->data.length);
		if (status != 0)
			break;
	}

	/* The actual child interpreter supplies native document globals and nested insertion writes. */
	if (status == 0)
		status = bind_run_script_source(window, text.data, text.length, "(document stream)", 1);
	wb_units_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: this parser hook completed without a fatal execution failure. */
	return 0;
}

/* Inserts the arguments into the active document parser. */
static int
document_write(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The common path concatenates every argument before inserting text. */
	status = document_write_text(realm, this_value, args, count, result, 0);
	if (status != 0)
		return status;

	/* Succeeded: write returns undefined. */
	return 0;
}

/* Inserts the arguments and one line feed into the active document parser. */
static int
document_writeln(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The common path adds the line feed after concatenating the arguments. */
	status = document_write_text(realm, this_value, args, count, result, 1);
	if (status != 0)
		return status;

	/* Succeeded: writeln returns undefined. */
	return 0;
}

/* Converts ordered arguments before feeding the receiver's actual HTML parser. */
static int
document_write_text(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result,
	int newline)
{
	struct bind_window *window;
	struct dom_document *document;
	struct vm_string *string;
	struct vm_cell *owner;
	struct wb_units text;
	unsigned index;
	int status;

	/* Native receiver and content kind precede any argument conversion side effects. */
	*result = VM_VALUE_UNDEFINED;
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* XML documents have no HTML insertion stream. */
	if (document->content != DOM_CONTENT_HTML) {
		status = bind_throw_dom(realm, "InvalidStateError", "XML documents cannot accept HTML writes.");
		return status;
	}

	/* Borrowed methods resolve the native owner rather than the callee's global. */
	window = document->view;
	if (window == NULL || window->detached) {
		status = bind_throw_dom(realm, "NotSupportedError", "No active document owner is available.");
		return status;
	}

	/* The owner retains the real Document and parser throughout conversion and inline GC. */
	owner = &window->realm->cell;
	status = vm_heap_add_root(realm->heap, &owner);
	if (status != 0)
		return status;
	wb_units_init(&text);

	/* Converts all arguments before any partial string enters the parser. */
	for (index = 0; index < count; index++) {
		status = vm_to_string(realm, args[index], &string);
		if (status != 0) {
			wb_units_release(&text);
			vm_heap_remove_root(realm->heap, &owner);
			return status;
		}

		/* Copy conversion output before another argument may invoke script or GC. */
		status = vm_string_append_units(string, &text);
		if (status != 0) {
			wb_units_release(&text);
			vm_heap_remove_root(realm->heap, &owner);
			return status;
		}
	}

	/* Writeln adds exactly one newline after the complete converted argument list. */
	if (newline) {
		status = wb_units_append_code_point(&text, 0x0aU);
		if (status != 0) {
			wb_units_release(&text);
			vm_heap_remove_root(realm->heap, &owner);
			return status;
		}
	}

	/* Conversion may retire the child, but may not leave a stale C owner in use. */
	if (document->view != window || window->detached) {
		status = bind_throw_dom(realm, "NotSupportedError", "The document owner was retired during conversion.");
		wb_units_release(&text);
		vm_heap_remove_root(realm->heap, &owner);
		return status;
	}

	/* Existing primary Page writes remain owned by the Page's active source parser. */
	if (!window->owned) {
		status = ENOTSUP;
		if (window->host.document_write != NULL)
			status = window->host.document_write(window->host.context, text.data, text.length);
	} else {
		/* A child write without an input stream starts actual replacement parsing. */
		if (window->document_parser == NULL) {
			status = document_stream_open(window);
			if (status != 0) {
				wb_units_release(&text);
				vm_heap_remove_root(realm->heap, &owner);
				return status;
			}
		}

		/* Depth keeps a nested close from freeing the parser on its executing C stack. */
		window->document_parser_depth++;
		if (window->document_parser_depth > 1U) {
			status = html_parser_write(window->document_parser, text.data, text.length);
		} else {
			status = html_parser_feed(window->document_parser, text.data, text.length);
		}

		/* The outer caller becomes eligible to finish or discard parser storage. */
		window->document_parser_depth--;

		/* Only the outer parse can finish EOF or discard a failed parser safely. */
		if (window->document_parser_depth == 0) {
			if (status == 0 && window->document_parser_close)
				status = document_stream_finish(window);
			if (status != 0) {
				html_parser_destroy(window->document_parser);
				window->document_parser = NULL;
				window->document_parser_close = 0;
				window->ready_state = "complete";
			}
		}
	}

	/* Written source is copied by the parser; every outcome releases temporary ownership. */
	wb_units_release(&text);
	vm_heap_remove_root(realm->heap, &owner);
	if (status == ENOTSUP) {
		status = bind_throw_dom(realm, "NotSupportedError", "Writing outside an active primary parser is not supported.");
		return status;
	}

	/* Existing parser insertion bounds remain visible as ordinary RangeError. */
	if (status == ELOOP) {
		status = vm_throw_range_error(realm, "Document write nesting limit exceeded.");
		return status;
	}

	/* Allocation, conversion and parser failures are never converted to success. */
	if (status != 0)
		return status;

	/* Succeeded: all converted arguments entered the receiver's own parser. */
	return 0;
}

/* Finds the top element at a viewport point, or null when none is there. */
static int
document_element_from_point(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_document *document;
	struct dom_node *node;
	double x;
	double y;
	int status;

	/* This must be a Document, and both coordinates use JavaScript's numeric conversion. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	status = vm_to_number(realm, js_argument(args, count, 0), &x);
	if (status == 0)
		status = vm_to_number(realm, js_argument(args, count, 1), &y);
	if (status != 0)
		return status;

	/* The host owns the current layout and hit testing. */
	window = bind_window_of(realm);
	node = NULL;
	if (document->view != NULL && ((struct bind_window *)document->view)->frame != NULL && window->host.child_element_at != NULL)
		node = window->host.child_element_at(window->host.context, (struct bind_window *)document->view, x, y);
	else if (window->frame != NULL && window->host.child_element_at != NULL)
		node = window->host.child_element_at(window->host.context, window, x, y);
	else if (window->host.element_at != NULL)
		node = window->host.element_at(window->host.context, x, y);
	return bind_wrap_or_null(window, node, result);
}

/* Reports the top element at a viewport point as the first item of an array. */
static int
document_elements_from_point(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value element;
	int status;

	/* This first implementation exposes the top painted element. */
	status = document_element_from_point(realm, this_value, args, count, &element);
	if (status != 0)
		return status;
	if (element == VM_VALUE_NULL)
		return js_builtin_array(realm, NULL, 0, result);
	return js_builtin_array(realm, &element, 1, result);
}

/* Reports the MIME content kind retained by the actual Document. */
static int
document_content_type(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	const char *mime;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only actual Documents expose this content-kind accessor. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Real response metadata takes precedence over factory defaults. */
	if (document->resource_mime != NULL) {
		*result = vm_value_cell(document->resource_mime);
		return 0;
	}

	/* The current parser always creates HTML; factories retain their XML MIME kind. */
	mime = "text/html";
	if (document->content == DOM_CONTENT_XML) {
		mime = "application/xml";
	} else if (document->content == DOM_CONTENT_XHTML) {
		mime = "application/xhtml+xml";
	} else if (document->content == DOM_CONTENT_SVG) {
		mime = "image/svg+xml";
	}

	/* Exposes the chosen MIME type as an ordinary readonly DOMString. */
	status = bind_string(realm, mime, result);
	if (status != 0)
		return status;

	/* Succeeded: the receiver's content kind is reported. */
	return 0;
}

/* Finds the document a method's this value stands for, throwing a TypeError otherwise. */
static int
document_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_document **document)
{
	struct dom_node *node;
	int status;

	/* The node, which must be a document. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_DOCUMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the document is found. */
	*document = (struct dom_document *)node;
	return 0;
}

/* Opens an object-preserving HTML replacement stream on an active managed child. */
static int
document_open(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	struct vm_cell *owner;
	int status;

	UNUSED_PARAMETER(args);

	/* Genuine Document receivers and XML refusal precede stream access. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	if (document->content != DOM_CONTENT_HTML) {
		status = bind_throw_dom(realm, "InvalidStateError", "XML documents cannot open HTML streams.");
		return status;
	}

	/* Primary replacement and the separate Window.open overload have no child stream here. */
	window = document->view;
	if (count >= 3U ||
	    window == NULL ||
	    !window->owned ||
	    window->detached) {
		status = bind_throw_dom(realm, "NotSupportedError", "This Document has no managed HTML replacement stream.");
		return status;
	}

	/* Parser script execution ignores open without discarding the current insertion point. */
	*result = this_value;
	if (window->document_parser_depth != 0)
		return 0;

	/* Replacement allocations and mutation records retain the actual child owner. */
	owner = &window->realm->cell;
	status = vm_heap_add_root(realm->heap, &owner);
	if (status != 0)
		return status;
	status = document_stream_open(window);
	vm_heap_remove_root(realm->heap, &owner);
	if (status != 0)
		return status;

	/* Succeeded: replacement parsing reuses the receiver's actual identity. */
	return 0;
}

/* Finishes a child-created stream while keeping executing parser storage alive. */
static int
document_close(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	struct vm_cell *owner;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* XML closing is invalid even when there is no associated stream. */
	*result = VM_VALUE_UNDEFINED;
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	if (document->content != DOM_CONTENT_HTML) {
		status = bind_throw_dom(realm, "InvalidStateError", "XML documents cannot close HTML streams.");
		return status;
	}

	/* No script-created parser makes close an ordinary no-op. */
	window = document->view;
	if (window == NULL || window->document_parser == NULL)
		return 0;

	/* Requested EOF is handled only after the executing parse's outer frame returns. */
	if (window->document_parser_depth != 0) {
		window->document_parser_close = 1;
		return 0;
	}

	/* EOF scripts and checkpoints may collect or retire the child owner. */
	owner = &window->realm->cell;
	status = vm_heap_add_root(realm->heap, &owner);
	if (status != 0)
		return status;
	status = document_stream_finish(window);
	vm_heap_remove_root(realm->heap, &owner);
	if (status != 0)
		return status;

	/* Succeeded: the real HTML parser consumed EOF before storage was released. */
	return 0;
}

/* Replaces actual children only after checked parser allocation succeeds. */
static int
document_stream_open(
	struct bind_window *window)
{
	struct html_parser *parser;
	struct dom_node *removed;
	struct vm_cell *removed_root;
	int status;

	/* Prepare the real HTML parser before abandoning any previous input or DOM. */
	status = html_parser_create(&parser, window->document, 1);
	if (status != 0)
		return status;
	html_parser_set_script_hook(parser, document_stream_script, window);
	html_parser_destroy(window->document_parser);
	window->document_parser = parser;
	html_parser_transfer_ownership(parser);
	window->document_parser_close = 0;
	window->ready_state = "loading";
	window->document->quirks = DOM_NO_QUIRKS;

	/* Native removals repair ranges, iterators and nested frame retirement before notification. */
	while (window->document->node.first_child != NULL) {
		removed = window->document->node.first_child;
		removed_root = &removed->cell;
		status = vm_heap_add_root(window->document->heap, &removed_root);
		if (status != 0) {
			html_parser_destroy(window->document_parser);
			window->document_parser = NULL;
			window->ready_state = "complete";
			return status;
		}

		/* Mutation callbacks may collect while they inspect the detached child. */
		dom_remove(removed);
		status = bind_environment_child_mutation(window, &window->document->node, NULL, removed);
		vm_heap_remove_root(window->document->heap, &removed_root);
		if (status != 0) {
			html_parser_destroy(window->document_parser);
			window->document_parser = NULL;
			window->ready_state = "complete";
			return status;
		}
	}

	/* Succeeded: the preserved child Document now owns an empty HTML input stream. */
	return 0;
}

/* Runs EOF under active-parser ownership and releases storage only after return. */
static int
document_stream_finish(
	struct bind_window *window)
{
	int status;

	/* Inline EOF scripts observe an active parse and cannot destroy it through close. */
	window->document_parser_depth++;
	status = html_parser_finish(window->document_parser);
	window->document_parser_depth--;
	html_parser_destroy(window->document_parser);
	window->document_parser = NULL;
	window->document_parser_close = 0;
	window->ready_state = "complete";
	if (status != 0)
		return status;

	/* Succeeded: both parser storage and its hidden DOM stack references were released. */
	return 0;
}

/* Runs the shared inline preparation through the parser callback contract. */
static int
document_stream_script(
	void *context,
	struct dom_element *script)
{
	int error;

	/* The actual Window owns the parser and its insertion point. */
	error = bind_run_inline_script(context, script);
	if (error != 0)
		return error;

	/* Succeeded: the production parser hook executed the supported inline text. */
	return 0;
}

/* Finds the first HTML child of a tag of the document's <html> element (its head or body). */
static struct dom_element *
document_html_child(
	const struct dom_document *document,
	int tag)
{
	struct dom_element *root;
	struct dom_node *child;
	int matches;

	/* The root must be an HTML <html>. */
	root = bind_first_element_child(&document->node);
	matches = 0;
	if (root != NULL)
		matches = dom_element_is(&root->node, DOM_NS_HTML, DOM_TAG_HTML);
	if (!matches)
		return NULL;

	/* Its first child of the tag. */
	for (child = root->node.first_child; child != NULL; child = child->next) {
		matches = dom_element_is(child, DOM_NS_HTML, tag);
		if (matches)
			return (struct dom_element *)child;
	}

	/* None. */
	return NULL;
}

/* Finds the document's first <title> element in tree order. */
static struct dom_node *
document_find_title(
	const struct dom_document *document)
{
	struct dom_node *walk;
	int is_title;

	/* The first <title> of the whole tree. */
	for (walk = bind_following(&document->node, &document->node);
	     walk != NULL;
	     walk = bind_following(walk, &document->node)) {
		is_title = dom_element_is(walk, DOM_NS_HTML, DOM_TAG_TITLE);
		if (is_title)
			return walk;
	}

	/* The document has no title. */
	return NULL;
}

/* Reports the document's root element (documentElement). */
static int
document_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *root;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Its element child, or null. */
	root = bind_first_element_child(&document->node);
	status = bind_wrap_or_null(bind_window_of(realm), (struct dom_node *)root, result);
	if (status != 0)
		return status;

	/* Succeeded: the root is reported. */
	return 0;
}

/* Reports the document's <head> (head). */
static int
document_head(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *head;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The <head> child of <html>, or null. */
	head = document_html_child(document, DOM_TAG_HEAD);
	status = bind_wrap_or_null(bind_window_of(realm), (struct dom_node *)head, result);
	if (status != 0)
		return status;

	/* Succeeded: the head is reported. */
	return 0;
}

/* Reports the document's <body> (body). */
static int
document_body(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *body;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The <body> child of <html>, or its <frameset>, or null. */
	body = document_html_child(document, DOM_TAG_BODY);
	if (body == NULL)
		body = document_html_child(document, DOM_TAG_FRAMESET);
	status = bind_wrap_or_null(bind_window_of(realm), (struct dom_node *)body, result);
	if (status != 0)
		return status;

	/* Succeeded: the body is reported. */
	return 0;
}

/* Reports the document's DOCTYPE node (doctype). */
static int
document_doctype(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_node *child;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Its DOCTYPE child. */
	for (child = document->node.first_child; child != NULL; child = child->next) {
		if (child->type == DOM_DOCUMENT_TYPE)
			break;
	}

	/* The DOCTYPE, or null. */
	status = bind_wrap_or_null(bind_window_of(realm), child, result);
	if (status != 0)
		return status;

	/* Succeeded: the DOCTYPE is reported. */
	return 0;
}

/* Reports the document's title: its <title>'s text with whitespace collapsed and trimmed (title). */
static int
document_title_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_node *title;
	struct wb_units text;
	struct wb_units collapsed;
	uint16_t unit;
	size_t index;
	int space;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document and its title's text. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	title = document_find_title(document);
	wb_units_init(&text);
	wb_units_init(&collapsed);
	status = 0;
	if (title != NULL)
		status = bind_text_content(title, &text);

	/* Each run of whitespace becomes one space, and none at either end. */
	space = 0;
	for (index = 0; status == 0 && index < text.length; index++) {
		unit = text.data[index];
		if (unit == 0x20U || unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU) {
			space = 1;
			continue;
		}

		/* The space before a character, when text precedes it. */
		if (space && collapsed.length != 0)
			status = wb_units_append_code_point(&collapsed, 0x20U);
		space = 0;
		if (status == 0)
			status = wb_units_append(&collapsed, &text.data[index], 1);
	}

	/* The string. */
	if (status == 0)
		status = bind_units(realm, collapsed.data, collapsed.length, result);
	wb_units_release(&text);
	wb_units_release(&collapsed);
	if (status != 0)
		return status;

	/* Succeeded: the title is reported. */
	return 0;
}

/* Sets the document's title: the text of its <title>, made in the head when there is none (title). */
static int
document_title_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *head;
	struct dom_element *created;
	struct dom_node *title;
	struct dom_node *text;
	struct vm_string *string;
	struct vm_string *name;
	struct vm_cell *string_root;
	struct vm_cell *name_root;
	struct wb_units units;
	int status;

	/* The document and the new title. */
	*result = VM_VALUE_UNDEFINED;
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;
	string_root = &string->cell;
	status = vm_heap_add_root(realm->heap, &string_root);
	if (status != 0)
		return status;

	/* The <title>, or a new one at the end of the head (without a head, nothing changes). */
	title = document_find_title(document);
	if (title == NULL) {
		head = document_html_child(document, DOM_TAG_HEAD);
		if (head == NULL) {
			vm_heap_remove_root(realm->heap, &string_root);
			return 0;
		}

		/* Allocates the title name before making its element. */
		name = vm_atom_from_ascii(realm->heap, "title");
		if (name == NULL) {
			vm_heap_remove_root(realm->heap, &string_root);
			return ENOMEM;
		}

		/* The title name survives the native element allocation. */
		name_root = &name->cell;
		status = vm_heap_add_root(document->heap, &name_root);
		if (status != 0) {
			vm_heap_remove_root(realm->heap, &string_root);
			return status;
		}

		/* Creates and attaches the missing title element. */
		created = dom_element_create(document, DOM_NS_HTML, name, NULL);
		vm_heap_remove_root(document->heap, &name_root);
		if (created == NULL) {
			vm_heap_remove_root(realm->heap, &string_root);
			return ENOMEM;
		}

		/* The head now owns the new title before text replacement begins. */
		title = &created->node;
		dom_append_child(&head->node, title);
	}

	/* Its children go, and one text node holds the title. */
	while (title->first_child != NULL)
		dom_remove(title->first_child);
	wb_units_init(&units);
	status = vm_string_append_units(string, &units);
	vm_heap_remove_root(realm->heap, &string_root);
	text = NULL;
	if (status == 0 && units.length != 0) {
		text = dom_text_create(document, units.data, units.length);
		if (text == NULL)
			status = ENOMEM;
	}

	/* The characters are in the node now; the node goes into the title. */
	wb_units_release(&units);
	if (status != 0)
		return status;
	if (text != NULL)
		dom_append_child(title, text);

	/* Succeeded: the title is set. */
	return 0;
}

/* Reports the classic script element that is running, or null (currentScript). */
static int
document_current_script(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	window = document->view;
	if (window == NULL || window->current_script == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}
	return bind_wrap_or_null(window, window->current_script, result);
}

/* Reports how far the document is loaded: loading, interactive or complete (readyState). */
static int
document_ready_state(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	const char *state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Borrowed accessors report the Document's owner, not the callee's Window. */
	window = document->view;
	state = "complete";
	if (window != NULL)
		state = window->ready_state;
	status = bind_string(realm, state, result);
	if (status != 0)
		return status;

	/* Succeeded: the state is reported. */
	return 0;
}

/* Returns the Document's one initial font set and its stable ready promise. */
static int
document_fonts(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct vm_object *set;
	struct vm_string *key;
	struct vm_cell *roots[3];
	vm_value promise;
	unsigned registered;
	unsigned index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	if (document->font_set != NULL) {
		*result = vm_value_cell(document->font_set);
		return 0;
	}

	set = vm_object_create(realm->heap, realm->object_prototype);
	if (set == NULL)
		return ENOMEM;
	roots[0] = &set->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	for (index = 0; index < 3U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	status = vm_promise_create(realm, NULL, &promise);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(promise);
	key = vm_atom_from_ascii(realm->heap, "ready");
	if (key == NULL) {
		status = ENOMEM;
		goto cleanup;
	}
	roots[2] = &key->cell;
	status = vm_object_define(realm->heap, set, vm_value_cell(key), promise,
	                          VM_PROPERTY_ENUMERABLE);
	if (status != 0)
		goto cleanup;

	/* Publish only a complete, GC-traced SameObject graph. */
	document->font_set = set;
	document->font_ready = roots[1];
	if (document->view == NULL || document->fonts_settled) {
		status = vm_promise_resolve(realm, promise, vm_value_cell(set));
		if (status != 0) {
			document->font_set = NULL;
			document->font_ready = NULL;
			goto cleanup;
		}
	}
	*result = vm_value_cell(set);

cleanup:
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	return status;
}

/* A completed page layout releases the pending initial ready promise. */
int
bind_document_fonts_settle(
	struct dom_document *document)
{
	struct bind_window *window;
	int status;

	if (document->fonts_settled)
		return 0;
	if (document->font_ready != NULL) {
		window = document->view;
		if (window == NULL)
			return 0;
		status = vm_promise_resolve(window->realm, vm_value_cell(document->font_ready),
		                            vm_value_cell(document->font_set));
		if (status != 0)
			return status;
	}
	document->fonts_settled = 1;
	return 0;
}

/*
 * Reports the cookies a script may see for the document's URL, as the
 * host keeps them ("a=b; c=d", or the empty string) (cookie).
 */
static int
document_cookie_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	struct wb_buffer text;
	struct vm_string *string;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The host writes the cookies. */
	window = bind_window_of(realm);
	wb_buffer_init(&text);
	status = 0;
	if (window->host.cookie_get != NULL)
		status = window->host.cookie_get(window->host.context, &text);
	if (status != 0) {
		wb_buffer_release(&text);
		return status;
	}

	/* The string, from the UTF-8 text. */
	string = vm_string_from_utf8(realm->heap, wb_buffer_string(&text), text.length);
	wb_buffer_release(&text);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: the cookies are reported. */
	*result = vm_value_cell(string);
	return 0;
}

/* Sets one cookie of the document's URL, written as a Set-Cookie header would be (cookie). */
static int
document_cookie_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	struct vm_string *value;
	struct wb_buffer text;
	int status;

	/* The document and the text. */
	*result = VM_VALUE_UNDEFINED;
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &value);
	if (status != 0)
		return status;

	/* A host that keeps no cookies drops it. */
	window = bind_window_of(realm);
	if (window->host.cookie_set == NULL)
		return 0;

	/* The text as UTF-8, handed to the host. */
	wb_buffer_init(&text);
	status = vm_string_to_utf8(value, &text);
	if (status == 0)
		status = window->host.cookie_set(window->host.context, wb_buffer_string(&text), text.length);
	if (getenv("BROWSER_NET_DUMP") != NULL)
		fprintf(stderr, "cookie set: %.300s -> %d\n", text.data == NULL ? "" : wb_buffer_string(&text), status);
	wb_buffer_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the cookie is set (or refused, as the host decides). */
	return 0;
}

/* Reports the document's URL (URL, documentURI). */
static int
document_url(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Loaded metadata follows the actual Document even after retirement. */
	if (document->resource_url != NULL) {
		*result = vm_value_cell(document->resource_url);
		return 0;
	}

	/* An owning blank child keeps its own URL even through a borrowed accessor. */
	window = document->view;
	if (window == NULL) {
		status = bind_string(realm, "about:blank", result);
	} else {
		status = bind_location_part(window, BIND_LOCATION_HREF, result);
	}

	/* Allocation failures retain the existing getter contract. */
	if (status != 0)
		return status;

	/* Succeeded: the URL is reported. */
	return 0;
}

/* Reports the document's host name (domain). */
static int
document_domain(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The location's host name. */
	status = bind_location_part(bind_window_of(realm), BIND_LOCATION_HOSTNAME, result);
	if (status != 0)
		return status;

	/* Succeeded: the domain is reported. */
	return 0;
}

/* Reports the window's Location object (location). */
static int
document_location(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* A window made without its environment has no location. */
	window = bind_window_of(realm);
	if (window->location == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the window's location. */
	*result = vm_value_cell(window->location);
	return 0;
}

/* Reports the page that led to the document: none is known in this pass (referrer). */
static int
document_referrer(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The empty string. */
	status = document_constant_string(realm, this_value, "", result);
	if (status != 0)
		return status;

	/* Succeeded: the referrer is reported. */
	return 0;
}

/* Reports whether the document is hidden: the page is shown while it runs (hidden). */
static int
document_hidden(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Succeeded: not hidden. */
	*result = VM_VALUE_FALSE;
	return 0;
}

/* Reports the document's visibility: visible (visibilityState). */
static int
document_visibility_state(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The page is shown while it runs. */
	status = document_constant_string(realm, this_value, "visible", result);
	if (status != 0)
		return status;

	/* Succeeded: the state is reported. */
	return 0;
}

/* Reports the document's mode: CSS1Compat, or BackCompat in quirks mode (compatMode). */
static int
document_compat_mode(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	const char *mode;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Only full quirks mode is BackCompat; limited quirks is standards mode to scripts. */
	mode = "CSS1Compat";
	if (document->quirks == DOM_QUIRKS)
		mode = "BackCompat";
	status = bind_string(realm, mode, result);
	if (status != 0)
		return status;

	/* Succeeded: the mode is reported. */
	return 0;
}

/* Reports the document's encoding: UTF-8, the one the parser reads in this pass (characterSet). */
static int
document_character_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The encoding's name. */
	status = document_constant_string(realm, this_value, "UTF-8", result);
	if (status != 0)
		return status;

	/* Succeeded: the encoding is reported. */
	return 0;
}

/* Reports a fixed string for a document attribute (this must be a document). */
static int
document_constant_string(
	struct vm_realm *realm,
	vm_value this_value,
	const char *text,
	vm_value *result)
{
	struct dom_document *document;
	int status;

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The string. */
	status = bind_string(realm, text, result);
	if (status != 0)
		return status;

	/* Succeeded: the string is reported. */
	return 0;
}

/* Reports the document's window (defaultView). */
static int
document_default_view(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Inactive and script-created Documents have no browsing-context view. */
	window = document->view;
	*result = VM_VALUE_NULL;
	if (window != NULL && !window->detached)
		*result = vm_value_cell(window->realm->global);

	/* Succeeded: the actual active owner, or null, is reported. */
	return 0;
}

/* Finds the first element in tree order whose id is a string (getElementById). */
static int
document_get_element_by_id(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_node *walk;
	struct dom_attribute *attribute;
	struct vm_string *id;
	struct vm_string *name;
	struct vm_cell *id_root;
	int same;
	int status;

	/* The document, the id and the attribute's name. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &id);
	if (status != 0)
		return status;
	id_root = &id->cell;
	status = vm_heap_add_root(realm->heap, &id_root);
	if (status != 0)
		return status;
	name = vm_atom_from_ascii(realm->heap, "id");
	vm_heap_remove_root(realm->heap, &id_root);
	if (name == NULL)
		return ENOMEM;

	/* The first element whose id attribute has the value. */
	for (walk = bind_following(&document->node, &document->node);
	     walk != NULL;
	     walk = bind_following(walk, &document->node)) {
		if (walk->type != DOM_ELEMENT)
			continue;
		attribute = dom_element_find_attribute((struct dom_element *)walk, DOM_NS_NONE, name);
		if (attribute == NULL)
			continue;
		same = vm_string_equal(attribute->value, id);
		if (same)
			break;
	}

	/* The element, or null. */
	status = bind_wrap_or_null(bind_window_of(realm), walk, result);
	if (status != 0)
		return status;

	/* Succeeded: the element is reported. */
	return 0;
}

/* Creates an element using the owning Document's namespace and case policy. */
static int
document_create_element(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *element;
	struct vm_string *name;
	struct vm_cell *element_root;
	struct vm_cell *name_root;
	int valid;
	int lower;
	int ns;
	int status;

	/* The document and the name. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Only HTML documents fold their local names. */
	lower = 0;
	if (document->content == DOM_CONTENT_HTML)
		lower = 1;
	status = bind_to_atom(realm, js_argument(args, count, 0), lower, &name);
	if (status != 0)
		return status;

	/* Rejects malformed local names before allocating or publishing any node. */
	valid = document_element_name_valid(name);
	if (!valid) {
		status = bind_throw_dom(realm, "InvalidCharacterError", "The tag name provided is not a valid name.");
		return status;
	}

	/* XHTML documents keep the HTML namespace while ordinary XML uses none. */
	ns = DOM_NS_NONE;
	if (document->content == DOM_CONTENT_HTML || document->content == DOM_CONTENT_XHTML)
		ns = DOM_NS_HTML;
	name_root = &name->cell;
	status = vm_heap_add_root(document->heap, &name_root);
	if (status != 0)
		return status;
	element = dom_element_create(document, ns, name, NULL);
	vm_heap_remove_root(document->heap, &name_root);
	if (element == NULL)
		return ENOMEM;
	element_root = &element->node.cell;
	status = vm_heap_add_root(document->heap, &element_root);
	if (status != 0)
		return status;

	/* Its object. */
	status = bind_wrap(bind_window_of(realm), &element->node, result);
	vm_heap_remove_root(document->heap, &element_root);
	if (status != 0)
		return status;

	/* Succeeded: the element is made. */
	return 0;
}

/* Creates an element with an exact namespace, prefix and case-preserving local name. */
static int
document_create_element_ns(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_element *element;
	struct vm_string *uri;
	struct vm_string *qualified;
	struct vm_cell *uri_root;
	struct vm_cell *element_root;
	vm_value given;
	int status;

	/* Receiver branding precedes argument conversion or creation side effects. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* Both namespace and qualified name are required by the IDL operation. */
	if (count < 2) {
		status = vm_throw_type_error(realm, "createElementNS requires two arguments.");
		return status;
	}

	/* Nullable namespace conversion runs before qualified-name conversion. */
	uri = NULL;
	given = args[0];
	if (given != VM_VALUE_NULL && given != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, given, &uri);
		if (status != 0)
			return status;

		/* An empty namespace has the same identity as no namespace. */
		if (uri->length == 0)
			uri = NULL;
	}

	/* Namespaced names retain their case instead of applying HTML ASCII folding. */
	uri_root = NULL;
	if (uri != NULL)
		uri_root = &uri->cell;
	status = vm_heap_add_root(realm->heap, &uri_root);
	if (status != 0)
		return status;
	status = bind_to_atom(realm, args[1], 0, &qualified);
	vm_heap_remove_root(realm->heap, &uri_root);
	if (status != 0)
		return status;

	/* The factory and this API share all validation and reserved namespace rules. */
	status = bind_create_element_ns(realm, document, uri, qualified, &element);
	if (status != 0)
		return status;
	element_root = &element->node.cell;
	status = vm_heap_add_root(document->heap, &element_root);
	if (status != 0)
		return status;

	/* Publishes the wrapper only after complete namespace validation. */
	status = bind_wrap(bind_window_of(realm), &element->node, result);
	vm_heap_remove_root(document->heap, &element_root);
	if (status != 0)
		return status;

	/* Succeeded: the owner Document's namespaced element is available. */
	return 0;
}

/* Validates an element local name using the DOM name-validation branches. */
static int
document_element_name_valid(
	const struct vm_string *name)
{
	size_t index;
	uint16_t unit;
	int ascii_letter;

	/* Every element must have at least one name character. */
	if (name->length == 0)
		return 0;

	/* ASCII-letter starts use the HTML-compatible forbidden-character branch. */
	unit = vm_string_at(name, 0);
	ascii_letter = 0;
	if (unit >= 'a' && unit <= 'z') {
		ascii_letter = 1;
	} else if (unit >= 'A' && unit <= 'Z') {
		ascii_letter = 1;
	}

	/* Both cases of ASCII-letter starts use the same complete-string exclusions. */
	if (ascii_letter) {
		/* Scans the complete string, including embedded NULL rather than C terminators. */
		for (index = 0; index < name->length; index++) {
			unit = vm_string_at(name, index);
			if (unit == 0 ||
			    unit == '\t' ||
			    unit == '\n' ||
			    unit == '\f' ||
			    unit == '\r' ||
			    unit == ' ' ||
			    unit == '/' ||
			    unit == '>')
				return 0;
		}

		/* Succeeded: an ASCII-letter-start name contains no forbidden character. */
		return 1;
	}

	/* Other starts permit only colon, underscore or non-ASCII code points. */
	if (unit != ':' &&
	    unit != '_' &&
	    unit < 0x80U)
		return 0;

	/* Non-letter starts restrict subsequent ASCII characters to name characters. */
	for (index = 1; index < name->length; index++) {
		unit = vm_string_at(name, index);
		if (unit >= 0x80U)
			continue;

		/* Lowercase ASCII letters are legal after a non-letter start. */
		if (unit >= 'a' && unit <= 'z')
			continue;

		/* Namespaced APIs preserve uppercase ASCII letters as well. */
		if (unit >= 'A' && unit <= 'Z')
			continue;

		/* Digits may occur after the first character. */
		if (unit >= '0' && unit <= '9')
			continue;

		/* These punctuation characters are allowed in a name's suffix. */
		if (unit == '-' ||
		    unit == '.' ||
		    unit == ':' ||
		    unit == '_')
			continue;

		/* Every other ASCII character makes this branch's local name invalid. */
		return 0;
	}

	/* Succeeded: all characters meet the non-letter-start name rules. */
	return 1;
}

/* Validates the namespace prefix without applying local-name start restrictions. */
static int
document_prefix_valid(
	const struct vm_string *prefix)
{
	size_t index;
	uint16_t unit;

	/* A separator without any preceding prefix is invalid. */
	if (prefix->length == 0)
		return 0;

	/* These forbidden characters are checked without NULL-terminated conversion. */
	for (index = 0; index < prefix->length; index++) {
		unit = vm_string_at(prefix, index);
		if (unit == 0 ||
		    unit == '\t' ||
		    unit == '\n' ||
		    unit == '\f' ||
		    unit == '\r' ||
		    unit == ' ' ||
		    unit == '/' ||
		    unit == '>')
			return 0;
	}

	/* Succeeded: this prefix can participate in namespace extraction. */
	return 1;
}

/* Classifies known URI identities without aliasing arbitrary URIs to built-ins. */
static int
document_namespace_id(
	const struct vm_string *uri)
{
	/* Built-in namespace IDs preserve the parser's established classification. */
	static const char *const urls[] = {
		NULL,
		"http://www.w3.org/1999/xhtml",
		"http://www.w3.org/2000/svg",
		"http://www.w3.org/1998/Math/MathML",
		"http://www.w3.org/1999/xlink",
		"http://www.w3.org/XML/1998/namespace",
		"http://www.w3.org/2000/xmlns/"
	};
	unsigned index;
	int same;

	/* No namespace remains distinct from all nonempty URI strings. */
	if (uri == NULL)
		return DOM_NS_NONE;

	/* Compares exact, case-sensitive URI strings with the established built-ins. */
	for (index = DOM_NS_HTML; index <= DOM_NS_XMLNS; index++) {
		same = vm_string_equal_ascii(uri, urls[index]);
		if (same)
			return (int)index;
	}

	/* Succeeded: an arbitrary namespace keeps generic foreign-element behavior. */
	return DOM_NS_OTHER;
}

/* Makes a text node (createTextNode). */
static int
document_create_text_node(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* A text node of the document. */
	status = document_create_character_data(realm, this_value, DOM_TEXT, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is made. */
	return 0;
}

/* Makes a comment (createComment). */
static int
document_create_comment(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* A comment of the document. */
	status = document_create_character_data(realm, this_value, DOM_COMMENT, args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the node is made. */
	return 0;
}

/* Makes an empty document fragment (createDocumentFragment). */
static int
document_create_fragment(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_node *fragment;
	struct vm_cell *fragment_root;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The document. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;

	/* The fragment. */
	fragment = dom_fragment_create(document);
	if (fragment == NULL)
		return ENOMEM;
	fragment_root = &fragment->cell;
	status = vm_heap_add_root(document->heap, &fragment_root);
	if (status != 0)
		return status;

	/* Its object. */
	status = bind_wrap(bind_window_of(realm), fragment, result);
	vm_heap_remove_root(document->heap, &fragment_root);
	if (status != 0)
		return status;

	/* Succeeded: the fragment is made. */
	return 0;
}

/* Makes a text node or a comment of the document with a string's characters. */
static int
document_create_character_data(
	struct vm_realm *realm,
	vm_value this_value,
	int type,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_document *document;
	struct dom_node *node;
	struct vm_string *string;
	struct vm_cell *node_root;
	struct wb_units units;
	int status;

	/* The document and the characters. */
	status = document_this(realm, this_value, &document);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;
	wb_units_init(&units);
	status = vm_string_append_units(string, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* The node. */
	if (type == DOM_TEXT) {
		node = dom_text_create(document, units.data, units.length);
	} else {
		node = dom_comment_create(document, units.data, units.length);
	}

	/* The characters are in the node now. */
	wb_units_release(&units);
	if (node == NULL)
		return ENOMEM;
	node_root = &node->cell;
	status = vm_heap_add_root(document->heap, &node_root);
	if (status != 0)
		return status;

	/* Its object. */
	status = bind_wrap(bind_window_of(realm), node, result);
	vm_heap_remove_root(document->heap, &node_root);
	if (status != 0)
		return status;

	/* Succeeded: the node is made. */
	return 0;
}
