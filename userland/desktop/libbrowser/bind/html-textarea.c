/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HTMLTextAreaElement (ws177-p019): its value, the textarea's own text
 * once it was typed or set (the control's dirty value), else its default
 * text (its children's text), as the form control keeps it for painting
 * and submission.  Setting it makes the value the control's own and ends
 * what an input method was composing.  defaultValue is its children's
 * text.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

static int textarea_value_get(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int textarea_value_set(struct vm_realm *realm, vm_value receiver, const vm_value *args, unsigned count, vm_value *result);
static int textarea_this(struct vm_realm *realm, vm_value receiver, struct dom_element **out);

/* The textarea's attributes on its prototype.  Constant for the program's life. */
static const struct bind_attribute textarea_attributes[] = {
	{ "value", textarea_value_get, textarea_value_set },
	{ NULL, NULL, NULL }
};

/* Only actual textarea nodes receive this prototype; native construction remains illegal. */
const struct bind_interface bind_html_text_area_element_interface = {
	"HTMLTextAreaElement", BIND_HTML_ELEMENT, 0, NULL, textarea_attributes, NULL, NULL
};

/* Reads the textarea's value: its own text, or its default text. */
static int
textarea_value_get(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct wb_units units;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* Only an actual textarea. */
	status = textarea_this(realm, receiver, &element);
	if (status != 0)
		return status;

	/* The value the control paints and submits. */
	wb_units_init(&units);
	status = dom_control_value(element, &units);
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* As a string. */
	status = bind_units(realm, units.data, units.length, result);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the value is the script's. */
	return 0;
}

/* Sets the textarea's value, which becomes its own (null is the empty text). */
static int
textarea_value_set(
	struct vm_realm *realm,
	vm_value receiver,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct dom_element *element;
	struct vm_string *text;
	struct wb_units units;
	vm_value argument;
	size_t index;
	uint16_t unit;
	int status;

	/* Only an actual textarea, before a conversion that may run script. */
	*result = VM_VALUE_UNDEFINED;
	status = textarea_this(realm, receiver, &element);
	if (status != 0)
		return status;
	argument = js_argument(args, count, 0);

	/* The text: null is the empty one. */
	if (argument == VM_VALUE_NULL) {
		text = vm_atom_from_ascii(realm->heap, "");
		if (text == NULL)
			return ENOMEM;
	} else {
		status = bind_to_string(realm, argument, &text);
		if (status != 0)
			return status;
	}

	/* Its units (a textarea keeps its line breaks). */
	wb_units_init(&units);
	for (index = 0; index < text->length; index++) {
		unit = vm_string_at(text, index);
		status = wb_units_append(&units, &unit, 1);
		if (status != 0) {
			wb_units_release(&units);
			return status;
		}
	}

	/* The control's own value from now on. */
	status = dom_input_set_value(element, units.data, units.length);
	wb_units_release(&units);
	if (status != 0)
		return status;

	/* Succeeded: the value is set. */
	return 0;
}

/* Takes the receiver when it is an actual HTML textarea (exact local name), else throws. */
static int
textarea_this(
	struct vm_realm *realm,
	vm_value receiver,
	struct dom_element **out)
{
	struct dom_node *node;
	struct dom_element *element;
	int same;
	int status;

	/* An element. */
	status = bind_this_node(realm, receiver, &node);
	if (status != 0)
		return status;
	if (node->type != DOM_ELEMENT) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* An HTML textarea by its exact local name. */
	element = (struct dom_element *)node;
	same = vm_string_equal_ascii(element->local_name, "textarea");
	if (element->ns != DOM_NS_HTML || !same) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the textarea. */
	*out = element;
	return 0;
}
