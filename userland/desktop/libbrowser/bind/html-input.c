/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Input reflection separates default attributes from the control's owned text value. */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

/* Current value modes distinguish owned text, reflected defaults and empty file state. */
enum input_value_mode {
	INPUT_VALUE,
	INPUT_DEFAULT,
	INPUT_DEFAULT_ON,
	INPUT_FILENAME
};

static int input_name_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_name_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_type_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_type_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_default_value_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_default_value_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_value_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_value_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_form_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

static int input_checked_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_checked_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

static int input_disabled_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_disabled_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_indeterminate_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int input_indeterminate_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);

/* Canonical type keywords are constant and preserve exact invalid/missing text fallback. */
static const char *const input_types[] = {
	"hidden", "text", "search", "tel", "url", "email", "password",
	"date", "month", "week", "time", "datetime-local", "number", "range",
	"color", "checkbox", "radio", "file", "submit", "image", "reset", "button"
};

/* Attributes are installed on the native prototype, keeping content and dirty state distinct. */
static const struct bind_attribute input_attributes[] = {
	{ "name", input_name_get, input_name_set },
	{ "type", input_type_get, input_type_set },
	{ "defaultValue", input_default_value_get, input_default_value_set },
	{ "value", input_value_get, input_value_set },
	{ "form", input_form_get, NULL },
	{ "checked", input_checked_get, input_checked_set },
	{ "disabled", input_disabled_get, input_disabled_set },
	{ "indeterminate", input_indeterminate_get, input_indeterminate_set },
	{ NULL, NULL, NULL }
};

/* Only actual input nodes receive this prototype; native construction remains illegal. */
const struct bind_interface bind_html_input_element_interface = {
	"HTMLInputElement", BIND_HTML_ELEMENT, 0, NULL, input_attributes, NULL, NULL
};

static int input_this(struct vm_realm *realm, vm_value receiver, struct dom_element **out);
static int input_attribute_get(struct vm_realm *realm, vm_value receiver, const char *name, vm_value *result);
static int input_attribute_set(struct vm_realm *realm, vm_value receiver, const char *name, const vm_value *args, unsigned count, vm_value *result);
static const char *input_type(const struct dom_element *element);
static enum input_value_mode input_mode(const char *type);
static void input_strip_lines(struct wb_units *units, const char *type);

/* Reads the input's name content attribute through its native brand. */
static int
input_name_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common reflection path preserves DOM ownership and conversion failures. */
	status = input_attribute_get(realm, receiver, "name", result);
	if (status != 0)
		return status;

	/* Succeeded: the actual input's name attribute is observed. */
	return 0;
}

/* Reflects the input's name content attribute through its native brand. */
static int
input_name_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The common reflection path preserves DOM ownership and conversion failures. */
	status = input_attribute_set(realm, receiver, "name", args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual input's name attribute is updated. */
	return 0;
}

/* Reflects the input's type content attribute through its native brand. */
static int
input_type_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The common reflection path preserves DOM ownership and conversion failures. */
	status = input_attribute_set(realm, receiver, "type", args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual input's type attribute is updated. */
	return 0;
}

/* Reads the input's value content attribute through its native brand. */
static int
input_default_value_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The common reflection path preserves DOM ownership and conversion failures. */
	status = input_attribute_get(realm, receiver, "value", result);
	if (status != 0)
		return status;

	/* Succeeded: the actual input's value attribute is observed. */
	return 0;
}

/* Reflects the input's value content attribute through its native brand. */
static int
input_default_value_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	/* The common reflection path preserves DOM ownership and conversion failures. */
	status = input_attribute_set(realm, receiver, "value", args, count, result);
	if (status != 0)
		return status;

	/* Succeeded: the actual input's value attribute is updated. */
	return 0;
}

/* Reports the canonical enumerated type without changing its raw content attribute. */
static int
input_type_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	const char *type;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A borrowed getter requires actual input namespace and local case. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	type = input_type(element);
	status = bind_string(realm, type, result);
	if (status != 0)
		return status;

	/* Succeeded: the raw attribute retains its original spelling independently. */
	return 0;
}

