/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Selectors API, classList and dataset (ws074-p031).
 *
 * querySelector and querySelectorAll (of documents, fragments and
 * elements), matches and closest parse the script's selector list with
 * the style engine's parser and match it with the cascade's matching, in
 * a style engine the host keeps for scripts.  querySelectorAll's list is
 * static, an array like getElementsByTagName's.
 *
 * classList is a DOMTokenList over the element's class attribute: each
 * change reads the attribute's words, changes the set and writes it back.
 * dataset is a DOMStringMap with an accessor for each data-* attribute the
 * element has when it is asked for; the engine has no exotic objects, so a
 * name set on it that the element did not have does not become an
 * attribute.  A new list and a new map are made each time they are asked
 * for.
 */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The prefix of the attributes dataset reflects. */
#define QUERY_DATA_PREFIX	"data-"

/* The length of that prefix. */
#define QUERY_DATA_PREFIX_LENGTH	5U

static int token_list_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_value_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_value_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_item(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_contains(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_add(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_remove(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_toggle(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int token_list_replace(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of DOMTokenList.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute token_list_attributes[] = {
	{ "length", token_list_length, NULL },
	{ "value", token_list_value_get, token_list_value_set },
	{ NULL, NULL, NULL }
};

/*
 * The operations of DOMTokenList.  The table is constant for the life of
 * the program.
 */
static const struct bind_operation token_list_operations[] = {
	{ "item", 1, token_list_item },
	{ "contains", 1, token_list_contains },
	{ "add", 0, token_list_add },
	{ "remove", 0, token_list_remove },
	{ "toggle", 1, token_list_toggle },
	{ "replace", 2, token_list_replace },
	{ "toString", 0, token_list_value_get },
	{ NULL, 0, NULL }
};

/*
 * The DOMTokenList interface (classList).
 */
const struct bind_interface bind_dom_token_list_interface = {
	"DOMTokenList", BIND_NO_PARENT, 0, NULL, token_list_attributes, token_list_operations, NULL
};

/*
 * The DOMStringMap interface (dataset), whose members are the accessors
 * each map is made with.
 */
const struct bind_interface bind_dom_string_map_interface = {
	"DOMStringMap", BIND_NO_PARENT, 0, NULL, NULL, NULL, NULL
};

static int query_parse(struct vm_realm *realm, vm_value argument, const char *method, struct css_query **query);
static int query_engine(struct bind_window *window, struct css_engine **engine);
static int query_root(struct vm_realm *realm, vm_value this_value, struct dom_node **root);
static int query_element_this(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int query_find(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, int all, vm_value *result);
static int token_list_element(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int token_list_read(struct vm_realm *realm, struct dom_element *element, struct vm_object **tokens);
static int token_list_write(struct vm_realm *realm, struct dom_element *element, struct vm_object *tokens);
static int token_list_index(const struct vm_object *tokens, const struct vm_string *token, uint32_t *index);
static int token_list_append(struct vm_realm *realm, struct vm_object *tokens, struct vm_string *token);
static int token_list_remove_at(struct vm_realm *realm, struct vm_object *tokens, uint32_t index);
static int token_list_token(struct vm_realm *realm, vm_value value, struct vm_string **token);
static int string_map_accessor(struct vm_realm *realm, struct vm_object *map, const struct dom_attribute *attribute);
static int string_map_name(struct vm_realm *realm, const struct vm_string *attribute, struct vm_string **name);
static int string_map_element(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int string_map_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int string_map_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int query_is_space(uint16_t unit);
static int query_has_prefix(const struct vm_string *string, const char *ascii);

/*
 * Finds the first element a selector list matches among the descendants
 * of a document, fragment or element (querySelector), or null.
 */
int
bind_query_selector(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The first match. */
	status = query_find(realm, this_value, args, count, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the element or null. */
	return 0;
}

/*
 * Lists every element a selector list matches among the descendants of a
 * document, fragment or element, in tree order (querySelectorAll).
 */
int
bind_query_selector_all(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* Every match. */
	status = query_find(realm, this_value, args, count, 1, result);
	if (status != 0)
		return status;

	/* Succeeded: the list. */
	return 0;
}

/*
 * Tells whether an element matches a selector list (matches and
 * webkitMatchesSelector).
 */
int
bind_matches(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct css_engine *engine;
	struct css_query *query;
	int matches;
	int status;

	/* The element. */
	window = bind_window_of(realm);
	status = query_element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* Selects the receiver's owner rather than the borrowed method's realm. */
	if (element->node.document->view != NULL)
		window = element->node.document->view;

	/* The selector list, which must parse. */
	status = query_parse(realm, js_argument(args, count, 0), "matches", &query);
	if (status != 0)
		return status;

	/* The engine to match with. */
	status = query_engine(window, &engine);
	if (status != 0) {
		css_query_destroy(query);
		return status;
	}

	/* The element itself (and :scope); without an engine nothing matches. */
	matches = 0;
	if (engine != NULL) {
		css_engine_query_scope(engine, element);
		matches = css_engine_query_matches(engine, element, query);
		css_engine_query_scope(engine, NULL);
	}
	css_query_destroy(query);

	/* Succeeded: whether it matches. */
	*result = vm_value_boolean(matches);
	return 0;
}

/*
 * Finds the nearest inclusive ancestor of an element that matches a
 * selector list (closest), or null.
 */
int
bind_closest(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct css_engine *engine;
	struct css_query *query;
	struct dom_node *walk;
	int matches;
	int status;

	/* The element. */
	window = bind_window_of(realm);
	status = query_element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* Selects the receiver's owner rather than the borrowed method's realm. */
	if (element->node.document->view != NULL)
		window = element->node.document->view;

	/* The selector list, which must parse. */
	status = query_parse(realm, js_argument(args, count, 0), "closest", &query);
	if (status != 0)
		return status;

	/* The engine to match with. */
	status = query_engine(window, &engine);
	if (status != 0) {
		css_query_destroy(query);
		return status;
	}

	/* The element, then each ancestor element, until one matches (:scope is the element). */
	matches = 0;
	walk = &element->node;
	if (engine != NULL)
		css_engine_query_scope(engine, element);
	while (engine != NULL && walk != NULL) {
		if (walk->type == DOM_ELEMENT) {
			matches = css_engine_query_matches(engine, (struct dom_element *)walk, query);
			if (matches)
				break;
		}

		/* The parent. */
		walk = walk->parent;
	}

	/* The list is done with. */
	css_query_destroy(query);
	if (engine != NULL)
		css_engine_query_scope(engine, NULL);

	/* None matches. */
	if (!matches) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the nearest one that matches. */
	status = bind_wrap(window, walk, result);
	if (status != 0)
		return status;
	return 0;
}

/*
 * Makes the DOMTokenList of an element's class attribute (classList).
 */
int
bind_class_list(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct vm_object *list;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	window = bind_window_of(realm);
	status = query_element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* The list, which keeps the element's object. */
	list = vm_object_create(realm->heap, window->prototypes[BIND_DOM_TOKEN_LIST]);
	if (list == NULL)
		return ENOMEM;
	list->kind = VM_KIND_PLATFORM;
	list->internal = this_value;

	/* Succeeded: the list. */
	*result = vm_value_cell(list);
	return 0;
}

/*
 * Makes the DOMStringMap of an element's data-* attributes (dataset), with
 * an accessor for each of them.
 */
int
bind_dataset(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_object *map;
	struct vm_cell *map_root;
	size_t index;
	int prefixed;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	window = bind_window_of(realm);
	status = query_element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* The map, which keeps the element's object. */
	map = vm_object_create(realm->heap, window->prototypes[BIND_DOM_STRING_MAP]);
	if (map == NULL)
		return ENOMEM;
	map->kind = VM_KIND_PLATFORM;
	map->internal = this_value;
	map_root = &map->cell;
	status = vm_heap_add_root(realm->heap, &map_root);
	if (status != 0)
		return status;

	/* An accessor for each data-* attribute without a namespace, in the attributes' order. */
	for (index = 0; index < element->attribute_count; index++) {
		attribute = &element->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;

		/* Only a name that starts with the prefix (which may be all of it). */
		prefixed = 0;
		if (attribute->name->length >= QUERY_DATA_PREFIX_LENGTH)
			prefixed = query_has_prefix(attribute->name, QUERY_DATA_PREFIX);
		if (!prefixed)
			continue;

		/* The attribute's accessor. */
		status = string_map_accessor(realm, map, attribute);
		if (status != 0) {
			vm_heap_remove_root(realm->heap, &map_root);
			return status;
		}
	}

	/* Succeeded: the map. */
	*result = vm_value_cell(map);
	vm_heap_remove_root(realm->heap, &map_root);
	return 0;
}

/*
 * Finds the first match, or every match, of a selector list among the
 * descendants of a document, fragment or element.
 */
static int
query_find(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	int all,
	vm_value *result)
{
	struct bind_window *window;
	struct css_engine *engine;
	struct css_query *query;
	struct vm_object *array;
	struct vm_cell *array_root;
	struct dom_node *root;
	struct dom_node *walk;
	const char *method;
	int matches;
	int status;

	/* The node whose descendants are searched. */
	window = bind_window_of(realm);
	status = query_root(realm, this_value, &root);
	if (status != 0)
		return status;

	/* Selects the receiver's owner rather than the borrowed method's realm. */
	if (root->document->view != NULL)
		window = root->document->view;

	/* The selector list, which must parse. */
	method = "querySelector";
	if (all)
		method = "querySelectorAll";
	status = query_parse(realm, js_argument(args, count, 0), method, &query);
	if (status != 0)
		return status;

	/* The engine to match with. */
	status = query_engine(window, &engine);
	if (status != 0) {
		css_query_destroy(query);
		return status;
	}

	/* An element's query has the element as :scope. */
	if (engine != NULL && root->type == DOM_ELEMENT)
		css_engine_query_scope(engine, (struct dom_element *)root);

	/* The list querySelectorAll fills. */
	array = NULL;
	array_root = NULL;
	if (all) {
		status = bind_array_create(realm, &array);
		if (status != 0) {
			css_query_destroy(query);
			return status;
		}

		/* The partial result remains live while each matched node is wrapped. */
		array_root = &array->cell;
		status = vm_heap_add_root(realm->heap, &array_root);
		if (status != 0) {
			css_query_destroy(query);
			return status;
		}
	}

	/* Each descendant element in tree order; without an engine nothing matches. */
	walk = NULL;
	if (engine != NULL)
		walk = bind_following(root, root);
	while (walk != NULL) {
		matches = 0;
		if (walk->type == DOM_ELEMENT)
			matches = css_engine_query_matches(engine, (struct dom_element *)walk, query);

		/* The first match ends querySelector's search. */
		if (matches && !all)
			break;

		/* querySelectorAll lists every match. */
		if (matches) {
			status = bind_array_push_node(window, array, walk);
			if (status != 0) {
				vm_heap_remove_root(realm->heap, &array_root);
				css_query_destroy(query);
				return status;
			}
		}

		/* The next node. */
		walk = bind_following(walk, root);
	}

	/* The list is done with. */
	css_query_destroy(query);

	/* querySelectorAll's list. */
	if (all) {
		*result = vm_value_cell(array);
		vm_heap_remove_root(realm->heap, &array_root);
		return 0;
	}

	/* Succeeded: querySelector's element, or null. */
	status = bind_wrap_or_null(window, walk, result);
	if (status != 0)
		return status;
	return 0;
}

/*
 * Parses a script's selector list; one that does not parse throws a
 * SyntaxError DOMException.
 */
static int
query_parse(
	struct vm_realm *realm,
	vm_value argument,
	const char *method,
	struct css_query **query)
{
	struct vm_string *text;
	struct vm_cell *text_root;
	struct wb_units units;
	struct wb_buffer message;
	int status;

	/* The argument's characters. */
	status = bind_to_string(realm, argument, &text);
	if (status != 0)
		return status;
	text_root = &text->cell;
	status = vm_heap_add_root(realm->heap, &text_root);
	if (status != 0)
		return status;
	wb_units_init(&units);
	status = vm_string_append_units(text, &units);
	if (status != 0) {
		wb_units_release(&units);
		vm_heap_remove_root(realm->heap, &text_root);
		return status;
	}

	/* The list. */
	status = css_query_parse(realm->heap, units.data, units.length, query);
	wb_units_release(&units);
	if (status == 0) {
		vm_heap_remove_root(realm->heap, &text_root);
		return 0;
	}

	/* Anything but a syntax error is the engine's failure. */
	if (status != EINVAL) {
		vm_heap_remove_root(realm->heap, &text_root);
		return status;
	}

	/* The message names the method and the text, as other browsers do. */
	wb_buffer_init(&message);
	status = wb_buffer_printf(&message, "Failed to execute '%s': '", method);
	if (status == 0)
		status = vm_string_to_utf8(text, &message);
	if (status == 0)
		status = wb_buffer_append_string(&message, "' is not a valid selector.");
	if (status != 0) {
		wb_buffer_release(&message);
		vm_heap_remove_root(realm->heap, &text_root);
		return status;
	}

	/* The exception. */
	status = bind_throw_dom(realm, "SyntaxError", wb_buffer_string(&message));
	wb_buffer_release(&message);
	vm_heap_remove_root(realm->heap, &text_root);
	return status;
}

/*
 * Finds the style engine a script's selectors are matched with, readied
 * for a new query; NULL when the host has none.
 */
static int
query_engine(
	struct bind_window *window,
	struct css_engine **engine)
{
	int error;

	/* Initial children use their own cascade when no page host was installed. */
	*engine = NULL;
	if (window->host.selector_engine == NULL) {
		error = bind_style_context_engine(window, engine);
		if (error != 0)
			return error;
	} else {
		/* A primary host's missing engine reports failed preparation. */
		*engine = window->host.selector_engine(window->host.context);
		if (*engine == NULL)
			return ENOMEM;
	}

	/* An unsupported empty host or retired child matches nothing. */
	if (*engine == NULL)
		return 0;

	/* Succeeded: the engine forgets class attributes split by an earlier query. */
	css_engine_query_begin(*engine);
	return 0;
}

/* Finds the document, fragment or element a query method's this value stands for. */
static int
query_root(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_node **root)
{
	struct dom_node *node;
	int status;

	/* The node, which must be one of the three. */
	*root = NULL;
	node = bind_node_of(this_value);
	if (node == NULL) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A node of another kind has no descendants to search. */
	if (node->type != DOM_DOCUMENT &&
	    node->type != DOM_DOCUMENT_FRAGMENT &&
	    node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the root. */
	*root = node;
	return 0;
}

/* Finds the element a method's this value stands for, throwing a TypeError otherwise. */
static int
query_element_this(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	struct dom_node *node;
	int status;

	/* The node, which must be an element. */
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the element. */
	*element = (struct dom_element *)node;
	return 0;
}

/* Finds the element of the DOMTokenList a method's this value stands for. */
static int
token_list_element(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	struct vm_object *object;
	struct dom_node *node;
	int is_object;
	int status;

	/* A platform object with the list's prototype. */
	is_object = vm_value_is_object(this_value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One the binding made. */
	object = (struct vm_object *)vm_value_as_cell(this_value);
	if (object->kind != VM_KIND_PLATFORM) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Its element's object. */
	node = bind_node_of(object->internal);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the element. */
	*element = (struct dom_element *)node;
	return 0;
}

/* Reads an element's class words into a new array, each once, in order. */
static int
token_list_read(
	struct vm_realm *realm,
	struct dom_element *element,
	struct vm_object **tokens)
{
	struct dom_attribute *attribute;
	struct vm_object *words;
	struct vm_object *unique;
	struct vm_cell *roots[2];
	struct vm_string *class_atom;
	struct vm_string *word;
	uint32_t index;
	uint32_t found;
	unsigned registered;
	unsigned slot;
	int present;
	int status;

	/* The words of the attribute, when there is one. */
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0;
	for (slot = 0; slot < 2U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The first array retains all parsed words while the unique set is built. */
	status = bind_array_create(realm, &words);
	if (status != 0)
		goto cleanup;
	roots[0] = &words->cell;
	class_atom = vm_atom_from_ascii(realm->heap, "class");
	if (class_atom == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Existing class text supplies the words to split. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, class_atom);
	if (attribute != NULL) {
		status = bind_split_classes(realm, attribute->value, words);
		if (status != 0)
			goto cleanup;
	}

	/* The set: each word once, where it first appears. */
	status = bind_array_create(realm, &unique);
	if (status != 0)
		goto cleanup;
	roots[1] = &unique->cell;
	for (index = 0; index < words->length; index++) {
		word = (struct vm_string *)vm_value_as_cell(words->elements[index]);
		present = token_list_index(unique, word, &found);
		if (present)
			continue;

		/* A word not seen before. */
		status = token_list_append(realm, unique, word);
		if (status != 0)
			goto cleanup;
	}

	/* Succeeded: the set. */
	*tokens = unique;

cleanup:
	/* The caller receives the completed set after temporary roots end. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Return the parser or allocation result after releasing the roots. */
	return status;
}

/* Writes a set of words to an element's class attribute, separated by spaces. */
static int
token_list_write(
	struct vm_realm *realm,
	struct dom_element *element,
	struct vm_object *tokens)
{
	static const uint16_t space = 0x20U;
	struct vm_string *class_atom;
	struct vm_string *value;
	struct vm_string *word;
	struct vm_cell *roots[2];
	struct wb_units units;
	uint32_t index;
	unsigned registered;
	unsigned slot;
	int status;

	/* The words joined by single spaces. */
	roots[0] = &tokens->cell;
	roots[1] = NULL;
	registered = 0;
	for (slot = 0; slot < 2U; slot++) {
		status = vm_heap_add_root(realm->heap, &roots[slot]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Copy the rooted words into independent UTF16 storage. */
	wb_units_init(&units);
	status = 0;
	for (index = 0; index < tokens->length && status == 0; index++) {
		word = (struct vm_string *)vm_value_as_cell(tokens->elements[index]);
		if (index > 0)
			status = wb_units_append(&units, &space, 1);
		if (status == 0)
			status = vm_string_append_units(word, &units);
	}

	/* A text that could not grow. */
	if (status != 0) {
		wb_units_release(&units);
		goto cleanup;
	}

	/* The attribute's new value. */
	value = vm_string_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (value == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Retain the new string through atom lookup and attribute publication. */
	roots[1] = &value->cell;

	/* The attribute takes it. */
	class_atom = vm_atom_from_ascii(realm->heap, "class");
	if (class_atom == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* Publish the joined text on its owner element. */
	status = dom_element_set_attribute(element, class_atom, value);

cleanup:
	/* A completed attribute owns the value after these temporary roots end. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* An error leaves the old attribute untouched. */
	if (status != 0)
		return status;

	/* Succeeded: the attribute holds the set. */
	return 0;
}

/* Tells whether a set holds a word, and where. */
static int
token_list_index(
	const struct vm_object *tokens,
	const struct vm_string *token,
	uint32_t *index)
{
	const struct vm_string *word;
	uint32_t at;
	int same;

	/* Compares each word. */
	for (at = 0; at < tokens->length; at++) {
		word = (const struct vm_string *)vm_value_as_cell(tokens->elements[at]);
		same = vm_string_equal(word, token);
		if (same) {
			*index = at;
			return 1;
		}
	}

	/* Succeeded: the set does not hold it. */
	return 0;
}

/* Appends a word to a set. */
static int
token_list_append(
	struct vm_realm *realm,
	struct vm_object *tokens,
	struct vm_string *token)
{
	struct vm_cell *roots[2];
	int status;

	/* At the end. */
	roots[0] = &tokens->cell;
	roots[1] = &token->cell;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &roots[0]);
		return status;
	}

	/* The target array and word are both protected during property insertion. */
	status = vm_object_define(realm->heap, tokens, vm_value_int32((int32_t)tokens->length), vm_value_cell(token), VM_PROPERTY_DEFAULT);
	vm_heap_remove_root(realm->heap, &roots[1]);
	vm_heap_remove_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;

	/* Succeeded: the word is in the set. */
	return 0;
}

/* Removes the word at a place of a set, moving the ones after it down. */
static int
token_list_remove_at(
	struct vm_realm *realm,
	struct vm_object *tokens,
	uint32_t index)
{
	struct vm_cell *root;
	uint32_t at;
	int status;

	/* Each later word one place down. */
	root = &tokens->cell;
	status = vm_heap_add_root(realm->heap, &root);
	if (status != 0)
		return status;
	for (at = index; at + 1U < tokens->length; at++)
		tokens->elements[at] = tokens->elements[at + 1U];

	/* The set is one shorter. */
	status = vm_array_set_length(realm->heap, tokens, tokens->length - 1U);
	vm_heap_remove_root(realm->heap, &root);
	if (status != 0)
		return status;

	/* Succeeded: the word is gone. */
	return 0;
}

/*
 * Reads a token argument: an empty one throws a SyntaxError, and one with
 * ASCII whitespace an InvalidCharacterError.
 */
static int
token_list_token(
	struct vm_realm *realm,
	vm_value value,
	struct vm_string **token)
{
	size_t index;
	uint16_t unit;
	int space;
	int status;

	/* The string. */
	status = bind_to_string(realm, value, token);
	if (status != 0)
		return status;

	/* An empty token is not one. */
	if ((*token)->length == 0) {
		status = bind_throw_dom(realm, "SyntaxError", "The token provided must not be empty.");
		return status;
	}

	/* Nor is one with whitespace in it. */
	for (index = 0; index < (*token)->length; index++) {
		unit = vm_string_at(*token, index);
		space = query_is_space(unit);
		if (space) {
			status = bind_throw_dom(realm, "InvalidCharacterError", "The token provided contains HTML space characters, which are not valid in tokens.");
			return status;
		}
	}

	/* Succeeded: the token. */
	return 0;
}

/* Reports how many words the set has (length). */
static int
token_list_length(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element's set. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = token_list_read(realm, element, &tokens);
	if (status != 0)
		return status;

	/* Succeeded: its size. */
	*result = vm_value_int32((int32_t)tokens->length);
	return 0;
}

/* Reports the class attribute's value, or the empty string (value and toString). */
static int
token_list_value_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_string *class_atom;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element and its attribute. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	class_atom = vm_atom_from_ascii(realm->heap, "class");
	if (class_atom == NULL)
		return ENOMEM;
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, class_atom);

	/* No attribute is the empty string. */
	if (attribute == NULL) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* Succeeded: the value as it is. */
	*result = vm_value_cell(attribute->value);
	return 0;
}

/* Sets the class attribute to a value's string (value). */
static int
token_list_value_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *class_atom;
	struct vm_string *value;
	struct vm_cell *value_root;
	int status;

	/* The element and the new value. */
	*result = VM_VALUE_UNDEFINED;
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &value);
	if (status != 0)
		return status;
	value_root = &value->cell;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0)
		return status;

	/* The attribute takes it as it is. */
	class_atom = vm_atom_from_ascii(realm->heap, "class");
	if (class_atom == NULL) {
		vm_heap_remove_root(realm->heap, &value_root);
		return ENOMEM;
	}

	/* The value remains protected until the element accepts it. */
	status = dom_element_set_attribute(element, class_atom, value);
	vm_heap_remove_root(realm->heap, &value_root);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Reports the word at a place of the set, or null past its end (item). */
static int
token_list_item(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	uint32_t index;
	int status;

	/* The element's set and the place. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, js_argument(args, count, 0), &index);
	if (status != 0)
		return status;
	status = token_list_read(realm, element, &tokens);
	if (status != 0)
		return status;

	/* A place past the end has no word. */
	if (index >= tokens->length) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the word. */
	*result = tokens->elements[index];
	return 0;
}

/* Tells whether the set has a word (contains). */
static int
token_list_contains(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	struct vm_string *token;
	struct vm_cell *token_root;
	uint32_t index;
	int present;
	int status;

	/* The element's set and the word, which is not checked. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &token);
	if (status != 0)
		return status;
	token_root = &token->cell;
	status = vm_heap_add_root(realm->heap, &token_root);
	if (status != 0)
		return status;
	status = token_list_read(realm, element, &tokens);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &token_root);
		return status;
	}

	/* Succeeded: whether it is there. */
	present = token_list_index(tokens, token, &index);
	vm_heap_remove_root(realm->heap, &token_root);
	*result = vm_value_boolean(present);
	return 0;
}

/* Adds each argument's word the set lacks, and writes the set (add). */
static int
token_list_add(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	struct vm_object *added;
	struct vm_cell *added_root;
	struct vm_string *token;
	uint32_t index;
	unsigned argument;
	int present;
	int status;

	/* The element, and every argument checked before anything changes. */
	*result = VM_VALUE_UNDEFINED;
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = bind_array_create(realm, &added);
	if (status != 0)
		return status;
	added_root = &added->cell;
	status = vm_heap_add_root(realm->heap, &added_root);
	if (status != 0)
		return status;
	for (argument = 0; argument < count; argument++) {
		status = token_list_token(realm, args[argument], &token);
		if (status != 0)
			goto cleanup_add;
		status = token_list_append(realm, added, token);
		if (status != 0)
			goto cleanup_add;
	}

	/* The set, with each new word at its end. */
	status = token_list_read(realm, element, &tokens);
	if (status != 0)
		goto cleanup_add;
	for (index = 0; index < added->length; index++) {
		token = (struct vm_string *)vm_value_as_cell(added->elements[index]);
		present = token_list_index(tokens, token, &argument);
		if (present)
			continue;

		/* A word the set lacks. */
		status = token_list_append(realm, tokens, token);
		if (status != 0)
			goto cleanup_add;
	}

	/* Succeeded: the attribute holds the set. */
	status = token_list_write(realm, element, tokens);

cleanup_add:
	/* Every argument remains reachable until validation and mutation finish. */
	vm_heap_remove_root(realm->heap, &added_root);
	if (status != 0)
		return status;
	return 0;
}

/* Removes each argument's word from the set, and writes the set when the attribute is there (remove). */
static int
token_list_remove(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_object *tokens;
	struct vm_object *removed;
	struct vm_cell *removed_root;
	struct vm_cell *tokens_root;
	struct vm_string *class_atom;
	struct vm_string *token;
	uint32_t index;
	uint32_t found;
	unsigned argument;
	int present;
	int status;
	int tokens_rooted;

	/* The element, and every argument checked before anything changes. */
	*result = VM_VALUE_UNDEFINED;
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = bind_array_create(realm, &removed);
	if (status != 0)
		return status;
	removed_root = &removed->cell;
	status = vm_heap_add_root(realm->heap, &removed_root);
	if (status != 0)
		return status;
	tokens_rooted = 0;
	for (argument = 0; argument < count; argument++) {
		status = token_list_token(realm, args[argument], &token);
		if (status != 0)
			goto cleanup_remove;
		status = token_list_append(realm, removed, token);
		if (status != 0)
			goto cleanup_remove;
	}

	/* The set, without each word. */
	status = token_list_read(realm, element, &tokens);
	if (status != 0)
		goto cleanup_remove;
	tokens_root = &tokens->cell;
	status = vm_heap_add_root(realm->heap, &tokens_root);
	if (status != 0)
		goto cleanup_remove;
	tokens_rooted = 1;
	for (index = 0; index < removed->length; index++) {
		token = (struct vm_string *)vm_value_as_cell(removed->elements[index]);
		present = token_list_index(tokens, token, &found);
		if (!present)
			continue;

		/* A word the set has. */
		status = token_list_remove_at(realm, tokens, found);
		if (status != 0)
			goto cleanup_remove;
	}

	/* An element without the attribute and with nothing left is not given one. */
	class_atom = vm_atom_from_ascii(realm->heap, "class");
	if (class_atom == NULL) {
		status = ENOMEM;
		goto cleanup_remove;
	}

	/* Empty absent classes stay absent after removal. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, class_atom);
	if (attribute == NULL && tokens->length == 0) {
		status = 0;
		goto cleanup_remove;
	}

	/* Succeeded: the attribute holds the set. */
	status = token_list_write(realm, element, tokens);

cleanup_remove:
	/* Every argument remains reachable until validation and mutation finish. */
	if (tokens_rooted)
		vm_heap_remove_root(realm->heap, &tokens_root);
	vm_heap_remove_root(realm->heap, &removed_root);
	if (status != 0)
		return status;
	return 0;
}

/*
 * Adds a word the set lacks or removes one it has, or with a second
 * argument adds it when that is true and removes it when false; reports
 * whether the word is in the set afterwards (toggle).
 */
static int
token_list_toggle(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	struct vm_string *token;
	struct vm_cell *token_root;
	uint32_t index;
	int present;
	int wanted;
	int status;

	/* The element, the word and the set. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = token_list_token(realm, js_argument(args, count, 0), &token);
	if (status != 0)
		return status;
	token_root = &token->cell;
	status = vm_heap_add_root(realm->heap, &token_root);
	if (status != 0)
		return status;
	status = token_list_read(realm, element, &tokens);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &token_root);
		return status;
	}

	/* Whether the word is wanted: the force when given, otherwise the opposite of now. */
	present = token_list_index(tokens, token, &index);
	wanted = 1;
	if (present)
		wanted = 0;
	if (count > 1U)
		wanted = vm_to_boolean(args[1]);

	/* Nothing to change. */
	if (wanted == present) {
		*result = vm_value_boolean(present);
		vm_heap_remove_root(realm->heap, &token_root);
		return 0;
	}

	/* Adds it, or removes it. */
	if (wanted) {
		status = token_list_append(realm, tokens, token);
	} else {
		status = token_list_remove_at(realm, tokens, index);
	}

	/* A change that failed. */
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &token_root);
		return status;
	}

	/* The attribute holds the set. */
	status = token_list_write(realm, element, tokens);
	vm_heap_remove_root(realm->heap, &token_root);
	if (status != 0)
		return status;

	/* Succeeded: whether the word is there now. */
	*result = vm_value_boolean(wanted);
	return 0;
}

/*
 * Replaces a word of the set with another where it stands (the other is
 * dropped from later places); reports whether the first was there
 * (replace).
 */
static int
token_list_replace(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *tokens;
	struct vm_string *old_token;
	struct vm_string *new_token;
	struct vm_cell *old_root;
	struct vm_cell *new_root;
	uint32_t old_index;
	uint32_t new_index;
	int old_present;
	int new_present;
	int status;

	/* The element, both words and the set. */
	status = token_list_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = token_list_token(realm, js_argument(args, count, 0), &old_token);
	if (status != 0)
		return status;
	old_root = &old_token->cell;
	status = vm_heap_add_root(realm->heap, &old_root);
	if (status != 0)
		return status;
	status = token_list_token(realm, js_argument(args, count, 1), &new_token);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &old_root);
		return status;
	}

	/* Retain the converted replacement while the class set is read. */
	new_root = &new_token->cell;
	status = vm_heap_add_root(realm->heap, &new_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &old_root);
		return status;
	}

	/* Read the current set after both conversions can run user code. */
	status = token_list_read(realm, element, &tokens);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &new_root);
		vm_heap_remove_root(realm->heap, &old_root);
		return status;
	}

	/* A word the set does not have is not replaced. */
	old_present = token_list_index(tokens, old_token, &old_index);
	if (!old_present) {
		*result = VM_VALUE_FALSE;
		vm_heap_remove_root(realm->heap, &new_root);
		vm_heap_remove_root(realm->heap, &old_root);
		return 0;
	}

	/* The new word takes the old one's place, and leaves any later place it had. */
	new_present = token_list_index(tokens, new_token, &new_index);
	if (new_present && new_index < old_index) {
		/* It was already earlier: the old one just goes. */
		status = token_list_remove_at(realm, tokens, old_index);
	} else {
		/* Otherwise it stands where the old one stood. */
		tokens->elements[old_index] = vm_value_cell(new_token);
		status = 0;
		if (new_present && new_index > old_index)
			status = token_list_remove_at(realm, tokens, new_index);
	}

	/* A change that failed. */
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &new_root);
		vm_heap_remove_root(realm->heap, &old_root);
		return status;
	}

	/* The attribute holds the set. */
	status = token_list_write(realm, element, tokens);
	vm_heap_remove_root(realm->heap, &new_root);
	vm_heap_remove_root(realm->heap, &old_root);
	if (status != 0)
		return status;

	/* Succeeded: the word was replaced. */
	*result = VM_VALUE_TRUE;
	return 0;
}

/*
 * Defines a dataset accessor for a data-* attribute: its name in camel
 * case, a getter and a setter that know the attribute's name.
 */
static int
string_map_accessor(
	struct vm_realm *realm,
	struct vm_object *map,
	const struct dom_attribute *attribute)
{
	struct vm_function *getter;
	struct vm_function *setter;
	struct vm_accessor *accessor;
	struct vm_string *name;
	struct vm_cell *roots[4];
	vm_value key;
	unsigned registered;
	unsigned index;
	int status;

	/* The property's name. */
	status = string_map_name(realm, attribute->name, &name);
	if (status != 0)
		return status;
	roots[0] = &name->cell;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	status = vm_key_from_string(realm->heap, name, &key);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &roots[0]);
		return status;
	}

	/* The temporary key name remains rooted during method construction. */
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 1;
	for (index = 1; index < 4U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The getter and the setter, which keep the attribute's name. */
	status = js_builtin_function(realm, "get", 0, string_map_get, NULL, &getter);
	if (status != 0)
		goto cleanup;
	roots[1] = &getter->object.cell;
	getter->data = vm_value_cell(attribute->name);
	status = js_builtin_function(realm, "set", 1, string_map_set, NULL, &setter);
	if (status != 0)
		goto cleanup;
	roots[2] = &setter->object.cell;
	setter->data = vm_value_cell(attribute->name);

	/* The accessor, enumerable as other browsers' names are. */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(getter), vm_value_cell(setter));
	if (accessor == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The descriptor remains live until it is attached to the rooted map. */
	roots[3] = &accessor->cell;
	status = vm_object_define(realm->heap, map, key, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);

cleanup:
	/* The map owns the completed accessor after publication. */
	while (registered > 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* The caller sees only a fully published accessor or the original error. */
	if (status != 0)
		return status;

	/* Succeeded: the accessor is defined. */
	return 0;
}

/* Turns a data-* attribute's name into its dataset name: the prefix gone, and each "-" before a lower-case letter gone with the letter in upper case. */
static int
string_map_name(
	struct vm_realm *realm,
	const struct vm_string *attribute,
	struct vm_string **name)
{
	struct wb_units units;
	size_t index;
	uint16_t unit;
	uint16_t next;
	int status;

	/* The characters after the prefix, turned as they are copied. */
	wb_units_init(&units);
	status = 0;
	index = QUERY_DATA_PREFIX_LENGTH;
	while (index < attribute->length && status == 0) {
		unit = vm_string_at(attribute, index);
		next = 0;
		if (index + 1U < attribute->length)
			next = vm_string_at(attribute, index + 1U);

		/* A hyphen before a lower-case letter makes the letter upper case. */
		if (unit == '-' && next >= 'a' && next <= 'z') {
			unit = (uint16_t)(next - 0x20U);
			index++;
		}

		/* The character. */
		status = wb_units_append(&units, &unit, 1);
		index++;
	}

	/* A name that could not grow. */
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* The name as a string. */
	*name = vm_string_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (*name == NULL)
		return ENOMEM;

	/* Succeeded: the name. */
	return 0;
}

/* Finds the element of the DOMStringMap an accessor's this value stands for. */
static int
string_map_element(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	int status;

	/* The map keeps its element's object as a token list does. */
	status = token_list_element(realm, this_value, element);
	if (status != 0)
		return status;

	/* Succeeded: the element. */
	return 0;
}

/* Reports a data-* attribute's value, or undefined when the element no longer has it. */
static int
string_map_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_function *callee;
	struct vm_string *name;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element and the attribute's name. */
	status = string_map_element(realm, this_value, &element);
	if (status != 0)
		return status;
	callee = js_builtin_callee(realm);
	name = (struct vm_string *)vm_value_as_cell(callee->data);

	/* An attribute removed since the map was made. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL) {
		*result = VM_VALUE_UNDEFINED;
		return 0;
	}

	/* Succeeded: its value. */
	*result = vm_value_cell(attribute->value);
	return 0;
}

/* Sets a data-* attribute to a value's string. */
static int
string_map_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_function *callee;
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *value_root;
	int status;

	/* The element, the attribute's name and the value. */
	*result = VM_VALUE_UNDEFINED;
	status = string_map_element(realm, this_value, &element);
	if (status != 0)
		return status;
	callee = js_builtin_callee(realm);
	name = (struct vm_string *)vm_value_as_cell(callee->data);
	status = bind_to_string(realm, js_argument(args, count, 0), &value);
	if (status != 0)
		return status;
	value_root = &value->cell;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0)
		return status;

	/* The attribute takes it. */
	status = dom_element_set_attribute(element, name, value);
	vm_heap_remove_root(realm->heap, &value_root);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Tells whether a character is ASCII whitespace (space, tab, line feed, form feed, carriage return). */
static int
query_is_space(
	uint16_t unit)
{
	/* The five characters. */
	switch (unit) {
	case 0x20U:
	case 0x09U:
	case 0x0aU:
	case 0x0cU:
	case 0x0dU:
		return 1;
	default:
		break;
	}

	/* Anything else. */
	return 0;
}

/* Tells whether a string starts with ASCII characters. */
static int
query_has_prefix(
	const struct vm_string *string,
	const char *ascii)
{
	size_t index;
	size_t length;
	uint16_t unit;

	/* A string shorter than the prefix cannot start with it. */
	length = strlen(ascii);
	if (string->length < length)
		return 0;

	/* Each character of the prefix. */
	for (index = 0; ascii[index] != '\0'; index++) {
		unit = vm_string_at(string, index);
		if (unit != (uint16_t)(unsigned char)ascii[index])
			return 0;
	}

	/* Succeeded: the string starts with it. */
	return 1;
}
