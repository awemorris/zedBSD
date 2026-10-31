/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Parser-inserted inline modules, deferred until the document is parsed. */

#include "page/page.h"
#include "net/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The synthetic root keeps an inline source distinct from every other root. */
struct page_inline_module {
	char *url;
	struct wb_units source;
	/* A root named by the script's src: fetched like any dependency, not held here. */
	int external;
};

static int module_resolve(void *context, const char *base, const uint16_t *specifier,
	size_t length, struct wb_buffer *url);
static int module_fetch(void *context, const char *url, struct wb_units *source);
static int module_fetch_json(void *context, const char *url, struct wb_units *source);
static int module_json_mime(const struct wb_buffer *type);
static int module_same_origin(const char *owner, const char *candidate);
static int module_javascript_mime(const struct wb_buffer *type);
static void module_report(struct page *page, const struct page_inline_module *entry,
	int status, const struct js_syntax_error *syntax);

/* Starts the page's ordered parser-inserted module list empty. */
void
page_modules_init(
	struct page *page)
{
	wb_vector_init(&page->module_scripts, sizeof(struct page_inline_module *));
	wb_vector_init(&page->deferred_scripts, sizeof(struct dom_element *));
}

/* Removes all module roots before the page's realm and heap are freed. */
void
page_modules_release(
	struct page *page)
{
	struct page_inline_module *entry;
	size_t index;

	js_module_graph_release(page->module_graph);
	page->module_graph = NULL;
	for (index = 0; index < page->module_scripts.count; index++) {
		entry = *(struct page_inline_module **)wb_vector_at(&page->module_scripts, index);
		wb_units_release(&entry->source);
		free(entry->url);
		free(entry);
	}
	wb_vector_release(&page->module_scripts);
	wb_vector_release(&page->deferred_scripts);
}

/* Saves one inline parser script's source without running it during parsing. */
int
page_module_enqueue(
	struct page *page,
	struct dom_element *script)
{
	struct page_inline_module *entry;
	const struct dom_character_data *data;
	struct dom_node *child;
	struct wb_buffer url;
	struct wb_buffer href;
	const char *base;
	const char *fragment;
	size_t index;
	int status;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return ENOMEM;
	wb_units_init(&entry->source);
	wb_buffer_init(&url);
	status = 0;

	/* An external root is the resolved src, which the graph fetches when it runs. */
	for (index = 0; index < script->attribute_count; index++) {
		if (script->attributes[index].ns == DOM_NS_NONE &&
		    vm_string_equal_ascii(script->attributes[index].name, "src")) {
			wb_buffer_init(&href);
			status = vm_string_to_utf8(script->attributes[index].value, &href);
			base = page->base;
			if (base == NULL)
				base = "about:blank";
			if (status == 0)
				status = page_resolve_location(base, wb_buffer_string(&href), &url);
			wb_buffer_release(&href);
			if (status == 0) {
				entry->url = strdup(wb_buffer_string(&url));
				entry->external = 1;
				if (entry->url == NULL)
					status = ENOMEM;
			}
			if (status == 0)
				status = wb_vector_push(&page->module_scripts, &entry);
			wb_buffer_release(&url);
			if (status != 0) {
				wb_units_release(&entry->source);
				free(entry->url);
				free(entry);
			}

			/* A src that does not resolve is no module to run. */
			if (status == EINVAL || status == ENOENT)
				return 0;
			return status;
		}
	}
	for (child = script->node.first_child; child != NULL && status == 0; child = child->next) {
		if (child->type != DOM_TEXT && child->type != DOM_CDATA_SECTION)
			continue;
		data = (const struct dom_character_data *)child;
		status = wb_units_append(&entry->source, data->data.data, data->data.length);
	}

	/* A fragment identifies each root while relative imports retain the document URL. */
	base = page->base;
	if (base == NULL)
		base = "about:blank";
	fragment = strchr(base, '#');
	if (status == 0 && fragment != NULL)
		status = wb_buffer_printf(&url, "%.*s#zedbsd-inline-module-%lu",
		    (int)(fragment - base), base, (unsigned long)page->module_scripts.count);
	else if (status == 0)
		status = wb_buffer_printf(&url, "%s#zedbsd-inline-module-%lu",
		    base, (unsigned long)page->module_scripts.count);
	if (status == 0) {
		entry->url = strdup(wb_buffer_string(&url));
		if (entry->url == NULL)
			status = ENOMEM;
	}
	if (status == 0)
		status = wb_vector_push(&page->module_scripts, &entry);
	wb_buffer_release(&url);
	if (status != 0) {
		wb_units_release(&entry->source);
		free(entry->url);
		free(entry);
	}
	return status;
}

/* Runs queued modules after parsing, then leaves their graph alive with the page. */
int
page_modules_run(
	struct page *page)
{
	struct page_inline_module *entry;
	struct js_syntax_error syntax;
	size_t index;
	int status;

