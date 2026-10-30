/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Elements for scripts: the Element interface (names, attributes and the
 * mixins' members) and HTMLElement (the reflected attributes the first
 * pass needs, and click).
 */

#include "bind/internal.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static int element_this(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int element_tag_name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_namespace_uri(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_prefix(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_local_name(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_id_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_id_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_class_name_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_class_name_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_attributes_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_attributes_get_own(struct vm_object *object, vm_value key, struct vm_property *property);
static int element_title_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_title_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_hidden_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_hidden_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_hidden_until_found(const struct vm_string *value);
static int details_open_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int details_open_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int details_toggle_task(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_get_attribute(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_set_attribute(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_remove_attribute(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_has_attribute(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_click(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_attribute_name(struct vm_realm *realm, const struct dom_element *element, vm_value value, struct vm_string **name);
static int image_src_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_src_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_alt_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_alt_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_width_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_width_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_height_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_height_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_complete(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_state(struct vm_realm *realm, struct dom_element *element, struct bind_image *state);
static int image_natural_width(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_natural_height(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int image_rendered_dimension(struct vm_realm *realm, struct dom_element *element, const char *name, int *available, vm_value *result);
static int image_dimension(struct vm_realm *realm, vm_value this_value, const char *name, vm_value *result);
static int script_src_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int script_src_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int script_type_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int script_type_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int script_async_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int script_async_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int element_reflect_get(struct vm_realm *realm, vm_value this_value, const char *name, vm_value *result);
static int element_reflect_set(struct vm_realm *realm, vm_value this_value, const char *name, const vm_value *args, unsigned count);

/* Current attribute indices are virtual; the owning Element retains the map. */
static const struct vm_native_operations element_attributes_native = {
	element_attributes_get_own, NULL, NULL, NULL, NULL
};

/*
 * The attributes of Element, with the members of its mixins.  The table
 * is constant for the life of the program.
 */
static const struct bind_attribute element_attributes[] = {
	{ "tagName", element_tag_name, NULL },
	{ "localName", element_local_name, NULL },
	{ "namespaceURI", element_namespace_uri, NULL },
	{ "prefix", element_prefix, NULL },
	{ "id", element_id_get, element_id_set },
	{ "className", element_class_name_get, element_class_name_set },
	{ "attributes", element_attributes_get, NULL },
	{ "children", bind_children, NULL },
	{ "firstElementChild", bind_first_element_child_get, NULL },
	{ "lastElementChild", bind_last_element_child_get, NULL },
	{ "childElementCount", bind_child_element_count, NULL },
	{ "previousElementSibling", bind_previous_element_sibling, NULL },
	{ "nextElementSibling", bind_next_element_sibling, NULL },
	{ "classList", bind_class_list, NULL },
	{ "clientWidth", bind_client_width, NULL },
	{ "clientHeight", bind_client_height, NULL },
	{ "clientTop", bind_client_top, NULL },
	{ "clientLeft", bind_client_left, NULL },
	{ "scrollWidth", bind_scroll_width, NULL },
	{ "scrollHeight", bind_scroll_height, NULL },
	{ "scrollTop", bind_scroll_top, bind_scroll_top_set },
	{ "scrollLeft", bind_scroll_left, bind_scroll_position_set },
	{ "innerHTML", bind_inner_html_get, bind_inner_html_set },
	{ "outerHTML", bind_outer_html_get, bind_outer_html_set },
	{ NULL, NULL, NULL }
};

/*
 * The operations of Element, with the members of its mixins.  The table
 * is constant for the life of the program.
 */
static const struct bind_operation element_operations[] = {
	{ "getAttribute", 1, element_get_attribute },
	{ "setAttribute", 2, element_set_attribute },
	{ "removeAttribute", 1, element_remove_attribute },
	{ "hasAttribute", 1, element_has_attribute },
	{ "getElementsByTagName", 1, bind_get_elements_by_tag_name },
	{ "getElementsByClassName", 1, bind_get_elements_by_class_name },
	{ "append", 0, bind_append },
	{ "prepend", 0, bind_prepend },
	{ "replaceChildren", 0, bind_replace_children },
	{ "remove", 0, bind_remove },
	{ "querySelector", 1, bind_query_selector },
	{ "querySelectorAll", 1, bind_query_selector_all },
	{ "matches", 1, bind_matches },
	{ "webkitMatchesSelector", 1, bind_matches },
	{ "closest", 1, bind_closest },
	{ "getBoundingClientRect", 0, bind_get_bounding_client_rect },
	{ "getClientRects", 0, bind_get_client_rects },
	{ "insertAdjacentHTML", 2, bind_insert_adjacent_html },
	{ "insertAdjacentElement", 2, bind_insert_adjacent_element },
	{ "insertAdjacentText", 2, bind_insert_adjacent_text },
	{ "scrollIntoView", 0, bind_scroll_into_view },
	{ "scrollTo", 0, bind_element_scroll_to },
	{ "scroll", 0, bind_element_scroll_to },
	{ "scrollBy", 0, bind_element_scroll_by },
	{ NULL, 0, NULL }
};

/*
 * The Element interface.
 */
const struct bind_interface bind_element_interface = {
	"Element", BIND_NODE, 0, NULL, element_attributes, element_operations, NULL
};

/*
 * The attributes of HTMLElement.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute html_element_attributes[] = {
	{ "title", element_title_get, element_title_set },
	{ "hidden", element_hidden_get, element_hidden_set },
	{ "dataset", bind_dataset, NULL },
	{ "style", bind_style, bind_style_set },
	{ "offsetWidth", bind_offset_width, NULL },
	{ "offsetHeight", bind_offset_height, NULL },
	{ "offsetParent", bind_offset_parent, NULL },
	{ "offsetTop", bind_offset_top, NULL },
	{ "offsetLeft", bind_offset_left, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of HTMLElement.  The table is constant for the life of
 * the program.
 */
static const struct bind_operation html_element_operations[] = {
	{ "click", 0, element_click },
	{ NULL, 0, NULL }
};

/*
 * The HTMLElement interface (every HTML element's, in this pass).
 */
const struct bind_interface bind_html_element_interface = {
	"HTMLElement", BIND_ELEMENT, 0, NULL, html_element_attributes, html_element_operations, NULL
};

/* The boolean open attribute belongs only to HTMLDetailsElement. */
static const struct bind_attribute details_attributes[] = {
	{ "open", details_open_get, details_open_set },
	{ NULL, NULL, NULL }
};

/* A details element has its own interface and inherits all HTMLElement members. */
const struct bind_interface bind_html_details_element_interface = {
	"HTMLDetailsElement", BIND_HTML_ELEMENT, 0, NULL, details_attributes, NULL, NULL
};

/* A dialog element has its own interface (its members are made by the prelude) and inherits all HTMLElement members. */
const struct bind_interface bind_html_dialog_element_interface = {
	"HTMLDialogElement", BIND_HTML_ELEMENT, 0, NULL, NULL, NULL, NULL
};

/*
 * The attributes of HTMLImageElement.  The table is constant for the life
 * of the program.
 */
static const struct bind_attribute image_attributes[] = {
	{ "src", image_src_get, image_src_set },
	{ "alt", image_alt_get, image_alt_set },
	{ "width", image_width_get, image_width_set },
	{ "height", image_height_get, image_height_set },
	{ "complete", image_complete, NULL },
	{ "naturalWidth", image_natural_width, NULL },
	{ "naturalHeight", image_natural_height, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The HTMLImageElement interface (img elements', and new Image()'s).  An
 * image a script makes is not loaded in this pass: it is complete and
 * has no natural size.
 */
const struct bind_interface bind_html_image_element_interface = {
	"HTMLImageElement", BIND_HTML_ELEMENT, 0, NULL, image_attributes, NULL, NULL
};

/* The reflected attributes of HTMLScriptElement needed by dynamic loaders. */
static const struct bind_attribute script_attributes[] = {
	{ "src", script_src_get, script_src_set },
	{ "type", script_type_get, script_type_set },
	{ "async", script_async_get, script_async_set },
	{ NULL, NULL, NULL }
};

/* The HTMLScriptElement interface (script elements'). */
const struct bind_interface bind_html_script_element_interface = {
	"HTMLScriptElement", BIND_HTML_ELEMENT, 0, NULL, script_attributes, NULL, NULL
};

/*
 * Tells whether an element's class attribute lists a class name (an
 * atom), as a whole word between ASCII whitespace.
 */
int
bind_element_has_class(
	const struct dom_element *element,
	const struct vm_string *name)
{
	const struct dom_attribute *attribute;
	const struct vm_string *classes;
	size_t index;
	size_t start;
	size_t length;
	size_t offset;
	uint16_t unit;
	uint16_t word_unit;
	uint16_t name_unit;
	int space;
	int same;

	/* The class attribute (its name's atom is found by comparing names). */
	classes = NULL;
	for (index = 0; index < element->attribute_count; index++) {
		attribute = &element->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;
		same = vm_string_equal_ascii(attribute->name, "class");
		if (same) {
			classes = attribute->value;
			break;
		}
	}

	/* No class attribute has no classes. */
	if (classes == NULL)
		return 0;

	/* Compares each word of the attribute with the name. */
	start = 0;
	for (index = 0; index <= classes->length; index++) {
		/* The end of the value ends the last word. */
		space = 1;
		if (index < classes->length) {
			unit = vm_string_at(classes, index);
			space = 0;
			if (unit == 0x20U || unit == 0x09U || unit == 0x0aU || unit == 0x0cU || unit == 0x0dU)
				space = 1;
		}

		/* A character of a word goes on to the next. */
		if (!space)
			continue;

		/* A word of the name's length with the same units is the class. */
		length = index - start;
		same = 0;
		if (length == name->length && length != 0)
			same = 1;
		for (offset = 0; same && offset < length; offset++) {
			word_unit = vm_string_at(classes, start + offset);
			name_unit = vm_string_at(name, offset);
			if (word_unit != name_unit)
				same = 0;
		}

		/* The word is the class, or the next word starts after the space. */
		if (same)
			return 1;
		start = index + 1U;
	}

	/* No word is the name. */
	return 0;
}

/* Finds the element a method's this value stands for, throwing a TypeError otherwise. */
static int
element_this(
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

	/* Succeeded: the element is found. */
	*element = (struct dom_element *)node;
	return 0;
}

/* Returns one saved wrapper whose indexed content follows current attributes. */
static int
element_attributes_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_object *wrapper;
	struct vm_object *prototype;
	struct vm_cell *root;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	if (element->attributes_collection == NULL) {
		root = &element->node.cell;
		status = vm_heap_add_root(element->node.document->heap, &root);
		if (status != 0)
			return status;
		prototype = realm->object_prototype;
		if (element->node.document->view != NULL)
			prototype = ((struct bind_window *)element->node.document->view)->realm->object_prototype;
		wrapper = vm_object_create(element->node.document->heap, prototype);
		if (wrapper != NULL) {
			wrapper->kind = VM_KIND_PLATFORM;
			wrapper->internal = vm_value_cell(element);
			wrapper->native_operations = &element_attributes_native;
			element->attributes_collection = wrapper;
		}
		vm_heap_remove_root(element->node.document->heap, &root);
		if (wrapper == NULL)
			return ENOMEM;
	}
	*result = vm_value_cell(element->attributes_collection);
	return 0;
}

/* Materializes one read-only indexed attribute view for the WPT read surface. */
static int
element_attributes_get_own(
	struct vm_object *object,
	vm_value key,
	struct vm_property *property)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_realm *realm;
	struct vm_object *item;
	struct vm_cell *root;
	struct vm_string *name;
	uint32_t index;
	int status;

	element = (struct dom_element *)vm_value_as_cell(object->internal);
	if (!vm_value_is_array_index(key, &index)) {
		if (!vm_value_is_string(key))
			return 0;
		name = (struct vm_string *)vm_value_as_cell(key);
		if (!vm_string_equal_ascii(name, "length"))
			return 0;
		property->temporary = vm_value_number((double)element->attribute_count);
		property->holder = object;
		property->value = &property->temporary;
		property->attributes = VM_PROPERTY_CONFIGURABLE;
		return 1;
	}
	if ((size_t)index >= element->attribute_count)
		return 0;
	if (element->node.document->view == NULL)
		return -EINVAL;
	realm = ((struct bind_window *)element->node.document->view)->realm;
	attribute = &element->attributes[index];
	item = vm_object_create(element->node.document->heap, realm->object_prototype);
	if (item == NULL)
		return -ENOMEM;
	root = &item->cell;
	status = vm_heap_add_root(element->node.document->heap, &root);
	if (status != 0)
		return -status;
	status = js_builtin_value(realm, item, "name", vm_value_cell(attribute->name), VM_PROPERTY_DEFAULT);
	if (status == 0)
		status = js_builtin_value(realm, item, "value", vm_value_cell(attribute->value), VM_PROPERTY_DEFAULT);
	vm_heap_remove_root(element->node.document->heap, &root);
	if (status != 0)
		return -status;
	property->temporary = vm_value_cell(item);
	property->holder = object;
	property->value = &property->temporary;
	property->attributes = VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE;
	return 1;
}

/* Reports a qualified name, uppercasing HTML elements only within HTML Documents. */
static int
element_tag_name(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct wb_units units;
	uint16_t unit;
	size_t index;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* The qualified name: the prefix, a colon and the local name. */
	wb_units_init(&units);
	status = 0;
	if (element->prefix != NULL) {
		status = vm_string_append_units(element->prefix, &units);
		if (status == 0)
			status = wb_units_append_code_point(&units, ':');
	}

	/* Then the local name. */
	if (status == 0)
		status = vm_string_append_units(element->local_name, &units);

	/* Only an HTML Document uppercases the qualified name of an HTML element. */
	for (index = 0;
	     status == 0 &&
	     element->ns == DOM_NS_HTML &&
	     element->node.document->content == DOM_CONTENT_HTML &&
	     index < units.length;
	     index++) {
		unit = units.data[index];
		if (unit >= 'a' && unit <= 'z')
			units.data[index] = (uint16_t)(unit - 0x20U);
	}

	/* The string. */
	if (status == 0)
		status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the tag name is reported. */
	return 0;
}

/* Reports the element's local name (localName). */
static int
element_local_name(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* Succeeded: the name's atom is the string. */
	*result = vm_value_cell(element->local_name);
	return 0;
}

/* Reports the URL of the element's namespace (namespaceURI). */
static int
element_namespace_uri(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	static const char *const urls[] = {
		[DOM_NS_HTML] = "http://www.w3.org/1999/xhtml",
		[DOM_NS_SVG] = "http://www.w3.org/2000/svg",
		[DOM_NS_MATHML] = "http://www.w3.org/1998/Math/MathML"
	};
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* Script-created elements retain their exact namespace URI, including custom ones. */
	if (element->namespace_uri != NULL) {
		*result = vm_value_cell(element->namespace_uri);
		return 0;
	}

	/* Parser-created elements retain their existing built-in namespace mapping. */
	if (element->ns >= sizeof(urls) / sizeof(urls[0]) || urls[element->ns] == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the namespace's URL. */
	status = bind_string(realm, urls[element->ns], result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports an element's namespace prefix, or null when its name has no prefix. */
static int
element_prefix(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A borrowed getter still requires an actual Element receiver. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* The stored prefix is part of identity rather than a parsed attribute. */
	*result = VM_VALUE_NULL;
	if (element->prefix != NULL)
		*result = vm_value_cell(element->prefix);

	/* Succeeded: the receiver's exact prefix or its absence is reported. */
	return 0;
}

/* Reports the id attribute, or the empty string (id). */
static int
element_id_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The reflected attribute. */
	status = element_reflect_get(realm, this_value, "id", result);
	if (status != 0)
		return status;

	/* Succeeded: the id is reported. */
	return 0;
}

/* Sets the id attribute (id). */
static int
element_id_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "id", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the id is set. */
	return 0;
}

/* Reports the class attribute, or the empty string (className). */
static int
element_class_name_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The reflected attribute. */
	status = element_reflect_get(realm, this_value, "class", result);
	if (status != 0)
		return status;

	/* Succeeded: the classes are reported. */
	return 0;
}

/* Sets the class attribute (className). */
static int
element_class_name_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "class", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the classes are set. */
	return 0;
}

/* Reports the title attribute, or the empty string (title). */
static int
element_title_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The reflected attribute. */
	status = element_reflect_get(realm, this_value, "title", result);
	if (status != 0)
		return status;

	/* Succeeded: the title is reported. */
	return 0;
}

/* Sets the title attribute (title). */
static int
element_title_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "title", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the title is set. */
	return 0;
}

/* An ASCII-insensitive match for the hidden attribute's special keyword. */
static int
element_hidden_until_found(const struct vm_string *value)
{
	static const char keyword[] = "until-found";
	size_t index;
	uint16_t unit;

	if (value == NULL || value->length != sizeof(keyword) - 1U)
		return 0;
	for (index = 0U; index < sizeof(keyword) - 1U; index++) {
		unit = vm_string_at(value, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (uint16_t)keyword[index])
			return 0;
	}
	return 1;
}

/* Reports the hidden attribute's missing, hidden or until-found state. */
static int
element_hidden_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	attribute = dom_attribute_ascii(element, "hidden");
	if (element_hidden_until_found(attribute))
		return bind_string(realm, "until-found", result);
	*result = vm_value_boolean(attribute != NULL);
	return 0;
}

/* Adds or removes the hidden attribute (hidden). */
static int
element_hidden_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_cell *roots[2];
	vm_value pair[2];
	vm_value given;
	struct vm_string *text;
	const char *new_value;
	double number;
	unsigned registered;
	int remove;
	int status;

	/* The special keyword keeps its spelling; the other false-like values remove. */
	*result = VM_VALUE_UNDEFINED;
	given = js_argument(args, count, 0);
	remove = 0;
	new_value = "";
	if (vm_value_is_string(given)) {
		text = (struct vm_string *)vm_value_as_cell(given);
		if (element_hidden_until_found(text))
			new_value = "until-found";
		else if (text->length == 0U)
			remove = 1;
	} else if (given == VM_VALUE_FALSE || given == VM_VALUE_NULL ||
	    given == VM_VALUE_UNDEFINED) {
		remove = 1;
	} else if (vm_value_is_number(given)) {
		number = vm_value_as_number(given);
		if (number == 0.0 || number != number)
			remove = 1;
	}

	/* The attribute's name and its resulting value stay live through mutation. */
	status = bind_string(realm, "hidden", &pair[0]);
	if (status != 0)
		return status;
	roots[0] = vm_value_as_cell(pair[0]);
	registered = 0;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	registered++;

	/* The name stays live while the empty value is allocated. */
	status = bind_string(realm, new_value, &pair[1]);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(pair[1]);
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0)
		goto cleanup;
	registered++;

	/* The omitted state removes; all other values set the content attribute. */
	if (remove)
		status = element_remove_attribute(realm, this_value, pair, 1, result);
	else
		status = element_set_attribute(realm, this_value, pair, 2, result);

cleanup:
	/* Both temporary arguments leave the root set on every outcome. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Either may have thrown. */
	if (status != 0)
		return status;

	/* Succeeded: the attribute follows the value. */
	return 0;
}

/* Queues one details toggle through the page's existing task timer. */
int
bind_details_queue_toggle(
	struct bind_window *window,
	struct dom_element *element)
{
	struct vm_function *function;
	struct vm_cell *roots[2];
	vm_value arguments[2];
	vm_value timer_id;
	unsigned registered;
	int status;

	/* A pending task delivers the final state once for this element. */
	if (element->toggle_pending)
		return 0;
	roots[0] = &element->node.cell;
	roots[1] = NULL;
	registered = 0U;
	status = vm_heap_add_root(window->realm->heap, &roots[0]);
	if (status != 0)
		return status;
	registered++;
	status = js_builtin_function(window->realm, "details toggle task", 0, details_toggle_task, NULL, &function);
	if (status != 0)
		goto cleanup;
	roots[1] = &function->object.cell;
	status = vm_heap_add_root(window->realm->heap, &roots[1]);
	if (status != 0)
		goto cleanup;
	registered++;
	function->data = vm_value_cell(element);
	arguments[0] = vm_value_cell(function);
	arguments[1] = vm_value_int32(0);
	status = bind_set_timeout(window->realm, VM_VALUE_UNDEFINED, arguments, 2U, &timer_id);
	if (status == 0)
		element->toggle_pending = 1;

cleanup:
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(window->realm->heap, &roots[registered]);
	}
	return status;
}

/* Runs after the current script task and sends one non-bubbling toggle. */
static int
details_toggle_task(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_function *callee;
	struct dom_element *element;
	int canceled;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	*result = VM_VALUE_UNDEFINED;
	callee = js_builtin_callee(realm);
	element = (struct dom_element *)vm_value_as_cell(callee->data);
	element->toggle_pending = 0;
	canceled = 0;
	return bind_fire_event(bind_window_of(realm), &element->node, "toggle", 0U, &canceled);
}

/* Reports the details element's reflected boolean open attribute. */
static int
details_open_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_DETAILS)
		return bind_throw_illegal(realm);
	*result = vm_value_boolean(dom_attribute_ascii(element, "open") != NULL);
	return 0;
}

