/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bar of editing buttons over a selection the fingers made (KL_VERSION
 * 74, ws190-p002, plan/ws190/phase001/phase.md sections 1.2 to 1.4 and
 * 2.2): Cut, Copy, Paste and Select All, those that apply, in a row on a
 * rounded panel with a shadow.  The library's fields and text area show it
 * through kl_ui (text-select.c); Text Editor uses these calls for its own
 * text view.
 *
 * The bar stands above the selection, below it (past the handles' knobs)
 * when there is no room above, and over the selection's top when neither
 * fits, always within the bounds it is given and across the selection's
 * middle as far as the bounds let it.
 */

#include "internal.h"

#include <string.h>

/* The size of the labels' text, and the thickness of the line between two buttons. */
#define BAR_TEXT		14U
#define BAR_DIVIDER		1.0f

/* How soft the bar's shadow is, and its corners' radius. */
#define BAR_SHADOW		10.0f
#define BAR_RADIUS		10.0f

/* The knob under a handle, which a bar below the selection stays clear of. */
#define BAR_KNOB		KL_TEXT_HANDLE

/*
 * The buttons in the order they stand, from the left, each with its label
 * (the library's widgets' text is English).
 */
static const unsigned bar_kinds[KL_TEXT_BAR_BUTTONS] = {
	KL_TEXT_BAR_CUT,
	KL_TEXT_BAR_COPY,
	KL_TEXT_BAR_PASTE,
	KL_TEXT_BAR_SELECT_ALL
};
static const char *const bar_labels[KL_TEXT_BAR_BUTTONS] = {
	"Cut",
	"Copy",
	"Paste",
	"Select All"
};

/*
 * The bar's calls, which a field gives kl_ui when it begins the fingers'
 * selection (internal.h).  Constant for the library's life.
 */
const struct keiui_bar_calls keiui_text_bar_calls = {
	kl_text_bar_buttons,
	kl_text_bar_layout,
	kl_text_bar_hit,
	kl_text_bar_draw
};

static int bar_intersect(const struct kl_rect *first, const struct kl_rect *second, struct kl_rect *result);
static int bar_top(const struct kl_rect *selection, const struct kl_rect *bounds);
static int bar_left(const struct kl_rect *selection, const struct kl_rect *bounds, int width);
static const char *bar_label(unsigned kind);

/*
 * Chooses the bar's buttons from what the text is (KL_TEXT_BAR_SELECTED
 * and the other facts): Cut and Copy for a selection that may leave the
 * text, Paste when there is text to paste in, Select All while the
 * selection is not the whole text.  Returns the buttons (KL_TEXT_BAR_CUT
 * and the others), 0 for none.
 */
unsigned
kl_text_bar_buttons(
	unsigned facts)
{
	unsigned buttons;
	int selected;
	int secret;
	int read_only;
	int clipboard;

	/* What the choice depends on. */
	buttons = 0U;
	selected = (facts & KL_TEXT_BAR_SELECTED) != 0U;
	secret = (facts & KL_TEXT_BAR_SECRET) != 0U;
	read_only = (facts & KL_TEXT_BAR_READ_ONLY) != 0U;
	clipboard = (facts & KL_TEXT_BAR_CLIPBOARD) != 0U;

	/* Copy: a selection, not a secret one, and a clipboard to copy to. */
	if (selected && !secret && clipboard)
		buttons |= KL_TEXT_BAR_COPY;

	/* Cut: the same, and a text that may change. */
	if ((buttons & KL_TEXT_BAR_COPY) != 0U && !read_only)
		buttons |= KL_TEXT_BAR_CUT;

	/* Paste: text in the clipboard and a text that may change (a secret field takes a pasted password). */
	if (clipboard && (facts & KL_TEXT_BAR_CAN_PASTE) != 0U && !read_only)
		buttons |= KL_TEXT_BAR_PASTE;

	/* Select All: some text, not all of it selected already. */
	if ((facts & KL_TEXT_BAR_EMPTY) == 0U && (facts & KL_TEXT_BAR_WHOLE) == 0U)
		buttons |= KL_TEXT_BAR_SELECT_ALL;

	/* Reports the buttons. */
	return buttons;
}

/*
 * Lays the bar of some buttons out over a selection (window coordinates):
 * the part of the selection within visible is what it stands by, and it
 * stays within bounds.  Returns 1 with the bar laid out, 0 for no bar (no
 * button, the selection out of sight, or bounds too low for it); bar's
 * count is then 0.
 */
int
kl_text_bar_layout(
	struct kl_text_bar *bar,
	struct kl_text *text,
	unsigned buttons,
	const struct kl_rect *selection,
	const struct kl_rect *visible,
	const struct kl_rect *bounds)
{
	struct kl_rect shown;
	size_t index;
	int widths[KL_TEXT_BAR_BUTTONS];
	int label_width;
	int width;
	int x;
	int inside;

	/* No bar until it is laid out. */
	memset(bar, 0, sizeof(*bar));

	/* The buttons shown, in their order, and the bar's width. */
	width = 0;
	for (index = 0; index < KL_TEXT_BAR_BUTTONS; index++) {
		if ((buttons & bar_kinds[index]) == 0U)
			continue;
		label_width = kl_text_width(text, bar_labels[index], strlen(bar_labels[index]), BAR_TEXT, 0);
		widths[bar->count] = label_width + 2 * KL_TEXT_BAR_MARGIN;
		bar->kinds[bar->count] = bar_kinds[index];
		width += widths[bar->count];
		bar->count++;
	}

	/* No button, no bar. */
	if (bar->count == 0U)
		return 0;

	/* The part of the selection in sight; none in sight, no bar. */
	inside = bar_intersect(selection, visible, &shown);
	if (!inside) {
		bar->count = 0;
		return 0;
	}

	/* Bounds too low for the bar and its gaps. */
	if (bounds->height < KL_TEXT_BAR_HEIGHT + 2 * KL_TEXT_BAR_GAP) {
		bar->count = 0;
		return 0;
	}

	/* The bar's place. */
	bar->buttons = buttons;
	bar->rect.x = bar_left(&shown, bounds, width);
	bar->rect.y = bar_top(&shown, bounds);
	bar->rect.width = width;
	bar->rect.height = KL_TEXT_BAR_HEIGHT;

	/* Each cell from the left. */
	x = bar->rect.x;
	for (index = 0; index < bar->count; index++) {
		bar->cells[index].x = x;
		bar->cells[index].y = bar->rect.y;
		bar->cells[index].width = widths[index];
		bar->cells[index].height = KL_TEXT_BAR_HEIGHT;
		x += widths[index];
	}

	/* Succeeded: the bar is laid out. */
	return 1;
}

/*
 * Records the bar's buttons for the input of the frame being drawn (a
 * press on them keeps the keyboard's focus where it is, and a drag that
 * starts on them does nothing) and reports the button clicked or tapped
 * since the last frame (one KL_TEXT_BAR_* button, 0 for none).  held (may
 * be NULL) is the button the pointer holds, 0 for none.
 */
unsigned
kl_text_bar_hit(
	struct kl_ui *ui,
	uint32_t id,
	const struct kl_text_bar *bar,
	unsigned *held)
{
	unsigned pressed;
	unsigned state;
	size_t index;

	/* Nothing pressed or held yet. */
	pressed = 0U;
	if (held != NULL)
		*held = 0U;

	/* Each cell, an index of the id. */
	for (index = 0; index < bar->count; index++) {
		state = keiui_ui_widget(ui, id, (uint32_t)index, &bar->cells[index], KEIUI_KEEP_FOCUS | KEIUI_NO_DRAG);

		/* Clicked or tapped since the last frame. */
		if ((state & KL_HIT_CLICKED) != 0U)
			pressed = bar->kinds[index];

		/* Held by the pointer. */
		if ((state & KL_HIT_ACTIVE) != 0U && held != NULL)
			*held = bar->kinds[index];
	}

	/* Reports the button pressed. */
	return pressed;
}

/*
 * Draws a bar laid out by kl_text_bar_layout: its shadow and panel, each
 * button's label, a thin line between two buttons, and the ground of the
 * button held (KL_TEXT_BAR_* or 0).
 */
void
kl_text_bar_draw(
	const struct kl_text_bar *bar,
	const struct kl_style *style,
	unsigned held)
{
	const struct kl_theme *theme;
	const struct kl_rect *cell;
	const char *label;
	size_t index;
	int label_width;
	int baseline;
	float divider_x;

	/* No bar, nothing drawn. */
	if (bar->count == 0U)
		return;

	/* The shadow, the panel and its edge. */
	theme = style->theme;
	kl_canvas_shadow(style->canvas, (float)bar->rect.x, (float)bar->rect.y, (float)bar->rect.width, (float)bar->rect.height, BAR_RADIUS, BAR_SHADOW, theme->shadow);
	kl_canvas_round(style->canvas, (float)bar->rect.x, (float)bar->rect.y, (float)bar->rect.width, (float)bar->rect.height, BAR_RADIUS, theme->panel);
	kl_canvas_round_border(style->canvas, (float)bar->rect.x, (float)bar->rect.y, (float)bar->rect.width, (float)bar->rect.height, BAR_RADIUS, 1.0f, theme->control_edge);

	/* Each button: its ground when held, its label in the middle, and the line before it. */
	baseline = kl_text_center(BAR_TEXT, bar->rect.y, bar->rect.height);
	for (index = 0; index < bar->count; index++) {
		cell = &bar->cells[index];

		/* The held button's ground, inside the panel's edge. */
		if (bar->kinds[index] == held) {
			kl_canvas_round(style->canvas, (float)cell->x + 2.0f, (float)cell->y + 2.0f, (float)cell->width - 4.0f, (float)cell->height - 4.0f, BAR_RADIUS - 2.0f, theme->selection);
		}

		/* The label in the middle of its cell. */
		label = bar_label(bar->kinds[index]);
		label_width = kl_text_width(style->text, label, strlen(label), BAR_TEXT, 0);
		(void)kl_text_draw(style->text, style->canvas, cell->x + (cell->width - label_width) / 2, baseline, label, strlen(label), BAR_TEXT, 0, theme->text);

		/* The line between this button and the one before it. */
		if (index > 0U) {
			divider_x = (float)cell->x;
			kl_canvas_line(style->canvas, divider_x, (float)cell->y + 8.0f, divider_x, (float)(cell->y + cell->height) - 8.0f, BAR_DIVIDER, theme->control_edge);
		}
	}
}

/* Gives the part two rectangles share; returns 1 with it, 0 when they share nothing. */
static int
bar_intersect(
	const struct kl_rect *first,
	const struct kl_rect *second,
	struct kl_rect *result)
{
	int left;
	int top;
	int right;
	int bottom;

	/* The later left and top edges. */
	left = first->x;
	if (second->x > left)
		left = second->x;
	top = first->y;
	if (second->y > top)
		top = second->y;

	/* The earlier right and bottom edges. */
	right = first->x + first->width;
	if (second->x + second->width < right)
		right = second->x + second->width;
	bottom = first->y + first->height;
	if (second->y + second->height < bottom)
		bottom = second->y + second->height;

	/* Nothing shared (a selection of no width, a caret, shares its line's height). */
	if (right < left || bottom <= top)
		return 0;

	/* Succeeded: the shared part. */
	result->x = left;
	result->y = top;
	result->width = right - left;
	result->height = bottom - top;
	return 1;
}

/*
 * Reports the bar's top edge: above the selection, else below it past the
 * handles' knobs, else over the selection's top, within the bounds.
 */
static int
bar_top(
	const struct kl_rect *selection,
	const struct kl_rect *bounds)
{
	int top;
	int lowest;

	/* Above the selection, when it fits. */
	top = selection->y - KL_TEXT_BAR_GAP - KL_TEXT_BAR_HEIGHT;
	if (top >= bounds->y)
		return top;

	/* Below the selection and its knobs, when it fits. */
	lowest = bounds->y + bounds->height - KL_TEXT_BAR_GAP - KL_TEXT_BAR_HEIGHT;
	top = selection->y + selection->height + BAR_KNOB + KL_TEXT_BAR_GAP;
	if (top <= lowest)
		return top;

	/* Neither: over the selection's top, within the bounds. */
	top = selection->y + KL_TEXT_BAR_GAP;
	if (top < bounds->y + KL_TEXT_BAR_GAP)
		top = bounds->y + KL_TEXT_BAR_GAP;
	if (top > lowest)
		top = lowest;

	/* Reports the top. */
	return top;
}

/* Reports the bar's left edge: across the selection's middle, kept within the bounds. */
static int
bar_left(
	const struct kl_rect *selection,
	const struct kl_rect *bounds,
	int width)
{
	int left;

	/* A bar wider than the bounds starts at their left edge. */
	if (width > bounds->width - 2 * KL_TEXT_BAR_GAP)
		return bounds->x;

	/* Across the middle of the selection. */
	left = selection->x + selection->width / 2 - width / 2;

	/* Not past either side of the bounds. */
	if (left < bounds->x + KL_TEXT_BAR_GAP)
		left = bounds->x + KL_TEXT_BAR_GAP;
	if (left > bounds->x + bounds->width - KL_TEXT_BAR_GAP - width)
		left = bounds->x + bounds->width - KL_TEXT_BAR_GAP - width;

	/* Reports the left edge. */
	return left;
}

/* Reports a button's label. */
static const char *
bar_label(
	unsigned kind)
{
	size_t index;

	/* The button's place in the order. */
	for (index = 0; index < KL_TEXT_BAR_BUTTONS; index++) {
		if (bar_kinds[index] == kind)
			return bar_labels[index];
	}

	/* An unknown button has no label. */
	return "";
}