	if (page->module_scripts.count == 0)
		return 0;
	if (page->module_graph == NULL) {
		status = js_module_graph_create(page->realm, module_resolve, module_fetch,
		    page, &page->module_graph);
		if (status != 0)
			return status;
		status = js_module_graph_set_json_fetch(page->module_graph, module_fetch_json);
		if (status != 0)
			return status;
	}
	for (index = 0; index < page->module_scripts.count; index++) {
		entry = *(struct page_inline_module **)wb_vector_at(&page->module_scripts, index);
		status = js_module_graph_run(page->module_graph, entry->url, &syntax);
		if (status == ENOMEM)
			return status;
		if (status != 0)
			module_report(page, entry, status, &syntax);
		status = bind_checkpoint(page->window);
		if (status != 0)
			return status;
	}
	return 0;
}

/* Loads a module for import(): through the page's graph (a SyntaxError or TypeError is thrown for a module that does not load). */
int
page_import_module(
	void *context,
	const char *url,
	int json,
	vm_value *namespace_object)
{
	struct js_syntax_error syntax;
	struct page *page;
	int status;

	page = context;
	*namespace_object = VM_VALUE_UNDEFINED;
	if (page->module_graph == NULL) {
		status = js_module_graph_create(page->realm, module_resolve, module_fetch,
		    page, &page->module_graph);
		if (status != 0)
			return status;
		status = js_module_graph_set_json_fetch(page->module_graph, module_fetch_json);
		if (status != 0)
			return status;
	}
	status = js_module_graph_import(page->module_graph, url, json, &syntax, namespace_object);
	if (status == 0 || status == VM_THROWN || status == ENOMEM)
		return status;
	if (status == EINVAL && syntax.message[0] != '\0')
		return vm_throw_error(page->realm, VM_ERROR_SYNTAX, syntax.message);
	return vm_throw_error(page->realm, VM_ERROR_TYPE, "Failed to fetch dynamically imported module");
}

/* Resolves a dependency against its owning module, within the document origin. */
static int
module_resolve(
	void *context,
	const char *base,
	const uint16_t *specifier,
	size_t length,
	struct wb_buffer *url)
{
	struct page *page;
	struct wb_buffer href;
	int status;

	page = context;
	wb_buffer_init(&href);
	status = wb_units_to_utf8(specifier, length, &href);
	if (status == 0)
		status = page_resolve_location(base, wb_buffer_string(&href), url);
	if (status == 0)
		status = module_same_origin(page->base, wb_buffer_string(url));
	wb_buffer_release(&href);
	return status;
}

/* Supplies an inline root or fetches an HTTP JavaScript dependency. */
static int
module_fetch(
	void *context,
	const char *url,
	struct wb_units *source)
{
	struct page_inline_module *entry;
	struct net_response response;
	struct page *page;
	const unsigned char *bytes;
	size_t length;
	size_t index;
	int status;

	page = context;
	for (index = 0; index < page->module_scripts.count; index++) {
		entry = *(struct page_inline_module **)wb_vector_at(&page->module_scripts, index);
		if (!entry->external && strcmp(entry->url, url) == 0)
			return wb_units_append(source, entry->source.data, entry->source.length);
	}

	status = module_same_origin(page->base, url);
	if (status != 0)
		return status;
	status = page_fetch_response(url, url, &response);
	if (status != 0)
		return status;
	if (response.status < 200 || response.status > 299 ||
	    strcmp(wb_buffer_string(&response.url), url) != 0 ||
	    !module_javascript_mime(&response.content_type)) {
		net_response_release(&response);
		return EINVAL;
	}
	bytes = response.body.data;
	length = response.body.length;
	if (length >= 3 && bytes[0] == 0xefU && bytes[1] == 0xbbU && bytes[2] == 0xbfU) {
		bytes += 3;
		length -= 3;
	}
	status = wb_utf8_to_units(bytes, length, source);
	net_response_release(&response);
	return status;
}

/* Fetches the text of a JSON module: a same-origin response with a JSON type. */
static int
module_fetch_json(
	void *context,
	const char *url,
	struct wb_units *source)
{
	struct net_response response;
	struct page *page;
	const unsigned char *bytes;
	size_t length;
	int status;

	/* Within the document's origin. */
	page = context;
	status = module_same_origin(page->base, url);
	if (status != 0)
		return status;

	/* A successful response of a JSON type. */
	status = page_fetch_response(url, url, &response);
	if (status != 0)
		return status;
	if (response.status < 200 || response.status > 299 || !module_json_mime(&response.content_type)) {
		net_response_release(&response);
		return EINVAL;
	}

	/* Its text, without a byte order mark. */
	bytes = response.body.data;
	length = response.body.length;
	if (length >= 3 && bytes[0] == 0xefU && bytes[1] == 0xbbU && bytes[2] == 0xbfU) {
		bytes += 3;
		length -= 3;
	}
	status = wb_utf8_to_units(bytes, length, source);
	net_response_release(&response);
	return status;
}