/* Sets or removes the details element's open content attribute. */
static int
details_open_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_cell *roots[2];
	vm_value pair[2];
	unsigned registered;
	int was_open;
	int open;
	int status;

	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	if (element->ns != DOM_NS_HTML || element->tag != DOM_TAG_DETAILS)
		return bind_throw_illegal(realm);
	*result = VM_VALUE_UNDEFINED;
	was_open = dom_attribute_ascii(element, "open") != NULL;
	open = vm_to_boolean(js_argument(args, count, 0));
	status = bind_string(realm, "open", &pair[0]);
	if (status != 0)
		return status;
	roots[0] = vm_value_as_cell(pair[0]);
	registered = 0U;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	registered++;
	status = bind_string(realm, "", &pair[1]);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(pair[1]);
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0)
		goto cleanup;
	registered++;
	if (open)
		status = element_set_attribute(realm, this_value, pair, 2U, result);
	else
		status = element_remove_attribute(realm, this_value, pair, 1U, result);
	if (status == 0 && was_open != open)
		status = bind_details_queue_toggle(bind_window_of(realm), element);

cleanup:
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}
	return status;
}

/* Reports an attribute's value, or null when the element does not have it (getAttribute). */
static int
element_get_attribute(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_string *name;
	int status;

	/* The element and the attribute's name. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = element_attribute_name(realm, element, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;

	/* The attribute, if the element has it. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	if (attribute == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Succeeded: the value is reported. */
	*result = vm_value_cell(attribute->value);
	return 0;
}

