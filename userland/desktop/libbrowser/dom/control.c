/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Form controls (ws074-p032): which kind of control an element is, and the
 * state the user changes -- a text control's value and caret, a checkbox's
 * checkedness -- kept beside the element until the element dies.
 *
 * A control follows its content attributes (value, checked, a textarea's
 * text) until the user or the page changes it; from then on its own state
 * is what it shows and what a form submits, as the HTML Standard's dirty
 * value flag and dirty checkedness flag say.
 */

#include "dom/dom.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * One type attribute value and the kind of input it makes.  The types this
 * pass does not draw on their own (email, search, url, number and the
 * rest) are text fields, as a browser shows them without their widgets.
 */
struct control_type {
	const char *name;
	int kind;
};

/*
 * The input types by their names in lower case.  The table is constant for
 * the life of the program and ends with a NULL name.
 */
static const struct control_type control_types[] = {
    {"text", DOM_CONTROL_TEXT},
    {"search", DOM_CONTROL_TEXT},
    {"email", DOM_CONTROL_TEXT},
    {"url", DOM_CONTROL_TEXT},
    {"tel", DOM_CONTROL_TEXT},
    {"number", DOM_CONTROL_TEXT},
    {"password", DOM_CONTROL_PASSWORD},
    {"button", DOM_CONTROL_BUTTON},
    {"submit", DOM_CONTROL_SUBMIT},
    {"image", DOM_CONTROL_SUBMIT},
    {"reset", DOM_CONTROL_RESET},
    {"checkbox", DOM_CONTROL_CHECKBOX},
    {"radio", DOM_CONTROL_RADIO},
    {"hidden", DOM_CONTROL_HIDDEN},
    {NULL, DOM_CONTROL_NONE}};

static int control_equal_folded(const struct vm_string *string, const char *ascii);
static int control_text_of(const struct dom_node *node, struct wb_units *out);

/*
 * Classifies an element's current native form-control kind.
 *
 * Missing or unknown input types are text fields; buttons submit by default.
 * Textarea and select retain their own kinds; other elements have no control.
 */
int
dom_control_kind(
	const struct dom_element *element)
{
	struct vm_string *type;
	size_t index;
	int same;

	/* Only HTML elements are form controls. */
	if (element->ns != DOM_NS_HTML)
		return DOM_CONTROL_NONE;

	/* A textarea and a select are always one. */
	if (element->tag == DOM_TAG_TEXTAREA)
		return DOM_CONTROL_TEXTAREA;

	/* A select owns its option state independently of any type attribute. */
	if (element->tag == DOM_TAG_SELECT)
		return DOM_CONTROL_SELECT;

	/* A <button> submits unless its type makes it a plain or a reset button. */
	type = dom_attribute_ascii(element, "type");
	if (element->tag == DOM_TAG_BUTTON) {
		/* Only an actual type attribute can override the button's submit default. */
		if (type != NULL) {
			/* The plain button type suppresses submission behavior. */
			same = control_equal_folded(type, "button");
			if (same)
				return DOM_CONTROL_BUTTON;

			/* The reset type selects the established form-reset behavior. */
			same = control_equal_folded(type, "reset");
			if (same)
				return DOM_CONTROL_RESET;
		}

		/* The default type. */
		return DOM_CONTROL_SUBMIT;
	}

	/* Anything else but an <input> is not a control. */
	if (element->tag != DOM_TAG_INPUT)
		return DOM_CONTROL_NONE;

	/* No type is a text field. */
	if (type == NULL)
		return DOM_CONTROL_TEXT;

	/* The type's kind, by its name in any case. */
	for (index = 0; control_types[index].name != NULL; index++) {
		/* Content input types match their ordinary ASCII names without case sensitivity. */
		same = control_equal_folded(type, control_types[index].name);
		if (same)
			return control_types[index].kind;
	}

	/* Succeeded: an unknown content type uses the ordinary text-field default. */
	return DOM_CONTROL_TEXT;
}

/*
 * Resolves an element's control state or creates its first clean state.
 *
 * Clean state follows content attributes; allocation failure returns NULL.
 */
struct dom_control *
dom_control_of(
	struct dom_element *element)
{
	struct dom_control *control;

