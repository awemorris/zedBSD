/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A page-owned graph of canonical module URLs, instances and dependencies. */

#include "js/js.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* One URL's module remains rooted while the graph owns it. */
struct js_module_graph_entry {
	struct js_module_graph_entry *next;
	char *url;
	/* Whether the module is a JSON module (its URL fetched as JSON and exported as default). */
	int json;
	struct js_module *module;
	struct js_module_graph_entry **dependencies;
	size_t dependency_count;
	int link_state;
	int eval_state;
	int failure;
};

/* The host owns URL resolution and source retrieval policy. */
struct js_module_graph {
	struct vm_realm *realm;
	js_module_resolve resolve;
	js_module_fetch fetch;
	js_module_fetch fetch_json;
	void *context;
	struct js_module_graph_entry *entries;
};

static struct js_module_graph_entry *module_graph_find(struct js_module_graph *graph,
	const char *url, int json);
static int module_graph_get(struct js_module_graph *graph, const char *url, int json,
	struct js_module_graph_entry **entry, struct js_syntax_error *error);
static int module_graph_json_source(struct wb_units *source);
static int module_graph_link(struct js_module_graph *graph,
	struct js_module_graph_entry *entry, struct js_syntax_error *error);
static int module_graph_evaluate(struct js_module_graph_entry *entry);

/* Creates a graph; callbacks and their context must outlive it. */
int
js_module_graph_create(
	struct vm_realm *realm,
	js_module_resolve resolve,
	js_module_fetch fetch,
	void *context,
	struct js_module_graph **graph)
{
	struct js_module_graph *created;

	*graph = NULL;
	if (realm == NULL || resolve == NULL || fetch == NULL)
		return EINVAL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->realm = realm;
	created->resolve = resolve;
	created->fetch = fetch;
	created->context = context;
	*graph = created;
	return 0;
}

/* Loads, links and evaluates one canonical root and its dependencies. */
int
js_module_graph_run(
	struct js_module_graph *graph,
	const char *url,
	struct js_syntax_error *error)
{
	struct js_module_graph_entry *entry;
	int status;

	if (graph == NULL || url == NULL || url[0] == '\0' || error == NULL)
		return EINVAL;
	memset(error, 0, sizeof(*error));
	status = module_graph_get(graph, url, 0, &entry, error);
	if (status == 0)
		status = module_graph_link(graph, entry, error);
	if (status == 0)
		status = module_graph_evaluate(entry);
	return status;
}

/* Releases all instances before their realm and heap are destroyed. */
void
js_module_graph_release(
	struct js_module_graph *graph)
{
	struct js_module_graph_entry *entry;
	struct js_module_graph_entry *next;

	if (graph == NULL)
		return;
	for (entry = graph->entries; entry != NULL; entry = next) {
		next = entry->next;
		js_module_release(entry->module);
		free(entry->dependencies);
		free(entry->url);
		free(entry);
	}
	free(graph);
}

/* Finds one canonical URL already owned by the graph. */
static struct js_module_graph_entry *
module_graph_find(
	struct js_module_graph *graph,
	const char *url,
	int json)
{
	struct js_module_graph_entry *entry;

	for (entry = graph->entries; entry != NULL; entry = entry->next) {
		if (entry->json == json && strcmp(entry->url, url) == 0)
			return entry;
	}
	return NULL;
}

/* Fetches and instantiates a URL once; a failed entry retains its error. */
static int
module_graph_get(
	struct js_module_graph *graph,
	const char *url,
	int json,
	struct js_module_graph_entry **entry,
	struct js_syntax_error *error)
{
	struct js_module_graph_entry *found;
	struct wb_units source;
	int status;

	*entry = NULL;
	found = module_graph_find(graph, url, json);
	if (found != NULL) {
		*entry = found;
		return found->failure;
	}

	/* The entry is visible to cycle traversal after instantiation succeeds. */
	found = calloc(1, sizeof(*found));
	if (found == NULL)
		return ENOMEM;
	found->url = strdup(url);
	if (found->url == NULL) {
		free(found);
		return ENOMEM;
	}
	found->json = json;
	wb_units_init(&source);
	if (json && graph->fetch_json == NULL)
		status = ENOTSUP;
	else if (json)
		status = graph->fetch_json(graph->context, url, &source);
	else
		status = graph->fetch(graph->context, url, &source);
	if (status == 0 && json)
		status = module_graph_json_source(&source);
	if (status == 0)
		status = js_module_create(graph->realm, source.data, source.length,
		    &found->module, error);
	wb_units_release(&source);
	if (status != 0) {
		free(found->url);
		free(found);
		return status;
	}
	found->dependency_count = js_module_import_count(found->module);
	if (found->dependency_count != 0) {
		found->dependencies = calloc(found->dependency_count,
		    sizeof(*found->dependencies));
		if (found->dependencies == NULL) {
			js_module_release(found->module);
			free(found->url);
			free(found);
			return ENOMEM;
		}
	}
	found->next = graph->entries;
	graph->entries = found;
	*entry = found;
	return 0;
}