/* Sets an attribute's value, adding the attribute when needed (setAttribute). */
static int
element_set_attribute(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	struct vm_string *value;
	int status;

	/* The element, the attribute's name and the value. */
	*result = VM_VALUE_UNDEFINED;
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = element_attribute_name(realm, element, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 1), &value);
	if (status != 0)
		return status;

	/* An empty name is not a name. */
	if (name->length == 0) {
		status = bind_throw_dom(realm, "InvalidCharacterError", "The attribute name is not a valid name.");
		return status;
	}

	/* The element's attribute takes the value. */
	status = dom_element_set_attribute(element, name, value);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Removes an attribute (removeAttribute). */
static int
element_remove_attribute(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	int status;

	/* The element and the attribute's name. */
	*result = VM_VALUE_UNDEFINED;
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = element_attribute_name(realm, element, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;

	/* The attribute goes, if the element has it. */
	status = dom_element_remove_attribute(element, name);
	if (status != 0)
		return status;

	/* Succeeded: the element does not have the attribute. */
	return 0;
}

/* Reports whether the element has an attribute (hasAttribute). */
static int
element_has_attribute(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_string *name;
	int status;

	/* The element and the attribute's name. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = element_attribute_name(realm, element, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;

	/* Whether the element has it. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, name);
	*result = VM_VALUE_FALSE;
	if (attribute != NULL)
		*result = VM_VALUE_TRUE;

	/* Succeeded: the answer is reported. */
	return 0;
}

/* Performs a synthetic script click through native activation and event dispatch. */
static int
element_click(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct dom_element *element;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	*result = VM_VALUE_UNDEFINED;
	window = bind_window_of(realm);
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;

	/* Use the target document realm when a method is borrowed across windows. */
	if (element->node.document->view != NULL)
		window = element->node.document->view;
	status = bind_click(window, element);
	if (status != 0)
		return status;

	/* Succeeded: the click is dispatched. */
	return 0;
}

/* Converts an attribute name argument to its atom, in lower case for an HTML element. */
static int
element_attribute_name(
	struct vm_realm *realm,
	const struct dom_element *element,
	vm_value value,
	struct vm_string **name)
{
	int lower;
	int status;

	/* HTML elements' attribute names are in lower case. */
	lower = 0;
	if (element->ns == DOM_NS_HTML)
		lower = 1;

	/* The atom. */
	status = bind_to_atom(realm, value, lower, name);
	if (status != 0)
		return status;

	/* Succeeded: the name is found. */
	return 0;
}

/* Reports the src attribute resolved against the document's URL, or the empty string (src). */
static int
image_src_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The reflected attribute, resolved against the document's URL. */
	status = bind_reflect_url_get(realm, this_value, DOM_TAG_IMG, "src", result);
	if (status != 0)
		return status;

	/* Succeeded: the URL is reported. */
	return 0;
}