	/* The state already made. */
	if (element->control != NULL)
		return element->control;

	/* A new, clean state. */
	control = calloc(1, sizeof(*control));
	if (control == NULL)
		return NULL;

	/* Clean native text state starts without an independently owned value buffer. */
	wb_units_init(&control->value);
	wb_units_init(&control->preedit);

	/* The element owns it from now on. */
	element->control = control;

	/* Succeeded: the element retains this clean control until normal finalization. */
	return control;
}

/*
 * Writes a control's current native value into cleared output storage.
 *
 * Dirty state overrides textarea text or the input value attribute;
 * an absent clean value remains empty.
 */
int
dom_control_value(
	struct dom_element *element,
	struct wb_units *out)
{
	struct vm_string *attribute;
	size_t index;
	uint16_t unit;
	int error;

	/* A dirty value is the control's own. */
	wb_units_clear(out);

	/* Dirty state remains independent of later default text and attribute changes. */
	if (element->control != NULL && element->control->dirty) {
		error = wb_units_append(out, element->control->value.data, element->control->value.length);
		if (error != 0)
			return error;

		/* Succeeded: the caller receives the control's owned dirty value. */
		return 0;
	}

	/* A textarea's default value is its text. */
	if (element->tag == DOM_TAG_TEXTAREA) {
		error = control_text_of(&element->node, out);
		if (error != 0)
			return error;

		/* Succeeded: the caller receives the textarea's ordinary default text. */
		return 0;
	}

	/* An input's default value is its value attribute. */
	attribute = dom_attribute_ascii(element, "value");
	if (attribute == NULL)
		return 0;