/* Reads a selected value mode, sanitizing simple text without changing its default attribute. */
static int
input_value_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	struct wb_units units;
	const char *type;
	enum input_value_mode mode;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Branding precedes every native-state observation. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	type = input_type(element);
	mode = input_mode(type);

	/* No selected-file service exists; script cannot synthesize a filename. */
	if (mode == INPUT_FILENAME) {
		status = bind_string(realm, "", result);
		if (status != 0)
			return status;
		return 0;
	}

	/* Default and default/on modes always follow the current no-namespace attribute. */
	if (mode != INPUT_VALUE) {
		attribute = dom_attribute_ascii(element, "value");
		if (attribute != NULL) {
			*result = vm_value_cell(attribute);
			return 0;
		}

		/* An absent radio or checkbox value uses the historical on keyword. */
		if (mode == INPUT_DEFAULT_ON) {
			status = bind_string(realm, "on", result);
		} else {
			status = bind_string(realm, "", result);
		}

		/* Refuse to publish a failed string allocation as a valid default value. */
		if (status != 0)
			return status;
		return 0;
	}

	/* Existing dirty storage is shared with painting and form submission. */
	wb_units_init(&units);
	status = dom_control_value(element, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Clean default reads use the same simple-text newline rule as owned values. */
	input_strip_lines(&units, type);
	status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: a clean default or dirty owned text value is exposed as UTF-16. */
	return 0;
}

/* Converts once before selecting the current mode and updating owned or reflected value. */
static int
input_value_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *text;
	struct vm_string *name;
	struct vm_cell *roots[2];
	struct wb_units units;
	vm_value argument;
	const char *type;
	enum input_value_mode mode;
	size_t index;
	unsigned registered;
	uint16_t unit;
	int status;

	/* Native receiver identity is established before a potentially mutating conversion. */
	*result = VM_VALUE_UNDEFINED;
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	argument = js_argument(args, count, 0);

	/* The legacy null-to-empty conversion applies to value only. */
	if (argument == VM_VALUE_NULL) {
		text = vm_atom_from_ascii(realm->heap, "");
		if (text == NULL)
			return ENOMEM;
	} else {
		status = bind_to_string(realm, argument, &text);
		if (status != 0)
			return status;
	}

	/* User conversion may change type, so select the mode only afterward. */
	type = input_type(element);
	mode = input_mode(type);
	if (mode == INPUT_FILENAME) {
		if (text->length != 0) {
			status = bind_throw_dom(realm, "InvalidStateError", "File input value must be empty.");
			return status;
		}

		/* Empty file value has no selected files or owned text to expose. */
		return 0;
	}

	/* Reflected value modes update the actual attribute without setting a text dirty flag. */
	if (mode != INPUT_VALUE) {
		/* Text is not yet owned by the DOM while the attribute atom allocates. */
		roots[0] = &text->cell;
		roots[1] = NULL;
		registered = 0;
		for (index = 0; index < 2U; index++) {
			status = vm_heap_add_root(realm->heap, &roots[index]);
			if (status != 0)
				goto cleanup;
			registered++;
		}

		/* The freshly allocated attribute atom also needs a registered slot. */
		name = vm_atom_from_ascii(realm->heap, "value");
		if (name == NULL) {
			status = ENOMEM;
			goto cleanup;
		}

		/* The actual DOM mutation takes ownership before temporary roots leave. */
		roots[1] = &name->cell;
		status = dom_element_set_attribute(element, name, text);

cleanup:
		while (registered != 0) {
			registered--;
			vm_heap_remove_root(realm->heap, &roots[registered]);
		}

		/* Failed publication leaves the previous reflected value intact. */
		if (status != 0)
			return status;
		return 0;
	}

	/* Preserve every UTF-16 unit except the selected simple-text newline sanitization. */
	wb_units_init(&units);
	for (index = 0; index < text->length; index++) {
		unit = vm_string_at(text, index);
		status = wb_units_append(&units, &unit, 1);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* Prepared owned storage avoids modifying the old value on an allocation failure. */
	input_strip_lines(&units, type);
	status = dom_input_set_value(element, units.data, units.length);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: owned value and dirty state changed without creating a default attribute. */
	return 0;
}

/* Reports the current ordinary-tree form owner in the input's actual relevant realm. */
static int
input_form_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_element *owner;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Form ownership never comes from a script property or prototype lookalike. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	owner = dom_form_owner(element);
	if (owner == NULL) {
		*result = VM_VALUE_NULL;
		return 0;
	}

	/* Actual ownership selects the correct wrapper even through another realm's getter. */
	status = bind_wrap(NULL, &owner->node, result);
	if (status != 0)
		return status;

	/* Succeeded: the value is this input's current ordinary form owner. */
	return 0;
}

/* Rejects foreign nodes and uppercase XML input identities before accessing state. */
static int
input_this(
	struct vm_realm *realm,
	vm_value receiver,
	struct dom_element **out)
{
	struct dom_node *node;
	struct dom_element *element;
	int same;
	int status;