/* Sets the src attribute (src). */
static int
image_src_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "src", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Reports the script's src attribute. */
static int
script_src_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	return image_src_get(realm, this_value, args, count, result);
}

/* Sets the script's src attribute. */
static int
script_src_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	return image_src_set(realm, this_value, args, count, result);
}

/* Reports the script's type attribute. */
static int
script_type_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	return element_reflect_get(realm, this_value, "type", result);
}

/* Sets the script's type attribute. */
static int
script_type_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	*result = VM_VALUE_UNDEFINED;
	return element_reflect_set(realm, this_value, "type", args, count);
}

/* Reports whether the script has the async attribute. */
static int
script_async_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value name;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);
	status = bind_string(realm, "async", &name);
	if (status != 0)
		return status;
	return element_has_attribute(realm, this_value, &name, 1, result);
}

/* Adds or removes the script's async attribute. */
static int
script_async_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_cell *roots[2];
	vm_value pair[2];
	unsigned registered;
	int asynchronous;
	int status;

	/* The attribute's name and the Boolean value. */
	*result = VM_VALUE_UNDEFINED;
	asynchronous = vm_to_boolean(js_argument(args, count, 0));
	status = bind_string(realm, "async", &pair[0]);
	if (status != 0)
		return status;
	roots[0] = vm_value_as_cell(pair[0]);
	registered = 0;
	status = vm_heap_add_root(realm->heap, &roots[0]);
	if (status != 0)
		return status;
	registered++;

	/* The name stays live while the empty value is allocated. */
	status = bind_string(realm, "", &pair[1]);
	if (status != 0)
		goto cleanup;
	roots[1] = vm_value_as_cell(pair[1]);
	status = vm_heap_add_root(realm->heap, &roots[1]);
	if (status != 0)
		goto cleanup;
	registered++;

	/* The requested Boolean chooses addition or removal. */
	if (asynchronous) {
		status = element_set_attribute(realm, this_value, pair, 2, result);
	} else {
		status = element_remove_attribute(realm, this_value, pair, 1, result);
	}

