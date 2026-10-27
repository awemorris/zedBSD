/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inline style of elements for scripts: the style
 * attribute as a CSSStyleDeclaration.
 *
 * The declaration reads and writes the element's style attribute each
 * time it is used: the attribute's text is cut into declarations (name,
 * value and !important), and a change writes them back in the form other
 * browsers serialize them ("name: value;" separated by spaces).  A value
 * is kept only when the style engine reads it (the declaration of a known
 * property with a value it takes, or a custom property), as other browsers
 * keep only what they parse.  The prototype has an accessor for each
 * property the style engine knows, by its CSS name and its camel-case
 * name (and "webkit" and "Webkit" ones for the -webkit- names).  A value
 * generally reads back as written, with selected parsed values put in
 * canonical form.  A shorthand's longhands are not split out of it.  A new
 * declaration is made each time style is read.
 */

#include "bind/internal.h"
#include "css/css.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The longest property name the accessors are made for, with its terminator. */
#define STYLE_NAME_MAX		64U

/*
 * The declarations of a style attribute while one is read or written:
 * three arrays of the same length, the names (lower case, but a custom
 * property's as it is), the values (trimmed, without !important) and the
 * priorities (true for !important).
 */
struct style_list {
	struct vm_object *names;
	struct vm_object *values;
	struct vm_object *priorities;
	struct vm_cell *roots[3];
	unsigned rooted;
};