	/* Only an actual DOM Element carries the required local name and namespace. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Exact local case matters in XML even though internal tag enums fold names. */
	element = (struct dom_element *)node;
	same = vm_string_equal_ascii(element->local_name, "input");
	if (element->ns != DOM_NS_HTML || !same) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the actual input's native storage is accessible to this operation. */
	*out = element;
	return 0;
}

/* Observes one raw content attribute with absence represented by the empty string. */
static int
input_attribute_get(
	struct vm_realm *realm,
	vm_value receiver,
	const char *name,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *text;
	int status;

	/* The common accessor does not trust a receiver's prototype chain. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	text = dom_attribute_ascii(element, name);
	if (text != NULL) {
		*result = vm_value_cell(text);
		return 0;
	}

	/* Absence does not create a new attribute or dirty value state. */
	status = bind_string(realm, "", result);
	if (status != 0)
		return status;

	/* Succeeded: raw default and reflected properties remain distinct from owned text. */
	return 0;
}

/* Uses the ordinary DOM mutation path after one branded DOMString conversion. */
static int
input_attribute_set(
	struct vm_realm *realm,
	vm_value receiver,
	const char *name,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	struct vm_string *text;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int status;

	/* A receiver error must precede every user-provided argument side effect. */
	*result = VM_VALUE_UNDEFINED;
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &text);
	if (status != 0)
		return status;

	/* Converted text remains live while creating and publishing the attribute. */
	roots[0] = &text->cell;
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* The constant name allocates only after the converted value has a root. */
	attribute = vm_atom_from_ascii(realm->heap, name);
	if (attribute == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The second root retains the name through ordinary DOM mutation. */
	roots[1] = &attribute->cell;

	/* Generation invalidation and live collection names follow the real attribute. */
	status = dom_element_set_attribute(element, attribute, text);

cleanup:
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Failed mutation cannot be reported as a successful reflection. */
	if (status != 0)
		return status;

	/* Succeeded: reflected views and actual HTML attributes now agree. */
	return 0;
}

/* Finds the exact ASCII-insensitive type keyword, preserving text fallback for invalid input. */
static const char *
input_type(
	const struct dom_element *element)
{
	struct vm_string *text;
	size_t index;
	size_t offset;
	size_t length;
	uint16_t unit;
	int same;

	/* Missing type uses the Text state without inserting a content attribute. */
	text = dom_attribute_ascii(element, "type");
	if (text == NULL)
		return "text";

	/* Compare all canonical keywords without trimming whitespace or folding Unicode. */
	for (index = 0; index < sizeof(input_types) / sizeof(input_types[0]); index++) {
		length = strlen(input_types[index]);
		if (length != text->length)
			continue;
		same = 1;
		for (offset = 0; offset < length; offset++) {
			unit = vm_string_at(text, offset);
			if (unit >= 'A' && unit <= 'Z')
				unit += 'a' - 'A';
			if (unit != (uint16_t)(unsigned char)input_types[index][offset]) {
				same = 0;
				break;
			}
		}

		/* The original spelling remains untouched in DOM attribute storage. */
		if (same)
			return input_types[index];
	}

	/* Invalid values select the canonical Text state. */
	return "text";
}

/* Classifies current IDL value behavior independently of widget drawing kinds. */
static enum input_value_mode
input_mode(
	const char *type)
{
	int same;

	/* Checkbox and radio reflect content values with an absent on default. */
	same = strcmp(type, "checkbox");
	if (same == 0)
		return INPUT_DEFAULT_ON;
	same = strcmp(type, "radio");
	if (same == 0)
		return INPUT_DEFAULT_ON;

	/* File names cannot be created by a script text setter. */
	same = strcmp(type, "file");
	if (same == 0)
		return INPUT_FILENAME;

	/* These ordinary button and hidden modes reflect the value attribute. */
	same = strcmp(type, "hidden");
	if (same == 0)
		return INPUT_DEFAULT;
	same = strcmp(type, "submit");
	if (same == 0)
		return INPUT_DEFAULT;
	same = strcmp(type, "image");
	if (same == 0)
		return INPUT_DEFAULT;
	same = strcmp(type, "reset");
	if (same == 0)
		return INPUT_DEFAULT;
	same = strcmp(type, "button");
	if (same == 0)
		return INPUT_DEFAULT;

	/* Succeeded: other types retain owned value-mode storage in this increment. */
	return INPUT_VALUE;
}