cleanup:
	/* Both temporary arguments leave the root set on every outcome. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* A failed attribute change leaves the ordering flag alone. */
	if (status != 0)
		return status;

	/* The property set to false opts a dynamic script into insertion order. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	if (asynchronous) {
		element->node.flags &= (uint16_t)~DOM_NODE_SCRIPT_ORDERED;
	} else {
		element->node.flags |= DOM_NODE_SCRIPT_ORDERED;
	}

	/* Succeeded: the attribute and ordering state follow the property. */
	return 0;
}

/* Reports the alt attribute, or the empty string (alt). */
static int
image_alt_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The reflected attribute. */
	status = element_reflect_get(realm, this_value, "alt", result);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is reported. */
	return 0;
}

/* Sets the alt attribute (alt). */
static int
image_alt_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "alt", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Reports the width attribute as a whole number of pixels, 0 when it has none (width). */
static int
image_width_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The attribute's number. */
	status = image_dimension(realm, this_value, "width", result);
	if (status != 0)
		return status;

	/* Succeeded: the size is reported. */
	return 0;
}

/* Sets the width attribute to a number's text (width). */
static int
image_width_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "width", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Reports the height attribute as a whole number of pixels, 0 when it has none (height). */
static int
image_height_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The attribute's number. */
	status = image_dimension(realm, this_value, "height", result);
	if (status != 0)
		return status;

	/* Succeeded: the size is reported. */
	return 0;
}