/* Resolves every import relative to its owner, recursively linking the graph. */
static int
module_graph_link(
	struct js_module_graph *graph,
	struct js_module_graph_entry *entry,
	struct js_syntax_error *error)
{
	struct js_module_graph_entry *target;
	struct js_module_import_info info;
	struct wb_buffer url;
	size_t index;
	int json;
	int status;

	if (entry->link_state == 2 || entry->link_state == 1)
		return 0;
	if (entry->link_state < 0)
		return entry->failure;
	entry->link_state = 1;
	status = 0;
	for (index = 0; index < entry->dependency_count; index++) {
		status = js_module_import_at(entry->module, index, &info);
		if (status != 0)
			break;

		/* An attribute other than type is a SyntaxError; a type the host does not know is a TypeError. */
		if (info.unknown_key) {
			status = vm_throw_error(graph->realm, VM_ERROR_SYNTAX, "Invalid import attribute");
			break;
		}
		json = 0;
		if (info.has_type) {
			json = info.type_length == 4U && info.type[0] == 'j' && info.type[1] == 's' &&
			    info.type[2] == 'o' && info.type[3] == 'n';
			if (!json) {
				status = vm_throw_error(graph->realm, VM_ERROR_TYPE, "Invalid module type attribute");
				break;
			}
		}

		/* The module the specifier names. */
		wb_buffer_init(&url);
		status = graph->resolve(graph->context, entry->url, info.specifier,
		    info.specifier_length, &url);
		if (status == 0 && (url.length == 0 || wb_buffer_string(&url)[0] == '\0'))
			status = EINVAL;
		if (status == 0)
			status = module_graph_get(graph, wb_buffer_string(&url), json, &target, error);
		wb_buffer_release(&url);
		if (status != 0)
			break;
		entry->dependencies[index] = target;
		status = module_graph_link(graph, target, error);
		if (status != 0)
			break;
		if (info.local_length == 0U)
			continue;
		status = js_module_link_import(entry->module, info.specifier,
		    info.specifier_length, info.local, info.local_length, target->module,
		    info.imported, info.imported_length);
		if (status != 0)
			break;
	}
	if (status != 0) {
		entry->failure = status;
		entry->link_state = -1;
		return status;
	}
	entry->link_state = 2;
	return 0;
}

/* Evaluates children before their importer, with cycles visiting each once. */
static int
module_graph_evaluate(
	struct js_module_graph_entry *entry)
{
	size_t index;
	int status;

	if (entry->eval_state == 2 || entry->eval_state == 1)
		return 0;
	if (entry->eval_state < 0)
		return entry->failure;
	entry->eval_state = 1;
	for (index = 0; index < entry->dependency_count; index++) {
		status = module_graph_evaluate(entry->dependencies[index]);
		if (status != 0) {
			entry->failure = status;
			entry->eval_state = -1;
			return status;
		}
	}
	status = js_module_evaluate(entry->module);
	if (status != 0) {
		entry->failure = status;
		entry->eval_state = -1;
		return status;
	}
	entry->eval_state = 2;
	return 0;
}

/* Wraps a JSON text as a module: export default <the text>; */
static int
module_graph_json_source(
	struct wb_units *source)
{
	static const char prefix[] = "export default ";
	struct wb_units wrapped;
	uint16_t unit;
	size_t index;
	int status;

	/* The prefix, the text, the end. */
	wb_units_init(&wrapped);
	status = 0;
	for (index = 0; index + 1U < sizeof(prefix) && status == 0; index++) {
		unit = (uint16_t)prefix[index];
		status = wb_units_append(&wrapped, &unit, 1U);
	}
	if (status == 0)
		status = wb_units_append(&wrapped, source->data, source->length);
	if (status == 0) {
		unit = ';';
		status = wb_units_append(&wrapped, &unit, 1U);
	}
	if (status == 0) {
		wb_units_release(source);
		*source = wrapped;
	} else {
		wb_units_release(&wrapped);
	}
	return status;
}

/* Gives the graph the host's way to fetch a JSON module's text. */
int
js_module_graph_set_json_fetch(
	struct js_module_graph *graph,
	js_module_fetch fetch)
{
	if (graph == NULL)
		return EINVAL;
	graph->fetch_json = fetch;
	return 0;
}