static int style_css_text_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_css_text_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_length(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_item(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_get_property_value(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_get_property_priority(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_set_property(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_remove_property(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/*
 * The attributes of CSSStyleDeclaration; the properties' accessors are
 * added by bind_style_install.  The table is constant for the life of the
 * program.
 */
static const struct bind_attribute style_attributes[] = {
	{ "cssText", style_css_text_get, style_css_text_set },
	{ "length", style_length, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of CSSStyleDeclaration.  The table is constant for the
 * life of the program.
 */
static const struct bind_operation style_operations[] = {
	{ "item", 1, style_item },
	{ "getPropertyValue", 1, style_get_property_value },
	{ "getPropertyPriority", 1, style_get_property_priority },
	{ "setProperty", 2, style_set_property },
	{ "removeProperty", 1, style_remove_property },
	{ NULL, 0, NULL }
};

/*
 * The CSSStyleDeclaration interface (an element's style).
 */
const struct bind_interface bind_css_style_declaration_interface = {
	"CSSStyleDeclaration", BIND_NO_PARENT, 0, NULL, style_attributes, style_operations, NULL
};

static int style_element(struct vm_realm *realm, vm_value this_value, struct dom_element **element);
static int style_read(struct vm_realm *realm, struct dom_element *element, struct style_list *list);
static int style_parse(struct vm_realm *realm, const uint16_t *units, size_t length, struct style_list *list);
static int style_parse_one(struct vm_realm *realm, const uint16_t *units, size_t start, size_t end, struct style_list *list);
static void style_trim(const uint16_t *units, size_t *start, size_t *end);
static int style_strip_important(const uint16_t *units, size_t start, size_t *end);
static int style_write(struct vm_realm *realm, struct dom_element *element, const struct style_list *list);
static int style_serialize(struct vm_realm *realm, const struct style_list *list, int canonical, struct wb_units *units);
static int style_find(const struct style_list *list, const struct vm_string *name, uint32_t *index);
static int style_set(struct vm_realm *realm, struct dom_element *element, struct vm_string *name, struct vm_string *value, int important);
static int style_remove(struct vm_realm *realm, struct dom_element *element, struct vm_string *name, vm_value *old_value);
static int style_list_create(struct vm_realm *realm, struct style_list *list);
static void style_list_release(struct vm_realm *realm, struct style_list *list);
static int style_list_append(struct vm_realm *realm, struct style_list *list, struct vm_string *name, struct vm_string *value, int important);
static int style_list_remove_at(struct vm_realm *realm, struct style_list *list, uint32_t index);
static int style_remove_from_list(struct vm_realm *realm, struct style_list *list, const struct vm_string *name);
static int style_valid(struct vm_realm *realm, const struct vm_string *name, const struct vm_string *value, int *valid);
static int style_canonical_decoration_line(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);
static int style_canonical_grid_empty_lines(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);
static int style_canonical_grid_auto_flow(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);
static int style_canonical_grid_placement(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);
static int style_decoration_parts(const struct vm_string *value, struct wb_units parts[5]);
static int style_decoration_longhand(const struct vm_string *name);
static int style_grid_covers(const struct vm_string *shorthand, const struct vm_string *name);
static const char *const *style_shorthand_longhands(const struct vm_string *name);
static int style_flex_part(struct vm_realm *realm, const struct vm_string *shorthand, struct vm_string *value, const char *longhand, struct vm_string **part);
static int style_canonical_flex(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);
static int style_wide_keyword(const struct vm_string *value);
static int style_grid_get(struct vm_realm *realm, const struct style_list *list, const struct vm_string *name, vm_value *result, int *present);
static int style_grid_expand(struct vm_realm *realm, struct style_list *list, const struct vm_string *name);
static int style_grid_remove_covered(struct vm_realm *realm, struct style_list *list, const struct vm_string *name, int *removed);
static int style_decoration_get(struct vm_realm *realm, const struct style_list *list, const struct vm_string *name, vm_value *result, int *present);
static int style_decoration_expand(struct vm_realm *realm, struct style_list *list);
static int style_property_name(struct vm_realm *realm, vm_value value, struct vm_string **name);
static int style_accessor(struct bind_window *window, const char *name, struct vm_string *property);
static void style_camel_case(const char *name, int capital, char out[STYLE_NAME_MAX]);
static int style_is_space(uint16_t unit);
static int style_member_get(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_member_set(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_css_supports(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int style_factor_name(const struct vm_string *name);
static int style_canonical_factor(struct vm_realm *realm, const struct vm_string *name, struct vm_string *value, struct vm_string **out);

/*
 * Adds an accessor to CSSStyleDeclaration's prototype for each property
 * the style engine knows: by its CSS name, its camel-case name, and for a
 * -webkit- name also the camel-case name that starts with "Webkit".
 */
int
bind_style_install(
	struct bind_window *window)
{
	struct vm_object *css;
	struct vm_string *property;
	const char *name;
	char camel[STYLE_NAME_MAX];
	size_t index;
	size_t length;
	int webkit;
	int status;

	/* Each name the engine knows. */
	for (index = 0;; index++) {
		name = css_property_name(index);
		if (name == NULL)
			break;
		length = strlen(name);
		if (length + 1U > STYLE_NAME_MAX)
			continue;

		/* The property's name, which the accessors keep. */
		property = vm_atom_from_ascii(window->realm->heap, name);
		if (property == NULL)
			return ENOMEM;

		/* The CSS name, then the camel-case one. */
		status = style_accessor(window, name, property);
		if (status != 0)
			return status;
		style_camel_case(name, 0, camel);
		status = style_accessor(window, camel, property);
		if (status != 0)
			return status;

		/* A -webkit- name also has its "Webkit" form. */
		webkit = strncmp(name, "-webkit-", 8) == 0;
		if (!webkit)
			continue;
		style_camel_case(name, 1, camel);
		status = style_accessor(window, camel, property);
		if (status != 0)
			return status;
	}

	/* Exposes the CSSOM alias for the existing float property. */
	property = vm_atom_from_ascii(window->realm->heap, "float");
	if (property == NULL)
		return ENOMEM;

	/* Both inline and computed declarations use the ordinary float accessor. */
	status = style_accessor(window, "cssFloat", property);
	if (status != 0)
		return status;

	/* The CSS namespace consults the same declaration parser as inline style. */
	css = vm_object_create(window->realm->heap, window->realm->object_prototype);
	if (css == NULL)
		return ENOMEM;
	status = js_builtin_value(window->realm, window->realm->global, "CSS", vm_value_cell(css), JS_BUILTIN_METHOD);
	if (status != 0)
		return status;
	status = js_builtin_method(window->realm, css, "supports", 2, style_css_supports);
	if (status != 0)
		return status;

	/* Succeeded: the prototype has the accessors. */
	return 0;
}

/*
 * Makes the CSSStyleDeclaration of an element's style attribute (style).
 */
int
bind_style(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_object *declaration;
	struct dom_node *node;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The element. */
	window = bind_window_of(realm);
	node = bind_node_of(this_value);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* The declaration, which keeps the element's object. */
	declaration = vm_object_create(realm->heap, window->prototypes[BIND_CSS_STYLE_DECLARATION]);
	if (declaration == NULL)
		return ENOMEM;
	declaration->kind = VM_KIND_PLATFORM;
	declaration->internal = this_value;

	/* Succeeded: the declaration. */
	*result = vm_value_cell(declaration);
	return 0;
}

/*
 * Sets an element's style from a text, as setting its cssText does
 * (element.style = "...").
 */
int
bind_style_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	vm_value declaration;
	int status;

	/* The element's declaration. */
	status = bind_style(realm, this_value, args, count, &declaration);
	if (status != 0)
		return status;

	/* Succeeded: its text is set. */
	status = style_css_text_set(realm, declaration, args, count, result);
	if (status != 0)
		return status;
	return 0;
}

/* Answers CSS.supports for a property/value or a simple parenthesized declaration. */
static int
style_css_supports(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *name;
	struct vm_string *value;
	struct vm_string *condition;
	struct vm_cell *name_root;
	struct wb_units units;
	size_t start;
	size_t end;
	int valid;
	int status;

	UNUSED_PARAMETER(this_value);
	valid = 0;
	if (count >= 2U) {
		/* The two-argument form uses the engine's actual declaration grammar. */
		status = style_property_name(realm, args[0], &name);
		if (status != 0)
			return status;
		name_root = &name->cell;
		status = vm_heap_add_root(realm->heap, &name_root);
		if (status != 0)
			return status;
		status = bind_to_string(realm, args[1], &value);
		if (status != 0) {
			vm_heap_remove_root(realm->heap, &name_root);
			return status;
		}
		status = style_valid(realm, name, value, &valid);
		vm_heap_remove_root(realm->heap, &name_root);
		if (status != 0)
			return status;
	} else {
		/* A basic condition is one declaration enclosed in parentheses. */
		status = bind_to_string(realm, count > 0U ? args[0] : VM_VALUE_UNDEFINED, &condition);
		if (status != 0)
			return status;
		wb_units_init(&units);
		status = vm_string_append_units(condition, &units);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
		start = 0;
		end = units.length;
		while (start < end && style_is_space(units.data[start]))
			start++;
		while (end > start && style_is_space(units.data[end - 1U]))
			end--;
		if (end > start + 2U && units.data[start] == '(' && units.data[end - 1U] == ')')
			status = css_declaration_valid(realm->heap, units.data + start + 1U, end - start - 2U, &valid);
		wb_units_release(&units);
		if (status != 0)
			return status;
	}

	/* Unsupported declarations and conditions are false. */
	*result = vm_value_boolean(valid);
	return 0;
}

/* Finds the element of the CSSStyleDeclaration a method's this value stands for. */
static int
style_element(
	struct vm_realm *realm,
	vm_value this_value,
	struct dom_element **element)
{
	struct vm_object *object;
	struct dom_node *node;
	int is_object;
	int computed;
	int status;

	/* A computed declaration (ws074-p082) reaches here only to be changed, which it cannot be. */
	computed = bind_computed_element(this_value, element);
	if (computed) {
		status = bind_throw_dom(realm, "NoModificationAllowedError", "The computed style cannot be modified.");
		return status;
	}

	/* A platform object. */
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

	/* That keeps an element's object. */
	node = bind_node_of(object->internal);
	if (node == NULL || node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the element. */
	*element = (struct dom_element *)node;
	return 0;
}

/* Reads the declarations of an element's style attribute (none without the attribute). */
static int
style_read(
	struct vm_realm *realm,
	struct dom_element *element,
	struct style_list *list)
{
	struct dom_attribute *attribute;
	struct vm_string *style_atom;
	struct vm_string *source;
	struct wb_units units;
	int status;

	/* Empty lists. */
	status = style_list_create(realm, list);
	if (status != 0)
		return status;

	/* CSSOM's exact source is authoritative until an external attribute change reparses it. */
	source = element->inline_style_source;
	if (source == NULL) {
		style_atom = vm_atom_from_ascii(realm->heap, "style");
		if (style_atom == NULL) {
			style_list_release(realm, list);
			return ENOMEM;
		}

		/* A declaration block without a current style attribute is empty. */
		attribute = dom_element_find_attribute(element, DOM_NS_NONE, style_atom);
		if (attribute == NULL)
			return 0;
		source = attribute->value;
	}

	/* Its characters, cut into declarations. */
	wb_units_init(&units);
	status = vm_string_append_units(source, &units);
	if (status == 0)
		status = style_parse(realm, units.data, units.length, list);
	wb_units_release(&units);
	if (status != 0) {
		style_list_release(realm, list);
		return status;
	}

	/* Succeeded: the declarations. */
	return 0;
}

/*
 * Cuts a declaration block's text into declarations at the semicolons
 * outside parentheses and quotes.
 */
static int
style_parse(
	struct vm_realm *realm,
	const uint16_t *units,
	size_t length,
	struct style_list *list)
{
	size_t start;
	size_t at;
	uint16_t quote;
	uint16_t unit;
	int depth;
	int status;

	/* Each declaration up to its semicolon or the end. */
	start = 0;
	at = 0;
	depth = 0;
	quote = 0;
	while (at <= length) {
		/* The end ends the last declaration. */
		unit = ';';
		if (at < length)
			unit = units[at];

		/* Chooses what the character does. */
		if (quote != 0 && at < length) {
			/* Inside quotes only the closing quote and an escape matter. */
			if (unit == '\\') {
				at++;
			} else if (unit == quote) {
				quote = 0;
			}
		} else if (unit == '"' || unit == '\'') {
			quote = unit;
		} else if (unit == '(') {
			depth++;
		} else if (unit == ')' && depth > 0) {
			depth--;
		} else if (unit == ';' && (depth == 0 || at == length)) {
			/* A declaration ends. */
			status = style_parse_one(realm, units, start, at, list);
			if (status != 0)
				return status;
			start = at + 1U;
		}

		/* The next character. */
		at++;
	}

	/* Succeeded: every declaration is read. */
	return 0;
}

/*
 * Reads one declaration ("name: value" with an optional !important) and
 * appends it; one without a name or a value is dropped.
 */
static int
style_parse_one(
	struct vm_realm *realm,
	const uint16_t *units,
	size_t start,
	size_t end,
	struct style_list *list)
{
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *name_root;
	struct vm_cell *value_root;
	int valid;
	int factor;
	struct wb_units lower;
	size_t colon;
	size_t name_start;
	size_t name_end;
	size_t value_start;
	size_t index;
	int important;
	int custom;
	int status;

	/* The colon between the name and the value. */
	colon = start;
	while (colon < end &&
	       units[colon] != ':')
		colon++;
	if (colon >= end)
		return 0;

	/* The name and the value, whitespace trimmed, the value without its !important. */
	name_start = start;
	name_end = colon;
	style_trim(units, &name_start, &name_end);
	value_start = colon + 1U;
	style_trim(units, &value_start, &end);
	important = style_strip_important(units, value_start, &end);

	/* Nothing on either side is no declaration. */
	if (name_end == name_start ||
	    end == value_start)
		return 0;

	/* The name's characters. */
	wb_units_init(&lower);
	status = wb_units_append(&lower, units + name_start, name_end - name_start);
	if (status != 0) {
		wb_units_release(&lower);
		return status;
	}

	/* In lower case, but a custom property's as it is. */
	custom = 0;
	if (lower.length >= 2U &&
	    lower.data[0] == '-' &&
	    lower.data[1] == '-')
		custom = 1;
	for (index = 0; !custom && index < lower.length; index++) {
		if (lower.data[index] >= 'A' &&
		    lower.data[index] <= 'Z')
			lower.data[index] = (uint16_t)(lower.data[index] + 0x20U);
	}

	/* The name's atom. */
	name = vm_atom_from_units(realm->heap, lower.data, lower.length);
	wb_units_release(&lower);
	if (name == NULL)
		return ENOMEM;
	name_root = &name->cell;
	status = vm_heap_add_root(realm->heap, &name_root);
	if (status != 0)
		return status;

	/* The value as it is written. */
	value = vm_string_from_units(realm->heap, units + value_start, end - value_start);
	if (value == NULL) {
		vm_heap_remove_root(realm->heap, &name_root);
		return ENOMEM;
	}

	/* Validation may allocate, so a fresh value has its own placeholder before native parsing. */
	value_root = &value->cell;
	status = vm_heap_add_root(realm->heap, &value_root);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &name_root);
		return status;
	}

	/* Raw style attributes obey the selected factor grammar as well as CSSOM assignments. */
	valid = 1;
	factor = style_factor_name(name);
	if (factor)
		status = style_valid(realm, name, value, &valid);
	if (status == 0 &&
	    valid)
		status = style_list_append(realm, list, name, value, important);
	vm_heap_remove_root(realm->heap, &value_root);
	vm_heap_remove_root(realm->heap, &name_root);
	if (status != 0)
		return status;

	/* Succeeded: the valid declaration was kept, or the invalid selected factor was ignored. */
	return 0;
}

/* Moves the start of a run of characters past its leading whitespace and its end before its trailing whitespace. */
static void
style_trim(
	const uint16_t *units,
	size_t *start,
	size_t *end)
{
	int space;

	/* The leading whitespace. */
	while (*start < *end) {
		space = style_is_space(units[*start]);
		if (!space)
			break;
		(*start)++;
	}

	/* The trailing whitespace. */
	while (*end > *start) {
		space = style_is_space(units[*end - 1U]);
		if (!space)
			break;
		(*end)--;
	}
}

/*
 * Tells whether a trimmed value ends in "!important" (the word in any
 * case, whitespace allowed after the "!"), and if it does moves its end
 * before the "!" and the whitespace before it.
 */
static int
style_strip_important(
	const uint16_t *units,
	size_t start,
	size_t *end)
{
	static const char word[] = "important";
	size_t length;
	size_t bang;
	size_t index;
	uint16_t unit;

	/* The value must be long enough for the word and a "!". */
	length = sizeof(word) - 1U;
	if (*end - start < length + 1U)
		return 0;

	/* The word at the end, in any case. */
	bang = *end - length;
	for (index = 0; index < length; index++) {
		unit = units[bang + index];
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		if (unit != (uint16_t)word[index])
			return 0;
	}

	/* The "!" before it, after optional whitespace. */
	style_trim(units, &start, &bang);
	if (bang == start || units[bang - 1U] != '!')
		return 0;

	/* Succeeded: the value ends before the "!" and the whitespace before it. */
	bang--;
	style_trim(units, &start, &bang);
	*end = bang;
	return 1;
}

/* Writes declarations to an element's style attribute as other browsers serialize them. */
static int
style_write(
	struct vm_realm *realm,
	struct dom_element *element,
	const struct style_list *list)
{
	struct vm_string *source;
	struct vm_string *text;
	struct vm_cell *roots[2];
	struct wb_units raw;
	struct wb_units visible;
	unsigned index;
	unsigned registered;
	int status;

	/* Separate exact declaration source from its public canonical string before touching the DOM. */
	wb_units_init(&raw);
	wb_units_init(&visible);
	roots[0] = NULL;
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;

		/* Cleanup owns only successfully acquired string placeholders. */
		registered++;
	}

	/* Raw list values retain precision, while public number components use the canonical display representation. */
	status = style_serialize(realm, list, 0, &raw);
	if (status != 0)
		goto cleanup;
	status = style_serialize(realm, list, 1, &visible);
	if (status != 0)
		goto cleanup;
	source = vm_string_from_units(realm->heap, raw.data, raw.length);
	if (source == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The exact source survives the following serialized-string allocation. */
	roots[0] = &source->cell;
	text = vm_string_from_units(realm->heap, visible.data, visible.length);
	if (text == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The DOM owns updating-flag, commit-versus-notification and reentrant restoration semantics. */
	roots[1] = &text->cell;
	status = dom_element_set_style_attribute(element, source, text);

cleanup:
	/* Native buffers and acquired roots never outlive this synchronous publication frame. */
	wb_units_release(&visible);
	wb_units_release(&raw);
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Preserve conversion, allocation and committed notification errors. */
	if (status != 0)
		return status;

	/* Succeeded: exact specified declarations and public attribute serialization are synchronized. */
	return 0;
}

/* Writes declarations as "name: value;" (with " !important" when they are), separated by spaces. */
static int
style_serialize(
	struct vm_realm *realm,
	const struct style_list *list,
	int canonical,
	struct wb_units *units)
{
	static const uint16_t colon[] = {':', ' '};
	static const uint16_t important[] = {' ', '!', 'i', 'm', 'p', 'o', 'r', 't', 'a', 'n', 't'};
	static const uint16_t semicolon = ';';
	static const uint16_t space = ' ';
	const struct vm_string *name;
	struct vm_string *value;
	uint32_t index;
	int priority;
	int status;

	/* Each declaration. */
	status = 0;
	for (index = 0; index < list->names->length && status == 0; index++) {
		name = (const struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		value = (struct vm_string *)vm_value_as_cell(list->values->elements[index]);
		priority = vm_to_boolean(list->priorities->elements[index]);

		/* A public view canonicalizes a component without rewriting the underlying declaration value. */
		if (canonical) {
			status = style_canonical_factor(realm, name, value, &value);
			if (status != 0)
				break;
		}

		/* A space before every declaration but the first. */
		if (index > 0)
			status = wb_units_append(units, &space, 1);

		/* The name, the colon, the value, the priority and the semicolon. */
		if (status == 0)
			status = vm_string_append_units(name, units);
		if (status == 0)
			status = wb_units_append(units, colon, 2);
		if (status == 0)
			status = vm_string_append_units(value, units);
		if (status == 0 &&
		    priority)
			status = wb_units_append(units, important, sizeof(important) / sizeof(important[0]));
		if (status == 0)
			status = wb_units_append(units, &semicolon, 1);
	}

	/* A failure to grow the text. */
	if (status != 0)
		return status;

	/* Succeeded: the text. */
	return 0;
}

/* Tells whether declarations have a property, and where the first one is. */
static int
style_find(
	const struct style_list *list,
	const struct vm_string *name,
	uint32_t *index)
{
	const struct vm_string *other;
	uint32_t at;
	int same;

	/* Compares each name. */
	for (at = 0; at < list->names->length; at++) {
		other = (const struct vm_string *)vm_value_as_cell(list->names->elements[at]);
		same = vm_string_equal(other, name);
		if (same) {
			*index = at;
			return 1;
		}
	}

	/* The property is not there. */
	return 0;
}

/*
 * Sets a property of an element's style to a value (the value is kept
 * only when the style engine reads it): the first declaration of it takes
 * the value and the others go, or a new one is appended.
 */
static int
style_set(
	struct vm_realm *realm,
	struct dom_element *element,
	struct vm_string *name,
	struct vm_string *value,
	int important)
{
	struct style_list list;
	struct vm_cell *roots[2];
	uint32_t index;
	uint32_t later;
	unsigned registered;
	int present;
	int valid;
	int status;

	/* Caller strings survive validation, list creation and attribute publication. */
	roots[0] = &name->cell;
	roots[1] = &value->cell;
	registered = 0;
	while (registered < 2U) {
		status = vm_heap_add_root(realm->heap, &roots[registered]);
		if (status != 0)
			goto roots_cleanup;
		registered++;
	}

	/* A value the engine does not read changes nothing. */
	status = style_valid(realm, name, value, &valid);
	if (status != 0)
		goto roots_cleanup;
	if (!valid)
		goto roots_cleanup;
	status = style_canonical_decoration_line(realm, name, value, &value);
	if (status != 0)
		goto roots_cleanup;
	roots[1] = &value->cell;
	status = style_canonical_grid_empty_lines(realm, name, value, &value);
	if (status != 0)
		goto roots_cleanup;
	roots[1] = &value->cell;
	status = style_canonical_grid_auto_flow(realm, name, value, &value);
	if (status != 0)
		goto roots_cleanup;
	roots[1] = &value->cell;
	status = style_canonical_grid_placement(realm, name, value, &value);
	if (status != 0)
		goto roots_cleanup;
	roots[1] = &value->cell;
	status = style_canonical_flex(realm, name, value, &value);
	if (status != 0)
		goto roots_cleanup;
	roots[1] = &value->cell;

	/* The declarations as they are. */
	status = style_read(realm, element, &list);
	if (status != 0)
		goto roots_cleanup;
	if (vm_string_equal_ascii(name, "text-decoration")) {
		/* A later shorthand replaces all four earlier longhand values. */
		index = 0U;
		while (index < list.names->length) {
			struct vm_string *earlier;

			earlier = (struct vm_string *)vm_value_as_cell(list.names->elements[index]);
			if (!style_decoration_longhand(earlier)) {
				index++;
				continue;
			}
			status = style_list_remove_at(realm, &list, index);
			if (status != 0)
				goto cleanup;
		}
	}
	if (style_decoration_longhand(name)) {
		status = style_decoration_expand(realm, &list);
		if (status != 0)
			goto cleanup;
	}

	/* A grid shorthand replaces its earlier longhands; a longhand splits a grid shorthand that sets it. */
	status = style_grid_remove_covered(realm, &list, name, &present);
	if (status == 0)
		status = style_grid_expand(realm, &list, name);
	if (status != 0)
		goto cleanup;

	/* A property not there is appended. */
	present = style_find(&list, name, &index);
	if (!present) {
		status = style_list_append(realm, &list, name, value, important);
		if (status != 0)
			goto cleanup;
	} else {
		/* Otherwise its first declaration takes the value and the later ones go. */
		list.values->elements[index] = vm_value_cell(value);
		list.priorities->elements[index] = vm_value_boolean(important);
		later = index + 1U;
		while (later < list.names->length) {
			present = vm_string_equal((const struct vm_string *)vm_value_as_cell(list.names->elements[later]), name);
			if (!present) {
				later++;
				continue;
			}

			/* A later declaration of it. */
			status = style_list_remove_at(realm, &list, later);
			if (status != 0)
				goto cleanup;
		}
	}

	/* Succeeded: the attribute holds the declarations. */
	status = style_write(realm, element, &list);

cleanup:
	/* The serialized attribute now owns the values; no caller root remains. */
	style_list_release(realm, &list);

roots_cleanup:
	/* Inputs leave the root set after every validation or mutation outcome. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Reports a failed validation or attribute update. */
	if (status != 0)
		return status;
	return 0;
}

/*
 * Removes every declaration of a property from an element's style, and
 * reports the value it had (the empty string when it had none).
 */
static int
style_remove(
	struct vm_realm *realm,
	struct dom_element *element,
	struct vm_string *name,
	vm_value *old_value)
{
	struct style_list list;
	struct vm_cell *old_root;
	struct vm_string *value;
	struct vm_cell *name_root;
	uint32_t index;
	int present;
	int removed;
	int decoration;
	int old_registered;
	int status;

	/* The requested property remains live while its declaration list is built. */
	name_root = &name->cell;
	status = vm_heap_add_root(realm->heap, &name_root);
	if (status != 0)
		return status;

	/* The declarations as they are. */
	status = style_read(realm, element, &list);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &name_root);
		return status;
	}

	/* The last removed value remains live until the mutation returns. */
	old_root = NULL;
	old_registered = 0;
	status = vm_heap_add_root(realm->heap, &old_root);
	if (status != 0)
		goto cleanup;
	old_registered = 1;
	status = bind_string(realm, "", old_value);
	if (status != 0)
		goto cleanup;
	old_root = vm_value_as_cell(*old_value);
	decoration = style_decoration_longhand(name);
	if (decoration) {
		status = style_decoration_expand(realm, &list);
		if (status != 0)
			goto cleanup;
	}

	/* A grid longhand comes out of a shorthand that sets it; a grid shorthand takes its longhands with it. */
	removed = 0;
	status = style_grid_expand(realm, &list, name);
	if (status != 0)
		goto cleanup;
	status = style_grid_remove_covered(realm, &list, name, &removed);
	if (status != 0)
		goto cleanup;

	/* Each declaration of the property, the last one's value reported. */
	for (;;) {
		present = style_find(&list, name, &index);
		if (!present)
			break;
		/* The returned old value uses the same canonical view as its getter. */
		value = (struct vm_string *)vm_value_as_cell(list.values->elements[index]);
		status = style_canonical_factor(realm, name, value, &value);
		if (status != 0)
			goto cleanup;
		*old_value = vm_value_cell(value);
		old_root = &value->cell;
		status = style_list_remove_at(realm, &list, index);
		if (status != 0)
			goto cleanup;
		removed = 1;
	}

	/* Nothing removed leaves the attribute as it is. */
	if (!removed)
		goto cleanup;

	/* Succeeded: the attribute holds the rest. */
	status = style_write(realm, element, &list);

cleanup:
	/* The caller receives a value after the transient list is no longer used. */
	style_list_release(realm, &list);
	if (old_registered)
		vm_heap_remove_root(realm->heap, &old_root);
	vm_heap_remove_root(realm->heap, &name_root);
	if (status != 0)
		return status;
	return 0;
}

/* Makes three empty arrays for declarations. */
static int
style_list_create(
	struct vm_realm *realm,
	struct style_list *list)
{
	unsigned index;
	int status;

	/* Stable caller slots protect each unpublished array during every allocation. */
	list->names = NULL;
	list->values = NULL;
	list->priorities = NULL;
	list->rooted = 0;
	for (index = 0; index < 3U; index++) {
		list->roots[index] = NULL;
		status = vm_heap_add_root(realm->heap, &list->roots[index]);
		if (status != 0)
			goto failed;
		list->rooted++;
	}

	/* The names. */
	status = bind_array_create(realm, &list->names);
	if (status != 0)
		goto failed;
	list->roots[0] = &list->names->cell;

	/* The values. */
	status = bind_array_create(realm, &list->values);
	if (status != 0)
		goto failed;
	list->roots[1] = &list->values->cell;

	/* Succeeded: and the priorities. */
	status = bind_array_create(realm, &list->priorities);
	if (status != 0)
		goto failed;
	list->roots[2] = &list->priorities->cell;
	return 0;

failed:
	/* A failed list factory leaves no registered caller stack slots. */
	style_list_release(realm, list);
	return status;
}

/* Releases all collector roots held by one temporary declaration list. */
static void
style_list_release(
	struct vm_realm *realm,
	struct style_list *list)
{
	/* Only registered slots can be removed after partial construction. */
	while (list->rooted != 0U) {
		list->rooted--;
		vm_heap_remove_root(realm->heap, &list->roots[list->rooted]);
	}
}

/* Appends one declaration. */
static int
style_list_append(
	struct vm_realm *realm,
	struct style_list *list,
	struct vm_string *name,
	struct vm_string *value,
	int important)
{
	struct vm_cell *roots[2];
	vm_value at;
	unsigned registered;
	int status;

	/* Keep both new strings live until their arrays own them. */
	roots[0] = &name->cell;
	roots[1] = &value->cell;
	registered = 0;
	while (registered < 2U) {
		status = vm_heap_add_root(realm->heap, &roots[registered]);
		if (status != 0)
			goto cleanup;
		registered++;
	}
	status = style_canonical_decoration_line(realm, name, value, &value);
	if (status != 0)
		goto cleanup;
	roots[1] = &value->cell;

	/* The place after the last. */
	at = vm_value_int32((int32_t)list->names->length);

	/* The name. */
	status = vm_object_define(realm->heap, list->names, at, vm_value_cell(name), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* The value. */
	status = vm_object_define(realm->heap, list->values, at, vm_value_cell(value), VM_PROPERTY_DEFAULT);
	if (status != 0)
		goto cleanup;

	/* Succeeded: and the priority. */
	status = vm_object_define(realm->heap, list->priorities, at, vm_value_boolean(important), VM_PROPERTY_DEFAULT);

cleanup:
	/* The list now owns any installed string on both success and partial failure. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Reports a failed array insertion. */
	if (status != 0)
		return status;
	return 0;
}

/* Removes the declaration at a place, moving the later ones down. */
static int
style_list_remove_at(
	struct vm_realm *realm,
	struct style_list *list,
	uint32_t index)
{
	uint32_t at;
	uint32_t length;
	int status;

	/* Each later declaration one place down. */
	length = list->names->length;
	for (at = index; at + 1U < length; at++) {
		list->names->elements[at] = list->names->elements[at + 1U];
		list->values->elements[at] = list->values->elements[at + 1U];
		list->priorities->elements[at] = list->priorities->elements[at + 1U];
	}

	/* The names are one shorter. */
	status = vm_array_set_length(realm->heap, list->names, length - 1U);
	if (status != 0)
		return status;

	/* The values. */
	status = vm_array_set_length(realm->heap, list->values, length - 1U);
	if (status != 0)
		return status;

	/* Succeeded: and the priorities. */
	status = vm_array_set_length(realm->heap, list->priorities, length - 1U);
	if (status != 0)
		return status;
	return 0;
}

/* Removes every declaration of a property from declarations. */
static int
style_remove_from_list(
	struct vm_realm *realm,
	struct style_list *list,
	const struct vm_string *name)
{
	uint32_t index;
	int present;
	int status;

	/* Each declaration of it, until none is left. */
	for (;;) {
		present = style_find(list, name, &index);
		if (!present)
			break;
		status = style_list_remove_at(realm, list, index);
		if (status != 0)
			return status;
	}

	/* Succeeded: the property is not there. */
	return 0;
}

/* Tells whether the style engine reads a property with a value. */
static int
style_valid(
	struct vm_realm *realm,
	const struct vm_string *name,
	const struct vm_string *value,
	int *valid)
{
	static const uint16_t colon = ':';
	struct wb_units text;
	int status;

	/* The declaration's text. */
	wb_units_init(&text);
	status = vm_string_append_units(name, &text);
	if (status == 0)
		status = wb_units_append(&text, &colon, 1);
	if (status == 0)
		status = vm_string_append_units(value, &text);
	if (status != 0) {
		wb_units_release(&text);
		return status;
	}

	/* The engine's answer. */
	status = css_declaration_valid(realm->heap, text.data, text.length, valid);
	wb_units_release(&text);
	if (status != 0)
		return status;

	/* Succeeded: the answer is in *valid. */
	return 0;
}

/* Identify the selected native longhand factor components without widening other property grammars. */
static int
style_factor_name(
	const struct vm_string *name)
{
	int same;

	/* The grow factor has one nonnegative numeric component. */
	same = vm_string_equal_ascii(name, "flex-grow");
	if (same)
		return 1;

	/* The shrink factor uses the same declared-number serialization. */
	same = vm_string_equal_ascii(name, "flex-shrink");
	if (!same)
		return 0;

	/* Succeeded: this is a selected factor longhand. */
	return 1;
}

/* Build a canonical declared-number view while leaving exact native source values untouched. */
static int
style_canonical_factor(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	struct wb_units source;
	struct wb_units canonical;
	int factor;
	int handled;
	int status;

	/* Other properties, custom properties and nonnumeric grammar keep their existing representation. */
	*out = value;
	factor = style_factor_name(name);
	if (!factor)
		return 0;
	wb_units_init(&source);
	wb_units_init(&canonical);
	status = vm_string_append_units(value, &source);
	if (status != 0)
		goto cleanup;
	status = css_number_serialize(source.data, source.length, &canonical, &handled);
	if (status != 0)
		goto cleanup;
	if (handled) {
		*out = vm_string_from_units(realm->heap, canonical.data, canonical.length);
		if (*out == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
	}

cleanup:
	/* No tokenizer or formatting buffer borrows VM string storage after returning. */
	wb_units_release(&canonical);
	wb_units_release(&source);
	if (status != 0)
		return status;

	/* Succeeded: the caller receives a new view or the original untouched component. */
	return 0;
}

/* Puts the valid plain grid-auto-flow keywords in their canonical order. */
static int
style_canonical_grid_auto_flow(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	struct wb_units source;
	const char *canonical;
	char word[7];
	size_t index;
	size_t length;
	int column;
	int dense;
	int status;

	*out = value;
	if (!vm_string_equal_ascii(name, "grid-auto-flow"))
		return 0;
	wb_units_init(&source);
	status = vm_string_append_units(value, &source);
	if (status != 0) {
		wb_units_release(&source);
		return status;
	}
	column = 0;
	dense = 0;
	index = 0;
	while (index < source.length) {
		while (index < source.length && style_is_space(source.data[index]))
			index++;
		if (index == source.length)
			break;
		length = 0;
		while (index < source.length && !style_is_space(source.data[index])) {
			if (length >= sizeof(word) - 1U || source.data[index] > 127U) {
				wb_units_release(&source);
				return 0;
			}
			word[length] = (char)source.data[index];
			if (word[length] >= 'A' && word[length] <= 'Z')
				word[length] = (char)(word[length] + 0x20);
			length++;
			index++;
		}
		word[length] = '\0';
		if (strcmp(word, "column") == 0)
			column = 1;
		else if (strcmp(word, "dense") == 0)
			dense = 1;
		else if (strcmp(word, "row") != 0) {
			/* Escaped identifiers and CSS-wide values retain their spelling. */
			wb_units_release(&source);
			return 0;
		}
	}
	wb_units_release(&source);
	canonical = "row";
	if (column)
		canonical = dense ? "column dense" : "column";
	else if (dense)
		canonical = "dense";
	*out = vm_atom_from_ascii(realm->heap, canonical);
	if (*out == NULL)
		return ENOMEM;
	return 0;
}

/* Omits empty Grid line-name groups from an already validated inline style value. */
static int
style_canonical_grid_empty_lines(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	struct wb_units source;
	struct wb_units canonical;
	size_t index;
	size_t closing;
	size_t next;
	uint16_t space;
	int changed;
	int status;

	*out = value;
	if (!vm_string_equal_ascii(name, "grid-template-columns") &&
	    !vm_string_equal_ascii(name, "grid-template-rows"))
		return 0;
	wb_units_init(&source);
	status = vm_string_append_units(value, &source);
	if (status != 0) {
		wb_units_release(&source);
		return status;
	}
	wb_units_init(&canonical);
	space = ' ';
	changed = 0;
	index = 0;
	while (index < source.length) {
		/* An empty group may contain whitespace but no line name. */
		if (source.data[index] == '[') {
			closing = index + 1U;
			while (closing < source.length && style_is_space(source.data[closing]))
				closing++;
			if (closing < source.length && source.data[closing] == ']') {
				changed = 1;
				while (canonical.length != 0U && style_is_space(canonical.data[canonical.length - 1U]))
					canonical.length--;
				next = closing + 1U;
				while (next < source.length && style_is_space(source.data[next]))
					next++;
				if (canonical.length != 0U && next < source.length && source.data[next] != ')') {
					status = wb_units_append(&canonical, &space, 1U);
					if (status != 0)
						goto cleanup;
				}
				index = next;
				continue;
			}
		}
		status = wb_units_append(&canonical, source.data + index, 1U);
		if (status != 0)
			goto cleanup;
		index++;
	}
	if (changed) {
		*out = vm_string_from_units(realm->heap, canonical.data, canonical.length);
		if (*out == NULL)
			status = ENOMEM;
	}

cleanup:
	wb_units_release(&canonical);
	wb_units_release(&source);
	return status;
}

/* Canonicalizes grid placement and grid-template-areas values from their parsed form (the engine writes them). */
static int
style_canonical_grid_placement(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	static const uint16_t colon = ':';
	struct wb_units text;
	struct wb_units canonical;
	int handled;
	int status;

	/* The value as it is unless the engine writes it. */
	*out = value;

	/* The declaration's text. */
	wb_units_init(&text);
	status = vm_string_append_units(name, &text);
	if (status == 0)
		status = wb_units_append(&text, &colon, 1);
	if (status == 0)
		status = vm_string_append_units(value, &text);
	if (status != 0) {
		wb_units_release(&text);
		return status;
	}

	/* The engine's canonical text, for the properties it writes. */
	wb_units_init(&canonical);
	status = css_grid_declaration_serialize(realm->heap, text.data, text.length, &canonical, &handled);
	wb_units_release(&text);
	if (status != 0) {
		wb_units_release(&canonical);
		return status;
	}

	/* The canonical value as a string. */
	if (handled) {
		*out = vm_string_from_units(realm->heap, canonical.data, canonical.length);
		if (*out == NULL) {
			wb_units_release(&canonical);
			return ENOMEM;
		}
	}
	wb_units_release(&canonical);

	/* Succeeded: *out is the value to keep. */
	return 0;
}

/* Canonicalizes parsed decoration keywords and the complete shorthand. */
static int
style_canonical_decoration_line(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	struct wb_units source;
	struct wb_units canonical;
	struct wb_units parts[4];
	size_t index;
	int shorthand;
	int lines;
	int status;

	*out = value;
	shorthand = vm_string_equal_ascii(name, "text-decoration");
	if (!shorthand && !vm_string_equal_ascii(name, "text-decoration-line"))
		return 0;
	wb_units_init(&source);
	status = vm_string_append_units(value, &source);
	if (status != 0) {
		wb_units_release(&source);
		return status;
	}
	wb_units_init(&canonical);
	if (shorthand) {
		for (index = 0U; index < 4U; index++)
			wb_units_init(&parts[index]);
		status = css_decoration_shorthand_serialize(source.data, source.length,
		    &canonical, &parts[0], &parts[1], &parts[2], &parts[3]);
		for (index = 0U; index < 4U; index++)
			wb_units_release(&parts[index]);
	} else {
		status = css_decoration_line_parse(source.data, source.length, &lines);
		if (status == 0)
			status = css_decoration_line_serialize(lines, &canonical);
	}
	wb_units_release(&source);
	if (status == EINVAL) {
		wb_units_release(&canonical);
		return 0;
	}
	if (status != 0) {
		wb_units_release(&canonical);
		return status;
	}
	*out = vm_string_from_units(realm->heap, canonical.data, canonical.length);
	wb_units_release(&canonical);
	if (*out == NULL)
		return ENOMEM;
	return 0;
}

/* Splits a stored decoration shorthand through the production CSS parser. */
static int
style_decoration_parts(const struct vm_string *value, struct wb_units parts[5])
{
	struct wb_units source;
	size_t index;
	int status;

	wb_units_init(&source);
	for (index = 0U; index < 5U; index++)
		wb_units_init(&parts[index]);
	status = vm_string_append_units(value, &source);
	if (status == 0 &&
	    (vm_string_equal_ascii(value, "inherit") || vm_string_equal_ascii(value, "initial") ||
	    vm_string_equal_ascii(value, "unset") || vm_string_equal_ascii(value, "revert") ||
	    vm_string_equal_ascii(value, "revert-layer"))) {
		for (index = 0U; index < 5U; index++) {
			status = wb_units_append(&parts[index], source.data, source.length);
			if (status != 0)
				break;
		}
	} else if (status == 0) {
		status = css_decoration_shorthand_serialize(source.data, source.length,
		    &parts[0], &parts[1], &parts[2], &parts[3], &parts[4]);
	}
	wb_units_release(&source);
	return status;
}

/* The four longhands of text-decoration have stable positions in the parser result. */
static int
style_decoration_longhand(const struct vm_string *name)
{
	if (vm_string_equal_ascii(name, "text-decoration-line"))
		return 1;
	if (vm_string_equal_ascii(name, "text-decoration-thickness"))
		return 2;
	if (vm_string_equal_ascii(name, "text-decoration-style"))
		return 3;
	if (vm_string_equal_ascii(name, "text-decoration-color"))
		return 4;
	return 0;
}

/* Reads a longhand from a shorthand that has not yet been expanded. */
static int
style_decoration_get(
	struct vm_realm *realm,
	const struct style_list *list,
	const struct vm_string *name,
	vm_value *result,
	int *present)
{
	struct wb_units parts[5];
	struct vm_string *value;
	size_t index;
	size_t part;
	int status;

	*present = 0;
	part = (size_t)style_decoration_longhand(name);
	if (part == 0U)
		return 0;
	for (index = 0U; index < list->names->length; index++) {
		value = (struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		if (vm_string_equal_ascii(value, "text-decoration"))
			break;
	}
	if (index == list->names->length)
		return 0;
	value = (struct vm_string *)vm_value_as_cell(list->values->elements[index]);
	status = style_decoration_parts(value, parts);
	if (status == 0) {
		value = vm_string_from_units(realm->heap, parts[part].data, parts[part].length);
		if (value == NULL)
			status = ENOMEM;
		else {
			*result = vm_value_cell(value);
			*present = 1;
		}
	}
	for (index = 0U; index < 5U; index++)
		wb_units_release(&parts[index]);
	return status;
}

/* Replaces a shorthand with its longhands before a longhand mutation. */
static int
style_decoration_expand(struct vm_realm *realm, struct style_list *list)
{
	static const char *const names[4] = {
		"text-decoration-line", "text-decoration-thickness",
		"text-decoration-style", "text-decoration-color"
	};
	struct wb_units parts[5];
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *name_root;
	size_t index;
	size_t part;
	int important;
	int status;

	for (index = 0U; index < list->names->length; index++) {
		name = (struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		if (vm_string_equal_ascii(name, "text-decoration"))
			break;
	}
	if (index == list->names->length)
		return 0;
	value = (struct vm_string *)vm_value_as_cell(list->values->elements[index]);
	important = vm_to_boolean(list->priorities->elements[index]);
	status = style_decoration_parts(value, parts);
	if (status != 0)
		goto cleanup;
	status = style_list_remove_at(realm, list, (uint32_t)index);
	if (status != 0)
		goto cleanup;
	for (part = 0U; part < 4U; part++) {
		name = vm_atom_from_ascii(realm->heap, names[part]);
		if (name == NULL) {
			status = ENOMEM;
			goto cleanup;
		}
		name_root = &name->cell;
		status = vm_heap_add_root(realm->heap, &name_root);
		if (status != 0)
			goto cleanup;
		value = vm_string_from_units(realm->heap, parts[part + 1U].data, parts[part + 1U].length);
		if (value == NULL)
			status = ENOMEM;
		else
			status = style_list_append(realm, list, name, value, important);
		vm_heap_remove_root(realm->heap, &name_root);
		if (status != 0)
			goto cleanup;
	}

cleanup:
	for (index = 0U; index < 5U; index++)
		wb_units_release(&parts[index]);
	return status;
}

/*
 * Tells whether a grid shorthand sets a property: one of its longhands, or
 * a smaller grid shorthand all of whose longhands it sets.
 */
static int
style_grid_covers(
	const struct vm_string *shorthand,
	const struct vm_string *name)
{
	const char *const *longhands;
	const char *const *inner;
	size_t index;
	size_t at;
	int found;

	/* The shorthand's longhands. */
	longhands = style_shorthand_longhands(shorthand);
	if (longhands == NULL || vm_string_equal(shorthand, name))
		return 0;

	/* One of them. */
	for (index = 0; longhands[index] != NULL; index++) {
		if (vm_string_equal_ascii(name, longhands[index]))
			return 1;
	}

	/* A shorthand inside it. */
	inner = style_shorthand_longhands(name);
	if (inner == NULL)
		return 0;
	for (index = 0; inner[index] != NULL; index++) {
		found = 0;
		for (at = 0; longhands[at] != NULL; at++) {
			if (strcmp(inner[index], longhands[at]) == 0)
				found = 1;
		}
		if (!found)
			return 0;
	}

	/* Succeeded: every longhand of it is the shorthand's. */
	return 1;
}

/* The longhands of a grid or flex shorthand (NULL for another property). */
static const char *const *
style_shorthand_longhands(
	const struct vm_string *name)
{
	const char *const *longhands;

	/* A grid shorthand, else a flex one. */
	longhands = css_grid_shorthand_longhands(name);
	if (longhands == NULL)
		longhands = css_flex_shorthand_longhands(name);
	return longhands;
}

/* Writes the value a flex or flex-flow declaration gives one of its longhands. */
static int
style_flex_part(
	struct vm_realm *realm,
	const struct vm_string *shorthand,
	struct vm_string *value,
	const char *longhand,
	struct vm_string **part)
{
	const char *const *longhands;
	struct wb_units canonical;
	struct wb_units parts[3];
	size_t index;
	int found;
	int status;

	/* The parts. */
	wb_units_init(&canonical);
	for (index = 0; index < 3U; index++)
		wb_units_init(&parts[index]);
	status = css_flex_shorthand_parts(shorthand, value, &canonical, parts, &found);

	/* The longhand's. */
	longhands = css_flex_shorthand_longhands(shorthand);
	for (index = 0; status == 0 && found && longhands[index] != NULL; index++) {
		if (strcmp(longhands[index], longhand) != 0)
			continue;
		*part = vm_string_from_units(realm->heap, parts[index].data, parts[index].length);
		if (*part == NULL)
			status = ENOMEM;
		break;
	}
	wb_units_release(&canonical);
	for (index = 0; index < 3U; index++)
		wb_units_release(&parts[index]);
	return status;
}

/* Puts a flex or flex-flow value in its canonical form (another property's as it is). */
static int
style_canonical_flex(
	struct vm_realm *realm,
	const struct vm_string *name,
	struct vm_string *value,
	struct vm_string **out)
{
	struct wb_units canonical;
	struct wb_units parts[3];
	size_t index;
	int found;
	int status;

	/* Only the flex shorthands, and not a CSS-wide keyword. */
	*out = value;
	if (css_flex_shorthand_longhands(name) == NULL || style_wide_keyword(value))
		return 0;

	/* The canonical text. */
	wb_units_init(&canonical);
	for (index = 0; index < 3U; index++)
		wb_units_init(&parts[index]);
	status = css_flex_shorthand_parts(name, value, &canonical, parts, &found);
	if (status == 0 && found) {
		*out = vm_string_from_units(realm->heap, canonical.data, canonical.length);
		if (*out == NULL)
			status = ENOMEM;
	}
	wb_units_release(&canonical);
	for (index = 0; index < 3U; index++)
		wb_units_release(&parts[index]);
	return status;
}

/* Tells whether a value is a CSS-wide keyword, which every longhand of a shorthand takes. */
static int
style_wide_keyword(
	const struct vm_string *value)
{
	/* The keywords. */
	return vm_string_equal_ascii(value, "inherit") || vm_string_equal_ascii(value, "initial") ||
	    vm_string_equal_ascii(value, "unset") || vm_string_equal_ascii(value, "revert") ||
	    vm_string_equal_ascii(value, "revert-layer");
}

/* Writes the value a grid shorthand's declaration gives one of its longhands. */
static int
style_grid_part(
	struct vm_realm *realm,
	const struct vm_string *shorthand,
	struct vm_string *value,
	const char *longhand,
	struct vm_string **part)
{
	struct wb_units out;
	int found;
	int status;

	/* A CSS-wide keyword is every longhand's. */
	*part = NULL;
	if (style_wide_keyword(value)) {
		*part = value;
		return 0;
	}

	/* A flex shorthand's part. */
	if (css_flex_shorthand_longhands(shorthand) != NULL)
		return style_flex_part(realm, shorthand, value, longhand, part);

	/* Otherwise the grid shorthand's part. */
	wb_units_init(&out);
	status = css_grid_shorthand_part(realm->heap, shorthand, value, longhand, &out, &found);
	if (status == 0 && found) {
		*part = vm_string_from_units(realm->heap, out.data, out.length);
		if (*part == NULL)
			status = ENOMEM;
	}
	wb_units_release(&out);
	return status;
}

/* Reads a grid longhand from the last grid shorthand that sets it. */
static int
style_grid_get(
	struct vm_realm *realm,
	const struct style_list *list,
	const struct vm_string *name,
	vm_value *result,
	int *present)
{
	struct vm_string *shorthand;
	struct vm_string *part;
	char longhand[STYLE_NAME_MAX];
	size_t index;
	size_t found;
	int status;

	/* The last shorthand that sets the longhand. */
	*present = 0;
	if (name->length >= sizeof(longhand) || name->length < 5U)
		return 0;
	for (index = 0; index < name->length; index++)
		longhand[index] = (char)vm_string_at(name, index);
	longhand[name->length] = '\0';
	if (strncmp(longhand, "grid-", 5U) != 0 && strncmp(longhand, "flex-", 5U) != 0)
		return 0;
	found = list->names->length;
	for (index = 0; index < list->names->length; index++) {
		shorthand = (struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		if (style_grid_covers(shorthand, name))
			found = index;
	}
	if (found == list->names->length)
		return 0;

	/* Its part. */
	shorthand = (struct vm_string *)vm_value_as_cell(list->names->elements[found]);
	status = style_grid_part(realm, shorthand, (struct vm_string *)vm_value_as_cell(list->values->elements[found]),
	    longhand, &part);
	if (status != 0 || part == NULL)
		return status;

	/* Succeeded: the longhand's value. */
	*result = vm_value_cell(part);
	*present = 1;
	return 0;
}

/*
 * Replaces the grid shorthands that set a longhand with their longhands
 * before the longhand changes (one that cannot be split stays as it is).
 */
static int
style_grid_expand(
	struct vm_realm *realm,
	struct style_list *list,
	const struct vm_string *name)
{
	const char *const *longhands;
	struct vm_string *shorthand;
	struct vm_string *value;
	struct vm_string *parts[8];
	struct vm_string *atom;
	struct vm_cell *roots[10];
	size_t index;
	size_t count;
	size_t rooted;
	size_t part;
	int important;
	int status;

	/* A grid longhand (or smaller shorthand) only. */
	if (vm_string_equal_ascii(name, "grid") || name->length < 5U)
		return 0;
	index = 0;
	status = 0;
	while (index < list->names->length && status == 0) {
		shorthand = (struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		if (!style_grid_covers(shorthand, name)) {
			index++;
			continue;
		}

		/* Every part, kept alive while the list changes. */
		longhands = style_shorthand_longhands(shorthand);
		value = (struct vm_string *)vm_value_as_cell(list->values->elements[index]);
		important = vm_to_boolean(list->priorities->elements[index]);
		rooted = 0;
		for (count = 0; longhands[count] != NULL && status == 0; count++) {
			status = style_grid_part(realm, shorthand, value, longhands[count], &parts[count]);
			if (status == 0 && parts[count] == NULL)
				status = EINVAL;
			if (status == 0) {
				roots[rooted] = &parts[count]->cell;
				status = vm_heap_add_root(realm->heap, &roots[rooted]);
				if (status == 0)
					rooted++;
			}
		}

		/* The shorthand goes and its longhands come in its place at the end. */
		if (status == 0)
			status = style_list_remove_at(realm, list, (uint32_t)index);
		for (part = 0; part < count && status == 0; part++) {
			atom = vm_atom_from_ascii(realm->heap, longhands[part]);
			if (atom == NULL) {
				status = ENOMEM;
				break;
			}
			roots[rooted] = &atom->cell;
			status = vm_heap_add_root(realm->heap, &roots[rooted]);
			if (status != 0)
				break;
			status = style_list_append(realm, list, atom, parts[part], important);
			vm_heap_remove_root(realm->heap, &roots[rooted]);
		}
		while (rooted > 0) {
			rooted--;
			vm_heap_remove_root(realm->heap, &roots[rooted]);
		}

		/* A shorthand that cannot be split stays. */
		if (status == EINVAL) {
			status = 0;
			index++;
		}
	}
	return status;
}

/* Removes the declarations a grid shorthand sets (its longhands and smaller shorthands). */
static int
style_grid_remove_covered(
	struct vm_realm *realm,
	struct style_list *list,
	const struct vm_string *name,
	int *removed)
{
	struct vm_string *earlier;
	uint32_t index;
	int status;

	/* Each covered declaration. */
	index = 0;
	while (index < list->names->length) {
		earlier = (struct vm_string *)vm_value_as_cell(list->names->elements[index]);
		if (!style_grid_covers(name, earlier)) {
			index++;
			continue;
		}
		status = style_list_remove_at(realm, list, index);
		if (status != 0)
			return status;
		*removed = 1;
	}

	/* Succeeded: none of them is left. */
	return 0;
}

/* Reads a property name argument: lower case, but a custom property's as it is. */
static int
style_property_name(
	struct vm_realm *realm,
	vm_value value,
	struct vm_string **name)
{
	struct vm_string *text;
	struct wb_units units;
	size_t index;
	int custom;
	int status;

	/* The argument's characters. */
	status = bind_to_string(realm, value, &text);
	if (status != 0)
		return status;
	wb_units_init(&units);
	status = vm_string_append_units(text, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* A custom property's name stays as it is; any other goes to lower case. */
	custom = 0;
	if (units.length >= 2U && units.data[0] == '-' && units.data[1] == '-')
		custom = 1;
	for (index = 0; !custom && index < units.length; index++) {
		if (units.data[index] >= 'A' && units.data[index] <= 'Z')
			units.data[index] = (uint16_t)(units.data[index] + 0x20U);
	}

	/* The name's atom. */
	*name = vm_atom_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (*name == NULL)
		return ENOMEM;

	/* Succeeded: the name. */
	return 0;
}

/* Defines one accessor of CSSStyleDeclaration's prototype for a property, under a name. */
static int
style_accessor(
	struct bind_window *window,
	const char *name,
	struct vm_string *property)
{
	struct vm_realm *realm;
	struct vm_function *getter;
	struct vm_function *setter;
	struct vm_accessor *accessor;
	struct vm_cell *roots[4];
	unsigned registered;
	int status;

	/* The getter and the setter, which keep the property's name. */
	realm = window->realm;
	roots[0] = &property->cell;
	roots[1] = NULL;
	roots[2] = NULL;
	roots[3] = NULL;
	registered = 0;
	while (registered < 4U) {
		status = vm_heap_add_root(realm->heap, &roots[registered]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Both functions retain the same property name for later callbacks. */
	status = js_builtin_function(realm, name, 0, style_member_get, NULL, &getter);
	if (status != 0)
		goto cleanup;
	roots[1] = &getter->object.cell;
	getter->data = vm_value_cell(property);
	status = js_builtin_function(realm, name, 1, style_member_set, NULL, &setter);
	if (status != 0)
		goto cleanup;
	roots[2] = &setter->object.cell;
	setter->data = vm_value_cell(property);

	/* The accessor on the prototype. */
	accessor = vm_accessor_create(realm->heap, vm_value_cell(getter), vm_value_cell(setter));
	if (accessor == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The unpublished accessor survives the prototype definition. */
	roots[3] = &accessor->cell;
	status = js_builtin_value(realm, window->prototypes[BIND_CSS_STYLE_DECLARATION], name, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR | VM_PROPERTY_ENUMERABLE | VM_PROPERTY_CONFIGURABLE);

cleanup:
	/* The prototype owns a successfully published accessor and its functions. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Reports a failed callback or accessor publication. */
	if (status != 0)
		return status;

	/* Succeeded: the accessor is defined. */
	return 0;
}

/*
 * Writes a CSS property name in camel case: each "-" before a letter goes
 * and the letter becomes upper case; a leading "-webkit-" becomes
 * "webkit" or, with capital, "Webkit".
 */
static void
style_camel_case(
	const char *name,
	int capital,
	char out[STYLE_NAME_MAX])
{
	size_t in;
	size_t made;
	char character;

	/* A leading hyphen (of a vendor prefix) goes; the prefix's letter is upper case only with capital. */
	in = 0;
	made = 0;
	if (name[0] == '-') {
		in = 1;
		out[made] = name[in];
		if (capital && name[in] >= 'a' && name[in] <= 'z')
			out[made] = (char)(name[in] - 0x20);
		made++;
		in++;
	}

	/* Each other character, a hyphen making the next one upper case. */
	while (name[in] != '\0' && made + 1U < STYLE_NAME_MAX) {
		character = name[in];
		if (character == '-' && name[in + 1U] != '\0') {
			in++;
			character = name[in];
			if (character >= 'a' && character <= 'z')
				character = (char)(character - 0x20);
		}

		/* The character. */
		out[made] = character;
		made++;
		in++;
	}

	/* The name ends. */
	out[made] = '\0';
}

/* Tells whether a character is CSS whitespace. */
static int
style_is_space(
	uint16_t unit)
{
	/* Space, tab, line feed, form feed, carriage return. */
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

/* Reports the declarations as other browsers serialize them (cssText). */
static int
style_css_text_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	struct vm_string *text;
	struct wb_units units;
	int computed;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A computed declaration's text is empty, as in other browsers. */
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		status = bind_string(realm, "", result);
		if (status != 0)
			return status;

		/* Succeeded: the requested declaration view is available. */
		return 0;
	}

	/* The element's declarations. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0)
		return status;

	/* Their text. */
	wb_units_init(&units);
	status = style_serialize(realm, &list, 1, &units);
	style_list_release(realm, &list);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* As a string. */
	text = vm_string_from_units(realm->heap, units.data, units.length);
	wb_units_release(&units);
	if (text == NULL)
		return ENOMEM;

	/* Succeeded: the text. */
	*result = vm_value_cell(text);
	return 0;
}

/* Replaces the declarations with the ones of a text the engine reads (cssText). */
static int
style_css_text_set(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list given;
	struct style_list kept;
	struct vm_string *text;
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *text_root;
	struct wb_units units;
	uint32_t index;
	int given_live;
	int kept_live;
	int text_live;
	int important;
	int valid;
	int status;

	/* The element and the text's declarations. */
	*result = VM_VALUE_UNDEFINED;
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &text);
	if (status != 0)
		return status;
	text_root = &text->cell;
	status = vm_heap_add_root(realm->heap, &text_root);
	if (status != 0)
		return status;
	text_live = 1;
	given_live = 0;
	kept_live = 0;
	status = style_list_create(realm, &given);
	if (status != 0)
		goto cleanup;
	given_live = 1;
	wb_units_init(&units);
	status = vm_string_append_units(text, &units);
	vm_heap_remove_root(realm->heap, &text_root);
	text_live = 0;
	if (status == 0)
		status = style_parse(realm, units.data, units.length, &given);
	wb_units_release(&units);
	if (status != 0)
		goto cleanup;

	/* The ones the engine reads, each property once (the last one given). */
	status = style_list_create(realm, &kept);
	if (status != 0)
		goto cleanup;
	kept_live = 1;
	for (index = 0; index < given.names->length; index++) {
		name = (struct vm_string *)vm_value_as_cell(given.names->elements[index]);
		value = (struct vm_string *)vm_value_as_cell(given.values->elements[index]);
		important = vm_to_boolean(given.priorities->elements[index]);
		status = style_valid(realm, name, value, &valid);
		if (status != 0)
			goto cleanup;
		if (!valid)
			continue;

		/* A property given again replaces the earlier declaration. */
		status = style_remove_from_list(realm, &kept, name);
		if (status != 0)
			goto cleanup;
		status = style_list_append(realm, &kept, name, value, important);
		if (status != 0)
			goto cleanup;
	}

	/* Succeeded: the attribute holds them. */
	status = style_write(realm, element, &kept);

cleanup:
	/* Each temporary list and converted input leaves the root set. */
	if (kept_live)
		style_list_release(realm, &kept);
	if (given_live)
		style_list_release(realm, &given);
	if (text_live)
		vm_heap_remove_root(realm->heap, &text_root);
	if (status != 0)
		return status;
	return 0;
}

/* Reports how many declarations there are (length). */
static int
style_length(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	int computed;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A computed declaration lists the properties it reports. */
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		*result = vm_value_int32((int32_t)bind_computed_count());
		return 0;
	}

	/* The element's declarations. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0)
		return status;

	/* Succeeded: how many. */
	*result = vm_value_int32((int32_t)list.names->length);
	style_list_release(realm, &list);
	return 0;
}

/* Reports the name of the declaration at a place, or the empty string past the end (item). */
static int
style_item(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	const char *name;
	uint32_t index;
	int computed;
	int status;

	/* A computed declaration names its properties in its order. */
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		status = vm_to_uint32(realm, js_argument(args, count, 0), &index);
		if (status != 0)
			return status;
		name = bind_computed_name(index);
		if (name == NULL)
			name = "";
		status = bind_string(realm, name, result);
		return status;
	}

	/* The element's declarations and the place. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, js_argument(args, count, 0), &index);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0)
		return status;

	/* A place past the end has no name. */
	if (index >= list.names->length) {
		status = bind_string(realm, "", result);
		style_list_release(realm, &list);
		return status;
	}

	/* Succeeded: the name. */
	*result = list.names->elements[index];
	style_list_release(realm, &list);
	return 0;
}

/* Reports a property's value, or the empty string (getPropertyValue). */
static int
style_get_property_value(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *name_root;
	uint32_t index;
	int present;
	int computed;
	int status;

	/* A computed declaration's value is the element's resolved one. */
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		status = style_property_name(realm, js_argument(args, count, 0), &name);
		if (status != 0)
			return status;
		name_root = &name->cell;
		status = vm_heap_add_root(realm->heap, &name_root);
		if (status != 0)
			return status;
		status = bind_computed_value(realm, element, name, result);
		vm_heap_remove_root(realm->heap, &name_root);
		if (status != 0)
			return status;

		/* Succeeded: the requested declaration view is available. */
		return 0;
	}

	/* The element, the name and the declarations. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_property_name(realm, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;
	name_root = &name->cell;
	status = vm_heap_add_root(realm->heap, &name_root);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &name_root);
		return status;
	}

	/* A property not there is the empty string. */
	present = style_find(&list, name, &index);
	if (!present) {
		status = style_decoration_get(realm, &list, name, result, &present);
		if (status == 0 && !present)
			status = style_grid_get(realm, &list, name, result, &present);
		if (status != 0) {
			style_list_release(realm, &list);
			vm_heap_remove_root(realm->heap, &name_root);
			return status;
		}

		/* A synthesized shorthand already supplied the public value. */
		if (present) {
			style_list_release(realm, &list);
			vm_heap_remove_root(realm->heap, &name_root);
			return 0;
		}
	}

	/* Name classification is complete before an absent-value result is allocated. */
	vm_heap_remove_root(realm->heap, &name_root);
	/* An absent declaration yields the ordinary empty string. */
	if (!present) {
		status = bind_string(realm, "", result);
		style_list_release(realm, &list);
		if (status != 0)
			return status;

		/* Succeeded: the requested declaration view is available. */
		return 0;
	}

	/* A canonical number view does not round the declaration used by later layout or CSSOM writes. */
	value = (struct vm_string *)vm_value_as_cell(list.values->elements[index]);
	status = style_canonical_factor(realm, name, value, &value);
	if (status != 0) {
		style_list_release(realm, &list);
		return status;
	}

	/* Publish the selected string before releasing its transient declaration list. */
	*result = vm_value_cell(value);
	style_list_release(realm, &list);

	/* Succeeded: the native declaration's public value is available. */
	return 0;
}

/* Reports "important" for a property declared !important, or the empty string (getPropertyPriority). */
static int
style_get_property_priority(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	struct vm_string *name;
	struct vm_cell *name_root;
	uint32_t index;
	int present;
	int important;
	int computed;
	int status;

	/* A computed declaration has no priorities. */
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		status = bind_string(realm, "", result);
		return status;
	}

	/* The element, the name and the declarations. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_property_name(realm, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;
	name_root = &name->cell;
	status = vm_heap_add_root(realm->heap, &name_root);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0) {
		vm_heap_remove_root(realm->heap, &name_root);
		return status;
	}

	/* Whether the property is there and important. */
	important = 0;
	present = style_find(&list, name, &index);
	vm_heap_remove_root(realm->heap, &name_root);
	if (present)
		important = vm_to_boolean(list.priorities->elements[index]);

	/* The priority's word. */
	if (important) {
		status = bind_string(realm, "important", result);
	} else {
		status = bind_string(realm, "", result);
	}

	/* Temporary declarations are no longer needed after the word is allocated. */
	style_list_release(realm, &list);

	/* A string that could not be made. */
	if (status != 0)
		return status;

	/* Succeeded: the word. */
	return 0;
}

/*
 * Sets a property to a value with an optional "important" priority; an
 * empty value removes it (setProperty).
 */
static int
style_set_property(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	struct vm_string *value;
	struct vm_string *priority;
	struct vm_cell *roots[3];
	vm_value old_value;
	unsigned registered;
	int important;
	int status;

	/* The element, the name, the value and the priority. */
	*result = VM_VALUE_UNDEFINED;
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	while (registered < 3U) {
		status = vm_heap_add_root(realm->heap, &roots[registered]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Converts each argument while retaining earlier converted strings. */
	status = style_property_name(realm, js_argument(args, count, 0), &name);
	if (status != 0)
		goto cleanup;
	roots[0] = &name->cell;
	status = bind_to_string(realm, js_argument(args, count, 1), &value);
	if (status != 0)
		goto cleanup;
	roots[1] = &value->cell;
	priority = NULL;
	if (count > 2U && args[2] != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, args[2], &priority);
		if (status != 0)
			goto cleanup;
		roots[2] = &priority->cell;
	}

	/* An empty value removes the property. */
	if (value->length == 0) {
		status = style_remove(realm, element, name, &old_value);
		goto cleanup;
	}

	/* The priority must be "important" or nothing. */
	important = 0;
	if (priority != NULL && priority->length != 0) {
		important = vm_string_equal_ascii(priority, "important");
		if (!important)
			goto cleanup;
	}

	/* Succeeded: the property is set. */
	status = style_set(realm, element, name, value, important);

cleanup:
	/* Converted arguments leave the root set after the mutation returns. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Reports a failed conversion or mutation. */
	if (status != 0)
		return status;
	return 0;
}

/* Removes a property and reports the value it had (removeProperty). */
static int
style_remove_property(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	int status;

	/* The element and the name. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_property_name(realm, js_argument(args, count, 0), &name);
	if (status != 0)
		return status;

	/* Succeeded: the property is gone. */
	status = style_remove(realm, element, name, result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports the value of the property an accessor stands for, or the empty string. */
static int
style_member_get(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct style_list list;
	struct vm_function *callee;
	struct vm_string *name;
	struct vm_string *value;
	uint32_t index;
	int present;
	int computed;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The property; a computed declaration's value is the element's resolved one. */
	callee = js_builtin_callee(realm);
	name = (struct vm_string *)vm_value_as_cell(callee->data);
	computed = bind_computed_element(this_value, &element);
	if (computed) {
		status = bind_computed_value(realm, element, name, result);
		if (status != 0)
			return status;

		/* Succeeded: the requested declaration view is available. */
		return 0;
	}

	/* The element and the declarations. */
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	status = style_read(realm, element, &list);
	if (status != 0)
		return status;

	/* A property not there is the empty string. */
	present = style_find(&list, name, &index);
	if (!present) {
		status = style_decoration_get(realm, &list, name, result, &present);
		if (status == 0 && !present)
			status = style_grid_get(realm, &list, name, result, &present);
		if (status != 0) {
			style_list_release(realm, &list);
			return status;
		}

		/* A synthesized shorthand already supplied the public value. */
		if (present) {
			style_list_release(realm, &list);
			return 0;
		}
	}

	/* An absent declaration yields the ordinary empty string. */
	if (!present) {
		status = bind_string(realm, "", result);
		style_list_release(realm, &list);
		if (status != 0)
			return status;

		/* Succeeded: the requested declaration view is available. */
		return 0;
	}

	/* A canonical number view does not round the declaration used by later layout or CSSOM writes. */
	value = (struct vm_string *)vm_value_as_cell(list.values->elements[index]);
	status = style_canonical_factor(realm, name, value, &value);
	if (status != 0) {
		style_list_release(realm, &list);
		return status;
	}

	/* Publish the selected string before releasing its transient declaration list. */
	*result = vm_value_cell(value);
	style_list_release(realm, &list);

	/* Succeeded: the native declaration's public value is available. */
	return 0;
}

/* Sets the property an accessor stands for; an empty value removes it. */
static int
style_member_set(
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
	vm_value old_value;
	vm_value given;
	int status;

	/* The element and the property. */
	*result = VM_VALUE_UNDEFINED;
	status = style_element(realm, this_value, &element);
	if (status != 0)
		return status;
	callee = js_builtin_callee(realm);
	name = (struct vm_string *)vm_value_as_cell(callee->data);

	/* Null removes the property, as the empty string does. */
	given = js_argument(args, count, 0);
	if (given == VM_VALUE_NULL) {
		status = style_remove(realm, element, name, &old_value);
		return status;
	}

	/* The value. */
	status = bind_to_string(realm, given, &value);
	if (status != 0)
		return status;

	/* An empty value removes the property. */
	if (value->length == 0) {
		status = style_remove(realm, element, name, &old_value);
		return status;
	}

	/* Succeeded: the property is set, without a priority. */
	status = style_set(realm, element, name, value, 0);
	if (status != 0)
		return status;
	return 0;
}