/* Sets the height attribute to a number's text (height). */
static int
image_height_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The reflected attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = element_reflect_set(realm, this_value, "height", args, count);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}

/* Asks the host for an image's state; without a host an image has no source loaded. */
static int
image_state(
	struct vm_realm *realm,
	struct dom_element *element,
	struct bind_image *state)
{
	struct bind_window *window;

	/* Nothing until the host says. */
	memset(state, 0, sizeof(*state));
	window = bind_window_of(realm);
	if (window->host.image == NULL)
		return 0;
	return window->host.image(window->host.context, element, 0, state);
}

/* Reports whether the image is complete: without a source, decoded or broken, but not while it loads (complete). */
static int
image_complete(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct bind_image state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* this must be an element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = image_state(realm, element, &state);
	if (status != 0)
		return status;

	/* Succeeded: whether it is done. */
	*result = vm_value_boolean(state.state != BIND_IMAGE_LOADING);
	return 0;
}

/* Reports the image's natural width: its decoded width, or 0 (naturalWidth). */
static int
image_natural_width(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct bind_image state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* this must be an element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = image_state(realm, element, &state);
	if (status != 0)
		return status;

	/* Succeeded: the width. */
	*result = vm_value_int32(state.width);
	return 0;
}

/* Reports the image's natural height: its decoded height, or 0 (naturalHeight). */
static int
image_natural_height(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct bind_image state;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* this must be an element. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	status = image_state(realm, element, &state);
	if (status != 0)
		return status;

	/* Succeeded: the height. */
	*result = vm_value_int32(state.height);
	return 0;
}