/* Recognizes a JSON MIME type: application/json, or any type that ends in +json. */
static int
module_json_mime(
	const struct wb_buffer *type)
{
	const unsigned char *bytes;
	size_t start;
	size_t end;
	size_t index;
	unsigned char unit;
	char lower[64];

	/* The essence, without parameters. */
	bytes = type->data;
	start = 0;
	while (start < type->length && (bytes[start] == ' ' || bytes[start] == '\t'))
		start++;
	end = start;
	while (end < type->length && bytes[end] != ';')
		end++;
	while (end > start && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t'))
		end--;
	if (end - start >= sizeof(lower))
		return 0;
	for (index = 0; index < end - start; index++) {
		unit = bytes[start + index];
		if (unit >= 'A' && unit <= 'Z')
			unit = (unsigned char)(unit + 0x20U);
		lower[index] = (char)unit;
	}
	lower[end - start] = '\0';
	if (strcmp(lower, "application/json") == 0)
		return 1;
	if (end - start > 5U && strcmp(lower + (end - start) - 5U, "+json") == 0)
		return 1;
	return 0;
}

/* Rejects a dependency outside the document's HTTP origin. */
static int
module_same_origin(
	const char *owner,
	const char *candidate)
{
	struct net_url document_url;
	struct net_url module_url;
	struct wb_buffer document_origin;
	struct wb_buffer module_origin;
	int status;

	if (owner == NULL || candidate == NULL)
		return EINVAL;
	memset(&document_url, 0, sizeof(document_url));
	memset(&module_url, 0, sizeof(module_url));
	wb_buffer_init(&document_origin);
	wb_buffer_init(&module_origin);
	status = net_url_parse(owner, strlen(owner), NULL, &document_url);
	if (status == 0)
		status = net_url_parse(candidate, strlen(candidate), NULL, &module_url);
	if (status == 0 && (!net_http_is_web(document_url.scheme) ||
	    !net_http_is_web(module_url.scheme)))
		status = EINVAL;
	if (status == 0)
		status = net_url_component(&document_url, NET_URL_ORIGIN, &document_origin);
	if (status == 0)
		status = net_url_component(&module_url, NET_URL_ORIGIN, &module_origin);
	if (status == 0 && (document_origin.length != module_origin.length ||
	    memcmp(document_origin.data, module_origin.data, document_origin.length) != 0))
		status = EACCES;
	wb_buffer_release(&document_origin);
	wb_buffer_release(&module_origin);
	net_url_release(&document_url);
	net_url_release(&module_url);
	return status;
}

/* Recognizes the JavaScript MIME essence, allowing ordinary parameters. */
static int
module_javascript_mime(
	const struct wb_buffer *type)
{
	static const char *const accepted[] = {
		"text/javascript", "application/javascript",
		"text/ecmascript", "application/ecmascript"
	};
	const unsigned char *bytes;
	size_t start;
	size_t end;
	size_t index;
	size_t kind;
	unsigned char unit;
	int same;

	bytes = type->data;
	start = 0;
	while (start < type->length && (bytes[start] == ' ' || bytes[start] == '\t'))
		start++;
	end = start;
	while (end < type->length && bytes[end] != ';')
		end++;
	while (end > start && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t'))
		end--;
	for (kind = 0; kind < sizeof(accepted) / sizeof(accepted[0]); kind++) {
		if (end - start != strlen(accepted[kind]))
			continue;
		same = 1;
		for (index = 0; index < end - start; index++) {
			unit = bytes[start + index];
			if (unit >= 'A' && unit <= 'Z')
				unit = (unsigned char)(unit + 0x20U);
			if (unit != (unsigned char)accepted[kind][index]) {
				same = 0;
				break;
			}
		}
		if (same)
			return 1;
	}
	return 0;
}

/* Reports a module failure without preventing the other scripts or load event. */
static void
module_report(
	struct page *page,
	const struct page_inline_module *entry,
	int status,
	const struct js_syntax_error *syntax)
{
	struct wb_buffer message;
	int written;

	if (status == VM_THROWN) {
		bind_report_exception_at(page->window, page->realm->exception, 1U, 1U);
		page->realm->exception = VM_VALUE_UNDEFINED;
		return;
	}
	wb_buffer_init(&message);
	if (status == EINVAL && syntax->message[0] != '\0') {
		/* A SyntaxError the window reports, with an error event. */
		(void)vm_throw_error(page->realm, VM_ERROR_SYNTAX, syntax->message);
		bind_report_exception_at(page->window, page->realm->exception, (unsigned)syntax->line, (unsigned)syntax->column);
		page->realm->exception = VM_VALUE_UNDEFINED;
		wb_buffer_release(&message);
		return;
	} else
		written = wb_buffer_printf(&message, "Failed to load module %s: %s",
		    entry->url, strerror(status));
	if (written == 0)
		bind_console(page->window, BIND_CONSOLE_ERROR, wb_buffer_string(&message));
	wb_buffer_release(&message);
}