	/* The attribute's units. */
	for (index = 0; index < attribute->length; index++) {
		/* Copies each actual attribute unit without changing its owning atom. */
		unit = vm_string_at(attribute, index);
		error = wb_units_append(out, &unit, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the attribute's value is written. */
	return 0;
}

/*
 * Replaces a control's native value and sets its dirty state and caret.
 *
 * The content value attribute no longer overrides the completed replacement.
 */
int
dom_control_set_value(
	struct dom_element *element,
	const uint16_t *units,
	size_t length)
{
	struct dom_control *control;
	int error;

	/* The state that holds the value. */
	control = dom_control_of(element);
	if (control == NULL)
		return ENOMEM;

	/* The new value replaces the old. */
	wb_units_clear(&control->value);
	error = wb_units_append(&control->value, units, length);
	if (error != 0)
		return error;

	/*
	 * dirty tells the submission and the painting that the value is the
	 * control's own now, whatever the value attribute says.
	 */
	control->dirty = 1;
	control->caret = length;

	/* Succeeded: the control has its new value. */
	return 0;
}

/*
 * Writes a button's current native label into cleared output storage.
 *
 * The value attribute overrides Submit, Reset or the plain button's empty label.
 */
int
dom_control_label(
	const struct dom_element *element,
	struct wb_units *out)
{
	struct vm_string *attribute;
	const char *fallback;
	size_t index;
	uint16_t unit;
	int kind;
	int error;

	/* A value attribute is the label. */
	wb_units_clear(out);
	attribute = dom_attribute_ascii(element, "value");
	if (attribute != NULL) {
		/* Copies the complete content value instead of a default button-kind label. */
		for (index = 0; index < attribute->length; index++) {
			/* Appends each actual attribute unit in its original order. */
			unit = vm_string_at(attribute, index);
			error = wb_units_append(out, &unit, 1);
			if (error != 0)
				return error;
		}

		/* The attribute's text is the whole label. */
		return 0;
	}

	/* The kind's own label (a plain button has none). */
	kind = dom_control_kind(element);
	fallback = "";
	if (kind == DOM_CONTROL_SUBMIT)
		fallback = "Submit";

	/* Only an actual reset kind overrides the remaining empty default. */
	if (kind == DOM_CONTROL_RESET)
		fallback = "Reset";

	/* Its characters, which are ASCII. */
	for (index = 0; fallback[index] != '\0'; index++) {
		/* Each established default-label byte is one ordinary ASCII unit. */
		unit = (uint16_t)(unsigned char)fallback[index];
		error = wb_units_append(out, &unit, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the default label is written. */
	return 0;
}

/*
 * Finds the first actual selected option for painting and ordinary form value consumption.
 */
struct dom_element *
dom_select_chosen(
	struct dom_element *select)
{
	struct dom_node *node;
	struct dom_element *option;

	/* The consumer observes owned state without changing a script's explicit empty selection. */
	node = dom_select_option_next(&select->node, NULL);
	while (node != NULL) {
		/* The live option's own selected state decides whether it is displayed. */
		option = (struct dom_element *)node;
		if (option->option_selected)
			return option;

		/* Advances in the select's actual option order without mutating selection. */
		node = dom_select_option_next(&select->node, node);
	}

	/* Succeeded: the actual list has no selection, including an explicitly cleared single select. */
	return NULL;
}

/*
 * Writes an option's collapsed native text label into cleared output storage.
 *
 * Leading and trailing whitespace is trimmed; internal runs become one space.
 */
int
dom_option_text(
	const struct dom_element *option,
	struct wb_units *out)
{
	/* Each collapsed internal whitespace run contributes this single ordinary space. */
	static const uint16_t space = 0x20U;
	struct wb_units text;
	size_t index;
	uint16_t unit;
	int pending;
	int is_space;
	int error;

	/* The option's text. */
	wb_units_clear(out);
	wb_units_init(&text);
	error = control_text_of(&option->node, &text);
	if (error != 0) {
		wb_units_release(&text);
		return error;
	}

	/* Each character, a run of spaces kept as one space between words. */
	pending = 0;
	for (index = 0; index < text.length && error == 0; index++) {
		/* Classifies each actual text unit before extending the normalized label. */
		unit = text.data[index];
		is_space = 0;

		/* The HTML Standard's ASCII whitespace. */
		switch (unit) {
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
			is_space = 1;
			break;
		default:
			break;
		}

		/* Whitespace waits to become one space before the next word. */
		if (is_space) {
			pending = 1;
			continue;
		}

		/* A space goes before a word that follows another. */
		if (pending && out->length != 0) {
			error = wb_units_append(out, &space, 1);
			if (error != 0) {
				wb_units_release(&text);
				return error;
			}
		}

		/* Appends the current word unit only after any internal separator succeeded. */
		pending = 0;
		error = wb_units_append(out, &unit, 1);
		if (error != 0) {
			wb_units_release(&text);
			return error;
		}
	}

	/* The text is no longer needed. */
	wb_units_release(&text);

	/* Succeeded: the label is written. */
	return 0;
}

/*
 * Reports a checkbox's or radio's current native checkedness.
 *
 * Initialized or dirty state overrides the default checked attribute.
 */
int
dom_control_checked(
	const struct dom_element *element)
{
	struct vm_string *attribute;

	/* Current initialized group state and dirty script state both override defaults. */
	if (element->control != NULL &&
	    (element->control->checked_dirty || element->control->checked_initialized))
		return element->control->checked;

	/* Otherwise the attribute's presence. */
	attribute = dom_attribute_ascii(element, "checked");
	if (attribute == NULL)
		return 0;

	/* Succeeded: the actual default checked attribute is present. */
	return 1;
}

/*
 * Releases an element's owned control state during native finalization.
 */
void
dom_control_free(
	struct dom_element *element)
{
	/* An element without a state has nothing to free. */
	if (element->control == NULL)
		return;

	/* The value's units and the text being composed, then the state. */
	wb_units_release(&element->control->value);
	wb_units_release(&element->control->preedit);
	free(element->control);
	element->control = NULL;

	/* Succeeded: the element owns no control buffer or stale state pointer. */
	return;
}

/*
 * Resolves an element's no-namespace attribute by its exact ASCII name.
 *
 * HTML parsing already folds attribute names; absence returns NULL.
 */
struct vm_string *
dom_attribute_ascii(
	const struct dom_element *element,
	const char *name)
{
	const struct dom_attribute *attribute;
	size_t index;
	int same;

	/* Each attribute in no namespace, by its name. */
	for (index = 0; index < element->attribute_count; index++) {
		/* Namespaced attributes cannot impersonate ordinary HTML content attributes. */
		attribute = &element->attributes[index];
		if (attribute->ns != DOM_NS_NONE)
			continue;

		/* The actual interned local name must match this complete ASCII name. */
		same = vm_string_equal_ascii(attribute->name, name);
		if (same)
			return attribute->value;
	}

	/* Succeeded: the actual no-namespace attribute inventory contains no matching name. */
	return NULL;
}

/*
 * Prepares input text before changing its owned value or dirty state.
 */
int
dom_input_set_value(
	struct dom_element *element,
	const uint16_t *units,
	size_t length)
{
	struct dom_control *control;
	struct wb_units prepared;
	struct wb_units previous;
	int status;

	/* The old value stays intact if preparing complete replacement storage fails. */
	wb_units_init(&prepared);
	status = wb_units_append(&prepared, units, length);
	if (status != 0) {
		wb_units_release(&prepared);
		return status;
	}

	/* The element publishes new owned state only after all required storage exists. */
	control = dom_control_of(element);
	if (control == NULL) {
		wb_units_release(&prepared);
		return ENOMEM;
	}

	/* Publish complete owned text with its matching dirty flag and caret. */
	previous = control->value;
	control->value = prepared;
	control->dirty = 1;
	control->caret = length;
	wb_units_release(&previous);

	/*
	 * A script's value ends what an input method was composing
	 * (ws177-p019); the session's change tells the program that the input
	 * method's own composing must go too.
	 */
	if (control->preedit.length != 0)
		element->node.document->compose_session++;
	wb_units_clear(&control->preedit);
	control->preedit_cursor = 0;
	control->preedit_begin = 0;

	/* Painting and submission must observe a script's new dirty text value. */
	element->node.document->generation++;

	/* Succeeded: replacement storage is owned by the input and the old value is released. */
	return 0;
}

/*
 * Copies an input's dirty value into independent native clone storage.
 */
int
dom_input_clone_value(
	struct dom_element *destination,
	const struct dom_element *source)
{
	int input;
	int status;

	/* Other elements retain their established clone semantics. */
	if (source->ns != DOM_NS_HTML || source->tag != DOM_TAG_INPUT)
		return 0;

	/* Exact input identity excludes folded XML spellings from this clone behavior. */
	input = vm_string_equal_ascii(source->local_name, "input");
	if (!input)
		return 0;

	/* Clean inputs follow copied content attributes without allocating dirty state. */
	if (source->control == NULL || !source->control->dirty)
		return 0;

	/* Prepares independent replacement storage under the established input setter contract. */
	status = dom_input_set_value(destination, source->control->value.data, source->control->value.length);
	if (status != 0)
		return status;

	/* Caret is an offset into owned text, never a copied display coordinate. */
	destination->control->caret = source->control->caret;
	if (destination->control->caret > destination->control->value.length)
		destination->control->caret = destination->control->value.length;

	/* Succeeded: the clone owns an independent dirty value buffer. */
	return 0;
}

/* Tells whether a string is an ASCII word in any case (the word is in lower case). */
static int
control_equal_folded(
	const struct vm_string *string,
	const char *ascii)
{
	size_t length;
	size_t index;
	uint16_t unit;

	/* The lengths must agree. */
	length = strlen(ascii);
	if (string->length != length)
		return 0;

	/* Each unit, folded to lower case. */
	for (index = 0; index < length; index++) {
		/* Only uppercase ASCII units participate in case folding for type names. */
		unit = vm_string_at(string, index);
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit - 'A' + 'a');

		/* Any unmatched folded unit rejects the complete ordinary type name. */
		if (unit != (uint16_t)(unsigned char)ascii[index])
			return 0;
	}

	/* Succeeded: every folded unit matches the complete ordinary type name. */
	return 1;
}

/* Appends the text of a node's text children (a textarea's default value). */
static int
control_text_of(
	const struct dom_node *node,
	struct wb_units *out)
{
	const struct dom_character_data *text;
	const struct dom_node *child;
	int error;

	/* Each text child in order (a textarea holds only text). */
	for (child = node->first_child; child != NULL; child = child->next) {
		/* Only direct Text children contribute to this existing default-value operation. */
		if (child->type != DOM_TEXT)
			continue;

		/* Copies the actual character storage without borrowing it past the append. */
		text = (const struct dom_character_data *)child;
		error = wb_units_append(out, text->data.data, text->data.length);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is appended. */
	return 0;
}