/* Observes actual native content dimensions while retaining every callback participant. */
static int
image_rendered_dimension(
	struct vm_realm *realm,
	struct dom_element *element,
	const char *name,
	int *available,
	vm_value *result)
{
	struct dom_document *document;
	struct bind_window *window;
	struct bind_box box;
	struct vm_cell *roots[2];
	double pixels;
	unsigned index;
	int connected;
	int found;
	int same;
	int finite;
	int status;

	/* A detached image or inactive owner has only its existing attribute fallback. */
	*available = 0;
	document = element->node.document;
	window = document->view;
	if (window == NULL ||
	    window->detached ||
	    window->host.node_box == NULL)
		return 0;
	connected = dom_is_inclusive_ancestor(&document->node, &element->node);
	if (!connected)
		return 0;

	/* Both native target and actual owner survive GC even if the host retires the child. */
	roots[0] = &element->node.cell;
	roots[1] = &window->realm->cell;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0) {
			/* Release only earlier registrations if a root slot cannot be installed. */
			while (index != 0) {
				index--;
				vm_heap_remove_root(realm->heap, &roots[index]);
			}

			/* Allocation failure never publishes an unprotected host observation. */
			return status;
		}
	}

	/* The native host provides actual layout, never a CSS string or author width getter. */
	memset(&box, 0, sizeof(box));
	found = window->host.node_box(window->host.context, &element->node, &box);
	status = 0;
	if (found &&
	    !window->detached &&
	    element->node.document == document &&
	    document->view == window) {
		/* Revalidate connection after callbacks that may remove the queried image. */
		connected = dom_is_inclusive_ancestor(&document->node, &element->node);
		if (connected) {
			/* Image IDL dimensions exclude actual used borders and padding on each axis. */
			same = strcmp(name, "width");
			if (same == 0) {
				pixels = box.width - box.border_left - box.border_right - box.padding_left - box.padding_right;
			} else {
				pixels = box.height - box.border_top - box.border_bottom - box.padding_top - box.padding_bottom;
			}

			/* Checked nonnegative fixed-unit sizes fit the existing integer image IDL result. */
			finite = isfinite(pixels);
			if (!finite || pixels > INT_MAX) {
				status = EOVERFLOW;
			} else {
				/* Negative content extents have zero unsigned image size. */
				if (pixels < 0)
					pixels = 0;
				*result = vm_value_int32((int32_t)pixels);
				*available = 1;
			}
		}
	}

	/* Every host outcome releases the target and owner roots before returning. */
	for (index = 0; index < 2U; index++)
		vm_heap_remove_root(realm->heap, &roots[index]);
	if (status != 0)
		return status;

	/* Succeeded: available distinguishes rendered dimensions from attribute fallback. */
	return 0;
}

