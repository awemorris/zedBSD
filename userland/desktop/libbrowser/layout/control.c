/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boxes of form controls (ws074-p032): an <input> or a <textarea> is a
 * replaced box whose natural size comes from its font and its attributes,
 * as Chromium sizes them -- a text field is size characters wide and one
 * line tall, a button is as wide as its label, a checkbox is 13 pixels
 * square, a textarea is cols characters by rows lines.  The text a field
 * or a button shows sits on a baseline inside its content box, which is
 * the baseline the control stands on in its line.
 */

#include "layout/layout.h"

#include <errno.h>
#include <stdlib.h>

/* The side of a checkbox or a radio button, in pixels. */
#define CONTROL_CHECK_SIZE	13

/* A text field's width in characters when its size attribute gives none. */
#define CONTROL_SIZE_DEFAULT	20

/* A textarea's columns and rows when its attributes give none. */
#define CONTROL_COLS_DEFAULT	20
#define CONTROL_ROWS_DEFAULT	2

/* The room a textarea keeps for its scroll bar, in pixels, as in Chromium. */
#define CONTROL_SCROLLBAR	15

/* The room a select keeps at its right for its arrow, in pixels. */
#define CONTROL_ARROW_ROOM	20

/* The character whose advance stands for a character's width: the digit zero, as UTF-16. */
static const uint16_t control_zero[1] = { 0x30U };

static long control_number(const struct dom_element *element, const char *name, long fallback);
static int control_widest_option(struct layout_tree *tree, struct dom_element *select, const struct text_font *font, layout_unit *widest);

/*
 * Measures a form control's box: its natural width and height and the
 * baseline of its text from its content box's top.  A box that is not a
 * control is left as it is.
 */
int
layout_control_measure(
	struct layout_tree *tree,
	struct layout_box *box)
{
	struct dom_element *element;
	struct text_font font;
	struct wb_units label;
	layout_unit line;
	layout_unit ascent;
	layout_unit average;
	layout_unit extra;
	layout_unit width;
	long count;
	long rows;
	int average_pixels;
	int maximum_pixels;
	int error;

	/* Only an element's box can be a control. */
	if (box->node == NULL || box->node->type != DOM_ELEMENT)
		return 0;
	element = (struct dom_element *)box->node;
	box->control = dom_control_kind(element);
	if (box->control == DOM_CONTROL_NONE || box->control == DOM_CONTROL_HIDDEN) {
		box->control = DOM_CONTROL_NONE;
		return 0;
	}

	/* A checkbox or a radio button is a small square that stands on the baseline. */
	if (box->control == DOM_CONTROL_CHECKBOX || box->control == DOM_CONTROL_RADIO) {
		box->natural_width = layout_from_px((float)CONTROL_CHECK_SIZE * box->style.zoom_eff);
		box->natural_height = layout_from_px((float)CONTROL_CHECK_SIZE * box->style.zoom_eff);
		box->has_baseline = 0;
		return 0;
	}

	/* The text's font, its line and where its baseline is in the line. */
	layout_font_of(tree, &box->style, &font);
	error = layout_control_line(tree->text, &box->style, &line, &ascent);
	if (error != 0)
		return error;

	/*
	 * A character's width, as Chromium takes it: the font's average width,
	 * with the room of its widest character past that added once to a field
	 * (not to a textarea); without those measures, the advance of the digit
	 * zero.
	 */
	extra = 0;
	error = text_font_char_widths(tree->text, &font, &average_pixels, &maximum_pixels);
	if (error == 0) {
		average = (layout_unit)average_pixels * LAYOUT_UNIT;
		extra = (layout_unit)(maximum_pixels - average_pixels) * LAYOUT_UNIT;
	} else {
		error = layout_units_width(tree->text, &font, control_zero, 1, &average);
		if (error != 0)
			return error;
	}

	/* A button is as wide as its label and one line tall. */
	if (box->control == DOM_CONTROL_BUTTON ||
	    box->control == DOM_CONTROL_SUBMIT ||
	    box->control == DOM_CONTROL_RESET) {
		wb_units_init(&label);
		error = dom_control_label(element, &label);
		if (error == 0)
			error = layout_units_width(tree->text, &font, label.data, label.length, &width);
		wb_units_release(&label);
		if (error != 0)
			return error;
		box->natural_width = width;
		box->natural_height = line;
		box->control_baseline = ascent;
		box->has_baseline = 1;
		return 0;
	}

	/* A select is as wide as its widest option and its arrow, and one line tall. */
	if (box->control == DOM_CONTROL_SELECT) {
		error = control_widest_option(tree, element, &font, &width);
		if (error != 0)
			return error;
		box->natural_width = width + CONTROL_ARROW_ROOM * LAYOUT_UNIT;
		box->natural_height = line;
		box->control_baseline = ascent;
		box->has_baseline = 1;
		return 0;
	}

	/* A textarea is cols characters (and its scroll bar) by rows lines; its first line's baseline. */
	if (box->control == DOM_CONTROL_TEXTAREA) {
		count = control_number(element, "cols", CONTROL_COLS_DEFAULT);
		rows = control_number(element, "rows", CONTROL_ROWS_DEFAULT);
		box->natural_width = (layout_unit)count * average + CONTROL_SCROLLBAR * LAYOUT_UNIT;
		box->natural_height = (layout_unit)rows * line;
		box->control_baseline = ascent;
		box->has_baseline = 1;
		return 0;
	}