/* Applies the selected text/search/tel/password newline algorithm without changing other types. */
static void
input_strip_lines(
	struct wb_units *units,
	const char *type)
{
	size_t index;
	size_t destination;
	uint16_t unit;
	int text;
	int same;

	/* Only the four simple-text states share this exact CR/LF stripping algorithm. */
	text = 0;
	same = strcmp(type, "text");
	if (same == 0)
		text = 1;
	same = strcmp(type, "search");
	if (same == 0)
		text = 1;
	same = strcmp(type, "tel");
	if (same == 0)
		text = 1;
	same = strcmp(type, "password");
	if (same == 0)
		text = 1;
	if (!text)
		return;

	/* Compact in place, preserving every other UTF-16 unit and its relative order. */
	destination = 0;
	for (index = 0; index < units->length; index++) {
		unit = units->data[index];
		if (unit == '\r' || unit == '\n')
			continue;
		units->data[destination++] = unit;
	}

	/* Succeeded: the logical length exposes only the sanitized simple-text value. */
	units->length = destination;
	return;
}

/* Observes current input checkedness independently of the checked content attribute. */
static int
input_checked_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int checked;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only an actual native input can expose this current state. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	checked = dom_control_checked(element);

	/* Succeeded: current checkedness is boolean even when content attributes differ. */
	*result = vm_value_boolean(checked);
	return 0;
}

/* Establishes dirty input checkedness and applies current ordinary radio exclusivity. */
static int
input_checked_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int checked;
	int status;

	/* Boolean conversion never calls a user valueOf or toString method. */
	*result = VM_VALUE_UNDEFINED;
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	checked = vm_to_boolean(js_argument(args, count, 0));
	status = dom_input_set_checked(element, checked, 1);
	if (status != 0)
		return status;

	/* Succeeded: this setter changes current state without reflecting a checked attribute. */
	return 0;
}

/* Reflects disabled attribute presence, independently of disabled ancestor fieldsets. */
static int
input_disabled_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *attribute;
	int disabled;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Native brand rejection precedes all attribute observations. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	attribute = dom_attribute_ascii(element, "disabled");
	disabled = 0;
	if (attribute != NULL)
		disabled = 1;

	/* Attribute presence is the reflected boolean; inherited disabling is separate. */
	*result = vm_value_boolean(disabled);
	return 0;
}

/* Reflects boolean disabled without invoking any user conversion methods. */
static int
input_disabled_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *name;
	struct vm_string *value;
	struct vm_cell *roots[2];
	unsigned index;
	unsigned registered;
	int disabled;
	int status;

	/* Brand before conversion and fully allocate before changing an attribute. */
	*result = VM_VALUE_UNDEFINED;
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	disabled = vm_to_boolean(js_argument(args, count, 0));
	name = vm_atom_from_ascii(realm->heap, "disabled");
	if (name == NULL)
		return ENOMEM;

	/* The attribute atom must survive both native mutation paths. */
	roots[0] = &name->cell;
	roots[1] = NULL;
	registered = 0;
	for (index = 0; index < 2U; index++) {
		status = vm_heap_add_root(realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* False removes the attribute through ordinary DOM mutation. */
	if (!disabled) {
		status = dom_element_remove_attribute(element, name);
		goto cleanup;
	}

	/* True uses the canonical empty content value, preserving ordinary generation hooks. */
	value = vm_atom_from_ascii(realm->heap, "");
	if (value == NULL) {
		status = ENOMEM;
		goto cleanup;
	}

	/* The empty attribute value remains live until the actual DOM owns it. */
	roots[1] = &value->cell;
	status = dom_element_set_attribute(element, name, value);

cleanup:
	while (registered != 0) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Return the native mutation error after removing acquired roots. */
	if (status != 0)
		return status;

	/* Succeeded: the native reflected boolean is now present. */
	return 0;
}

/* Reads independent indeterminateness for every actual HTML input. */
static int
input_indeterminate_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	int indeterminate;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* A clean input's initially false state needs no native storage. */
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	indeterminate = 0;
	if (element->control != NULL)
		indeterminate = element->control->indeterminate;

	/* Indeterminateness is independent of both checkedness and content attributes. */
	*result = vm_value_boolean(indeterminate);
	return 0;
}

/* Updates owned indeterminateness with WebIDL boolean conversion and native branding. */
static int
input_indeterminate_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct dom_control *control;
	int indeterminate;
	int status;

	/* Reject a foreign receiver before evaluating its proposed boolean value. */
	*result = VM_VALUE_UNDEFINED;
	status = input_this(realm, receiver, &element);
	if (status != 0)
		return status;
	indeterminate = vm_to_boolean(js_argument(args, count, 0));
	control = dom_control_of(element);
	if (control == NULL)
		return ENOMEM;

	/* Cloning and click preactivation share this independent owned state. */
	control->indeterminate = indeterminate;
	dom_note_change(&element->node, DOM_CHANGE_STATE, NULL, NULL);
	return 0;
}