/* Reads a rendered image size or its nonrendered content attribute fallback. */
static int
image_dimension(
	struct vm_realm *realm,
	vm_value this_value,
	const char *name,
	vm_value *result)
{
	const struct vm_string *text;
	struct dom_element *element;
	int available;
	int same;
	vm_value attribute;
	uint16_t unit;
	size_t index;
	int32_t pixels;
	int status;

	/* Width and height accessors belong only to actual HTML namespace img elements. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	same = vm_string_equal_ascii(element->local_name, "img");
	if (element->ns != DOM_NS_HTML || !same) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Rendered content dimensions precede the nonrendered content-attribute fallback. */
	status = image_rendered_dimension(realm, element, name, &available, result);
	if (status != 0)
		return status;
	if (available)
		return 0;

	/* The attribute's text. */
	status = element_reflect_get(realm, this_value, name, &attribute);
	if (status != 0)
		return status;
	text = (const struct vm_string *)vm_value_as_cell(attribute);

	/* Skips the leading spaces. */
	pixels = 0;
	index = 0;
	while (index < text->length) {
		unit = vm_string_at(text, index);
		if (unit != ' ' && unit != '\t' && unit != '\n')
			break;
		index++;
	}

	/* Accumulates the digits. */
	for (; index < text->length; index++) {
		/* The digits end at anything else. */
		unit = vm_string_at(text, index);
		if (unit < '0' || unit > '9')
			break;

		/* A number too large to grow further keeps its value so far. */
		if (pixels > 100000000)
			break;

		/* One more decimal place. */
		pixels = pixels * 10 + (int32_t)(unit - '0');
	}

	/* Succeeded: the number. */
	*result = vm_value_int32(pixels);
	return 0;
}

/* Reports a reflected attribute's value, or the empty string when the element does not have it. */
static int
element_reflect_get(
	struct vm_realm *realm,
	vm_value this_value,
	const char *name,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_attribute *attribute;
	struct vm_string *atom;
	int status;

	/* The element and the attribute's atom. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	atom = vm_atom_from_ascii(realm->heap, name);
	if (atom == NULL)
		return ENOMEM;

	/* An attribute the element does not have reflects as the empty string. */
	attribute = dom_element_find_attribute(element, DOM_NS_NONE, atom);
	if (attribute == NULL) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* Succeeded: the value is reported. */
	*result = vm_value_cell(attribute->value);
	return 0;
}

/* Sets a reflected attribute to a value's string. */
static int
element_reflect_set(
	struct vm_realm *realm,
	vm_value this_value,
	const char *name,
	const vm_value *args,
	unsigned count)
{
	struct dom_element *element;
	struct vm_string *atom;
	struct vm_string *value;
	int status;

	/* The element, the attribute's atom and the value. */
	status = element_this(realm, this_value, &element);
	if (status != 0)
		return status;
	atom = vm_atom_from_ascii(realm->heap, name);
	if (atom == NULL)
		return ENOMEM;
	status = bind_to_string(realm, js_argument(args, count, 0), &value);
	if (status != 0)
		return status;

	/* The attribute takes it. */
	status = dom_element_set_attribute(element, atom, value);
	if (status != 0)
		return status;

	/* Succeeded: the attribute is set. */
	return 0;
}