	/* A text field is size characters wide and one line tall. */
	count = control_number(element, "size", CONTROL_SIZE_DEFAULT);
	box->natural_width = (layout_unit)count * average + extra;
	box->natural_height = line;
	box->control_baseline = ascent;
	box->has_baseline = 1;

	/* Succeeded: the control's box is measured. */
	return 0;
}

/*
 * Measures the line a control's text is set in under its style's
 * line-height: its height, and how far down it the baseline is (half the
 * leading, then the ascent).
 */
int
layout_control_line(
	struct text_system *text,
	const struct css_style *style,
	layout_unit *line,
	layout_unit *ascent)
{
	struct text_font font;
	struct text_metrics metrics;
	layout_unit height;
	layout_unit content;
	int monospace;
	int error;

	/* The style's font. */
	monospace = 0;
	if (style->generic_family == CSS_FAMILY_MONOSPACE)
		monospace = 1;
	text_select_font(text, monospace, style->font_size, style->font_weight, &font);

	/* Its measures. */
	error = text_font_metrics(text, &font, &metrics);
	if (error != 0)
		return error;

	/* The line: normal, a multiple of the font size, or a length. */
	content = (layout_unit)(metrics.ascent + metrics.descent) * LAYOUT_UNIT;
	height = (layout_unit)metrics.line_height * LAYOUT_UNIT;
	if (style->line_height.unit == CSS_UNIT_NUMBER)
		height = layout_from_px(style->line_height.value * style->font_size);
	if (style->line_height.unit == CSS_UNIT_PX)
		height = layout_from_px(style->line_height.value);

	/* Half the leading above the ascent. */
	*line = height;
	*ascent = (layout_unit)metrics.ascent * LAYOUT_UNIT + (height - content) / 2;

	/* Succeeded: the line is measured. */
	return 0;
}

/*
 * Measures the advance of a run of UTF-16 text in a font, in layout units.
 */
int
layout_units_width(
	struct text_system *text,
	const struct text_font *font,
	const uint16_t *units,
	size_t length,
	layout_unit *width)
{
	struct text_glyph glyph;
	uint32_t code_point;
	size_t position;
	size_t used;
	int error;

	/* Each character's advance, added up. */
	*width = 0;
	position = 0;
	while (position < length) {
		used = wb_utf16_decode(units + position, length - position, &code_point);
		position += used;
		error = text_glyph(text, font, code_point, 0, &glyph);
		if (error != 0)
			return error;
		*width += (layout_unit)glyph.advance_units;
	}

	/* Succeeded: the run's width. */
	return 0;
}

/* Measures the widest option label of a select (through its optgroups). */
static int
control_widest_option(
	struct layout_tree *tree,
	struct dom_element *select,
	const struct text_font *font,
	layout_unit *widest)
{
	struct dom_node *node;
	struct wb_units label;
	layout_unit width;
	int is_option;
	int error;

	/* Each option's label, measured. */
	*widest = 0;
	error = 0;
	wb_units_init(&label);
	node = select->node.first_child;
	while (node != NULL && error == 0) {
		is_option = dom_element_is(node, DOM_NS_HTML, DOM_TAG_OPTION);
		if (is_option) {
			/* The label's width widens the select. */
			error = dom_option_text((struct dom_element *)node, &label);
			if (error == 0)
				error = layout_units_width(tree->text, font, label.data, label.length, &width);
			if (error == 0 && width > *widest)
				*widest = width;
		}

		/* The next node in the select: a child, else a sibling of the node or of an ancestor. */
		if (node->first_child != NULL) {
			node = node->first_child;
			continue;
		}
		while (node != NULL && node != &select->node && node->next == NULL)
			node = node->parent;
		if (node == NULL || node == &select->node)
			break;
		node = node->next;
	}

	/* The label is no longer needed. */
	wb_units_release(&label);
	if (error != 0)
		return error;

	/* Succeeded: the widest label. */
	return 0;
}

/*
 * Reads a positive whole number from an attribute (size, cols, rows), or
 * the fallback when the attribute is missing, not a number, or not above
 * zero.
 */
static long
control_number(
	const struct dom_element *element,
	const char *name,
	long fallback)
{
	struct vm_string *value;
	size_t index;
	uint16_t unit;
	long number;
	int digits;
	int space;

	/* The attribute. */
	value = dom_attribute_ascii(element, name);
	if (value == NULL)
		return fallback;

	/* Spaces before the digits are skipped. */
	index = 0;
	while (index < value->length) {
		unit = vm_string_at(value, index);
		space = 0;

		/* The HTML Standard's ASCII whitespace. */
		switch (unit) {
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
			space = 1;
			break;
		default:
			break;
		}

		/* The first character that is not a space starts the number. */
		if (!space)
			break;
		index++;
	}

	/* The digits, up to a size no page needs. */
	number = 0;
	digits = 0;
	while (index < value->length) {
		unit = vm_string_at(value, index);
		if (unit < '0' || unit > '9')
			break;
		if (number < 100000L)
			number = number * 10 + (long)(unit - '0');
		digits++;
		index++;
	}

	/* No digits, or zero, is the fallback. */
	if (digits == 0 || number <= 0)
		return fallback;

	/* Succeeded: the number. */
	return number;
}
