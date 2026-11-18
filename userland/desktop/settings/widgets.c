/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts Settings' pages are built of: the page's header, a card with
 * its title, a row of a label and its value, and the mark.
 *
 * Each part is drawn from a top edge within a column and returns the edge
 * below it, so that a page lays its parts out from the top down.
 */

#include "settings.h"

#include "../artwork/mark.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The header's text sizes: the page's name and its summary. */
#define WIDGETS_TEXT_TITLE	30U
#define WIDGETS_TEXT_SUMMARY	15U

/* A card's corners, its inner margin, its title's and subtitle's sizes, and the space its title takes. */
#define WIDGETS_CARD_RADIUS	14.0f
#define WIDGETS_CARD_PAD	18
#define WIDGETS_TEXT_CARD	16U
#define WIDGETS_TEXT_CARD_SUB	13U
#define WIDGETS_CARD_TITLE	46

/* A row's height and text size, and the share of the row its label takes. */
#define WIDGETS_ROW_HEIGHT	40
#define WIDGETS_TEXT_ROW	14U
#define WIDGETS_LABEL_SHARE	0.34f

/* The largest mark drawn, in pixels a side (its layers are kept rendered at the last size). */
#define WIDGETS_MARK_MAX	160U

/* A switch's size. */
#define WIDGETS_TOGGLE_WIDTH	44
#define WIDGETS_TOGGLE_HEIGHT	24

/* A slider's height (the knob's room). */
#define WIDGETS_SLIDER_HEIGHT	28

/* A button's height, its side margins and its text size. */
#define WIDGETS_BUTTON_HEIGHT	32
#define WIDGETS_BUTTON_SIDE	16
#define WIDGETS_TEXT_BUTTON	14U

/* The room at each end of libkeiland's slider's track (its knob's radius, widgets.c of libkeiland). */
#define WIDGETS_SLIDER_KNOB	9

/* The A key (Ctrl+A selects a field's whole text). */
#define WIDGETS_KEY_A		30U

/*
 * The widgets' input every text field lives in (se_fields_open,
 * ws090-p007), the text the fields are drawn with, and a canvas of one
 * pixel a field's keys are carried out on at once (se_field_key).
 */
static struct kl_ui *widgets_ui;
static struct kl_text *widgets_text;
static uint32_t widgets_pixel;
static struct kl_canvas widgets_tiny;

static uint32_t widgets_field_id(const struct kl_field *field);
static uint32_t widgets_control_id(int index);
static void widgets_style(struct se_app *app, struct kl_canvas *canvas, struct kl_style *style);

/*
 * Draws a page's header: its name large and its summary under it
 * (libkeiland's kl_header, ws090-p023).  Returns the edge below the header.
 */
int
se_page_header(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct se_page *page,
	int x,
	int top,
	int width)
{
	struct kl_style style;
	char summary[SE_SUMMARY_TEXT];
	const char *line;
	int bottom;

	/*
	 * A page of the table has its summary in the language with the
	 * system's name in it; a header made for the moment (the search's)
	 * is shown as it was written, braces and all.
	 */
	line = page->summary;
	if (page->id < SE_PAGES && page == &se_pages[page->id])
		line = se_named(summary, sizeof(summary), kl_tr(page->summary));

	/* libkeiland's header in the page's words. */
	widgets_style(app, canvas, &style);
	bottom = kl_header(&style, x, top, width, kl_tr(page->name), line);
	return bottom;
}

/*
 * Draws a card's ground and its title (and subtitle, when one is given)
 * in a rectangle (libkeiland's kl_card, ws090-p023).  Returns the edge
 * where the card's content starts.
 */
int
se_card_begin(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int height,
	const char *title,
	const char *subtitle)
{
	struct kl_style style;
	struct kl_rect rect;
	int content;

	/* libkeiland's card. */
	widgets_style(app, canvas, &style);
	rect.x = x;
	rect.y = top;
	rect.width = width;
	rect.height = height;
	content = kl_card(&style, &rect, title, subtitle);
	return content;
}

/*
 * Reports how tall a card is that holds some rows of values, with or
 * without a title (no subtitle).
 */
int
se_card_height(
	int rows,
	int titled)
{
	int height;

	/* The margins and the rows. */
	height = 2 * WIDGETS_CARD_PAD + rows * WIDGETS_ROW_HEIGHT;

	/* The title's room. */
	if (titled != 0)
		height += WIDGETS_CARD_TITLE - WIDGETS_CARD_PAD;

	/* The card's height. */
	return height;
}

/*
 * Draws one row of a card: a label and its value beside it, with a thin
 * line under the row unless it is the card's last (libkeiland's kl_row,
 * ws090-p023).  x and width are the card's.  Returns the edge below the
 * row.
 */
int
se_row_value(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const char *label,
	const char *value,
	int last)
{
	struct kl_style style;
	int bottom;

	/* libkeiland's row. */
	widgets_style(app, canvas, &style);
	bottom = kl_row(&style, x, top, width, label, value, last);
	return bottom;
}

/*
 * Draws the mark in a square of a size in pixels at (x, y), as opaque
 * as asked (0..1): its seven layers (userland/desktop/artwork/mark.c), each
 * in its colour, as the file manager draws it (ws035-p109).
 */
void
se_mark_draw(
	struct kl_canvas *canvas,
	int x,
	int y,
	unsigned pixels,
	float opacity)
{
	/*
	 * The bar pale, its shade deeper, the leaf clearer and its shade the
	 * deep blue of the splash, the overlap deeper still, then the white
	 * light along the edges and the sheen (colour, alpha).  The panes are
	 * translucent, so what is behind shows through.
	 */
	static const uint32_t colours[KL_MARK_LAYERS][2] = {
		{ 0xa9c3f6U, 175U },
		{ 0x7fa2f0U, 90U },
		{ 0xa3d8faU, 170U },
		{ 0x3a86f5U, 170U },
		{ 0x2f7cf3U, 200U },
		{ 0xffffffU, 170U },
		{ 0xffffffU, 60U }
	};

	/*
	 * The layers at the size last drawn (zero before the first); a new size
	 * renders them again.  They live for the program's life.
	 */
	static uint8_t layers[KL_MARK_LAYERS][WIDGETS_MARK_MAX * WIDGETS_MARK_MAX];
	static unsigned layers_pixels;
	uint32_t alpha;
	unsigned layer;

	/* A mark larger than the kept layers is drawn at their largest; an empty one not at all. */
	if (pixels > WIDGETS_MARK_MAX)
		pixels = WIDGETS_MARK_MAX;
	if (pixels == 0U)
		return;

	/* The layers at this size. */
	if (layers_pixels != pixels) {
		for (layer = 0; layer < KL_MARK_LAYERS; layer++)
			kl_mark_raster(layer, pixels, layers[layer], pixels);
		layers_pixels = pixels;
	}

	/* Each layer in its colour, as opaque as asked. */
	for (layer = 0; layer < KL_MARK_LAYERS; layer++) {
		alpha = (uint32_t)((float)colours[layer][1] * opacity + 0.5f);
		kl_canvas_mask(canvas, x, y, layers[layer], (int)pixels, (int)pixels, pixels, KL_RGBA(colours[layer][0], alpha));
	}
}

/*
 * Draws a switch with its top left at (x, y) (libkeiland's kl_switch,
 * ws090-p023): the accent with the knob on the right when on, faded when
 * it does nothing.  An enabled switch is a page's control (index), which
 * the page flips when it is clicked.
 */
void
se_toggle_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int on,
	int enabled,
	int index)
{
	struct kl_style style;
	struct kl_rect rect;
	unsigned flags;
	int shown;

	/* libkeiland's switch, as the page has it (the page flips it, not the switch). */
	widgets_style(app, canvas, &style);
	flags = 0U;
	if (enabled == 0)
		flags = KL_BUTTON_DISABLED;
	shown = on;
	(void)kl_switch(widgets_ui, &style, widgets_control_id(index), x, y, &shown, flags);

	/* An enabled switch is clickable. */
	if (enabled != 0) {
		rect.x = x - 4;
		rect.y = y - 4;
		rect.width = WIDGETS_TOGGLE_WIDTH + 8;
		rect.height = WIDGETS_TOGGLE_HEIGHT + 8;
		se_ui_hit(app, &rect, SE_HIT_CONTROL, index);
	}
}

/*
 * Draws a slider from (x, y), width long (libkeiland's kl_slider_flags,
 * ws090-p023): a track filled in the accent up to a fraction (0..1) and a
 * knob there, faded when it does nothing.  An enabled slider is a page's
 * control (index) whose rectangle is given back for a drag
 * (se_slider_fraction); the page moves it.
 */
void
se_slider_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width,
	float fraction,
	int enabled,
	int index,
	struct kl_rect *rect)
{
	struct kl_style style;
	struct kl_rect track;
	unsigned flags;
	double shown;

	/* Within the track. */
	if (fraction < 0.0f)
		fraction = 0.0f;
	if (fraction > 1.0f)
		fraction = 1.0f;

	/* libkeiland's slider, its track from x to x + width, at the page's value (the page moves it). */
	widgets_style(app, canvas, &style);
	track.x = x - WIDGETS_SLIDER_KNOB;
	track.y = y;
	track.width = width + 2 * WIDGETS_SLIDER_KNOB;
	track.height = WIDGETS_SLIDER_HEIGHT;
	flags = 0U;
	if (enabled == 0)
		flags = KL_BUTTON_DISABLED;
	shown = (double)fraction;
	(void)kl_slider_flags(widgets_ui, &style, widgets_control_id(index), &track, 0.0, 1.0, 0.0, &shown, flags);

	/* The whole track takes presses and drags (a little taller than it looks). */
	rect->x = x - 12;
	rect->y = y;
	rect->width = width + 24;
	rect->height = WIDGETS_SLIDER_HEIGHT;
	if (enabled != 0)
		se_ui_hit(app, rect, SE_HIT_CONTROL, index);
}

/*
 * Reports where along a slider a pointer at x is, from 0 (its left end)
 * to 1 (its right end).
 */
float
se_slider_fraction(
	const struct kl_rect *rect,
	int x)
{
	float fraction;

	/* The track lies 12 pixels inside the rectangle at each end. */
	fraction = (float)(x - rect->x - 12) / (float)(rect->width - 24);

	/* Before the left end. */
	if (fraction < 0.0f)
		return 0.0f;

	/* After the right end. */
	if (fraction > 1.0f)
		return 1.0f;

	/* On the track. */
	return fraction;
}

/*
 * Reports how wide a button with a label is (libkeiland's).
 */
int
se_button_width(
	struct se_app *app,
	const char *label)
{
	struct kl_style style;
	int width;

	/* libkeiland's measure. */
	widgets_style(app, NULL, &style);
	width = kl_button_width(&style, label);
	return width;
}

/*
 * Draws a button with its top left at (x, y) (libkeiland's kl_button,
 * ws090-p023): the accent when primary, darker under the pointer, faded
 * when it does nothing.  An enabled button is a page's control (index),
 * which the page carries out when it is clicked.  Returns its width.
 */
int
se_button_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	const char *label,
	int primary,
	int enabled,
	int index)
{
	struct kl_style style;
	struct kl_rect rect;
	unsigned flags;
	int width;

	/* The button's size. */
	widgets_style(app, canvas, &style);
	width = kl_button_width(&style, label);
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = WIDGETS_BUTTON_HEIGHT;

	/* libkeiland's button (its click is the page's, through the control's hit). */
	flags = 0U;
	if (primary != 0)
		flags |= KL_BUTTON_PRIMARY;
	if (enabled == 0)
		flags |= KL_BUTTON_DISABLED;
	(void)kl_button(widgets_ui, &style, widgets_control_id(index), &rect, label, flags);

	/* An enabled button is clickable. */
	if (enabled != 0)
		se_ui_hit(app, &rect, SE_HIT_CONTROL, index);

	/* The button's width. */
	return width;
}

/*
 * Draws a button that is a picture alone, WIDGETS_BUTTON_HEIGHT square with
 * its top left at (x, y): libkeiland's (white within a thin edge, darker
 * under the pointer, the icon in the middle).  It is a page's control
 * (index).
 */
void
se_icon_button_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	enum kl_icon icon,
	int index)
{
	struct kl_style style;
	struct kl_rect rect;

	/* The button's square. */
	rect.x = x;
	rect.y = y;
	rect.width = WIDGETS_BUTTON_HEIGHT;
	rect.height = WIDGETS_BUTTON_HEIGHT;

	/* libkeiland's button (its click is the page's, through the control's hit). */
	widgets_style(app, canvas, &style);
	(void)kl_icon_button(widgets_ui, &style, widgets_control_id(index), 0U, &rect, icon, WIDGETS_BUTTON_HEIGHT - 12, 0U);

	/* It is clickable. */
	se_ui_hit(app, &rect, SE_HIT_CONTROL, index);
}

/*
 * Draws a status dot (green for connected, grey for not) centred at (cx, cy).
 */
void
se_dot_draw(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	kl_color color)
{
	/* A small filled circle. */
	kl_canvas_circle(canvas, cx, cy, 4.5f, color);
}

/*
 * Draws a signal's four bars with their bottom left at (x, y): as many
 * full as the strength (dBm) earns, the rest faint.
 */
void
se_signal_draw(
	struct kl_canvas *canvas,
	float x,
	float y,
	int rssi,
	kl_color color)
{
	kl_color bar;
	float height;
	int bars;
	int index;

	/* The bars the strength earns: four above -55 dBm, one below -75. */
	bars = 1;
	if (rssi > -75)
		bars = 2;
	if (rssi > -65)
		bars = 3;
	if (rssi > -55)
		bars = 4;

	/* Each bar, taller to the right. */
	for (index = 0; index < 4; index++) {
		bar = color;
		if (index >= bars)
			bar = KL_RGBA(0x8a96aa, 90);
		height = 4.0f + 3.5f * (float)index;
		kl_canvas_round(canvas, x + 5.0f * (float)index, y - height, 3.0f, height, 1.0f, bar);
	}
}

/*
 * Writes a number of bytes as text in the largest unit under a thousand
 * of it (B, KB, MB, GB).
 */
void
se_bytes_text(
	uint64_t bytes,
	char *text,
	size_t size)
{
	static const char *const units[] = { "KB", "MB", "GB", "TB" };
	double value;
	unsigned unit;

	/* Under a kilobyte, whole bytes. */
	if (bytes < 1000U) {
		(void)snprintf(text, size, "%u B", (unsigned)bytes);
		return;
	}

	/* The largest unit the value is at least one of. */
	value = (double)bytes / 1000.0;
	unit = 0;
	while (value >= 1000.0 && unit + 1U < sizeof(units) / sizeof(units[0])) {
		value /= 1000.0;
		unit++;
	}

	/* One decimal under ten, none above. */
	if (value < 10.0) {
		(void)snprintf(text, size, "%.1f %s", value, units[unit]);
	} else {
		(void)snprintf(text, size, "%.0f %s", value, units[unit]);
	}
}

/*
 * Opens the widgets' input the text fields live in (ws090-p007): every
 * field of every page is libkeiland's kl_field under this one kl_ui.
 * Returns 0, or ENOMEM.
 */
int
se_fields_open(
	struct kl_text *text)
{
	int error;

	/* Made once. */
	if (widgets_ui != NULL)
		return 0;
	widgets_ui = kl_ui_create();
	if (widgets_ui == NULL)
		return ENOMEM;

	/* The text, and the one pixel the keys are carried out on. */
	widgets_text = text;
	error = kl_canvas_init(&widgets_tiny, &widgets_pixel, 1U, 1, 1);
	if (error != 0) {
		se_fields_close();
		return error;
	}

	/* Succeeded: the fields can be drawn. */
	return 0;
}

/*
 * Closes the fields' input.
 */
void
se_fields_close(void)
{
	/* The input and the pixel's canvas, and they are forgotten. */
	kl_ui_destroy(widgets_ui);
	widgets_ui = NULL;
	kl_canvas_release(&widgets_tiny);
}

/*
 * Begins a frame of the fields' input (before the pages are drawn).
 */
void
se_fields_begin(
	uint64_t now_us)
{
	/* Nothing without the input. */
	if (widgets_ui == NULL)
		return;
	kl_ui_begin(widgets_ui, now_us);
}

/*
 * Ends a frame of the fields' input (after the pages are drawn); the keys
 * no field took go.  Returns 1 while a field wants another frame.
 */
int
se_fields_end(
	uint64_t now_us)
{
	struct kl_event event;
	int moving;
	int taken;

	/* Nothing without the input. */
	if (widgets_ui == NULL)
		return 0;

	/* The frame, and what no field took (the pages took their own keys before). */
	moving = kl_ui_end(widgets_ui, now_us);
	for (;;) {
		taken = kl_ui_take(widgets_ui, &event);
		if (!taken)
			break;
	}

	/* Reports whether a field moves. */
	return moving;
}

/*
 * Gives the fields the pointer and an input method's text: the pointer's
 * moves, its leaving and the main button (a click puts a field's caret),
 * and the text an input method sends for the field with the keyboard.
 *
 * A widget the widgets light (a button) is also one of Settings' own
 * regions, whose change the next frame draws (se_ui_hover_settle), so the
 * widgets' own parts are not taken: they would hold every widget crossed
 * since the last frame.
 */
void
se_fields_input(
	const struct se_event *event)
{
	struct kl_window_event text;

	/* Nothing without the input. */
	if (widgets_ui == NULL)
		return;

	/* Each kind of input the fields take. */
	switch (event->type) {
	case SE_EVENT_MOTION:
		(void)kl_ui_pointer_motion(widgets_ui, (double)event->x, (double)event->y);
		return;
	case SE_EVENT_LEAVE:
		/* The lit widget goes dark with the pointer gone. */
		(void)kl_ui_pointer_leave(widgets_ui);
		return;
	case SE_EVENT_BUTTON:
		(void)kl_ui_pointer_motion(widgets_ui, (double)event->x, (double)event->y);
		if (event->button == SE_BUTTON_LEFT)
			(void)kl_ui_pointer_button(widgets_ui, event->pressed, event->time * 1000U);
		return;
	case SE_EVENT_TEXT:
	case SE_EVENT_TEXT_DELETE:
	case SE_EVENT_PREEDIT:
		break;
	default:
		return;
	}

	/* An input method's text, as the window gave it. */
	memset(&text, 0, sizeof(text));
	text.kind = KL_WINDOW_TEXT_COMMIT;
	if (event->type == SE_EVENT_TEXT_DELETE)
		text.kind = KL_WINDOW_TEXT_DELETE;
	else if (event->type == SE_EVENT_PREEDIT)
		text.kind = KL_WINDOW_TEXT_PREEDIT;
	memcpy(text.text, event->text, sizeof(text.text));
	text.text[sizeof(text.text) - 1U] = '\0';
	text.before = event->before;
	text.begin = -1;
	text.end = -1;
	(void)kl_ui_text(widgets_ui, &text);
}

/*
 * Reports the fields' input, for the window's text input (main.c,
 * kl_ui_window_text); NULL before se_fields_open.
 */
struct kl_ui *
se_fields_ui(void)
{
	/* Succeeded: the input, or none. */
	return widgets_ui;
}

/*
 * Types a key press into a text field (it gets the keyboard): a character,
 * Left, Right, Home, End, Backspace, Delete, Ctrl+A.  Enter, Esc and Tab
 * are the pages' own.  The field takes it in the next frame.  Returns 1
 * when the key is the field's.
 */
int
se_field_key(
	struct kl_field *field,
	const struct se_event *event)
{
	struct kl_style style;
	struct kl_rect rect;
	uint32_t character;
	uint32_t id;
	int scroll;
	int editing;

	/* Nothing without the input; Alt and Super are commands, Control's only key is A. */
	if (widgets_ui == NULL)
		return 0;
	if ((event->modifiers & (SE_MOD_ALT | SE_MOD_SUPER)) != 0U)
		return 0;
	if ((event->modifiers & SE_MOD_CTRL) != 0U && event->key != WIDGETS_KEY_A)
		return 0;

	/* The editing keys, or a key that types a character. */
	editing = 0;
	switch (event->key) {
	case KL_KEY_LEFT:
	case KL_KEY_RIGHT:
	case KL_KEY_HOME:
	case KL_KEY_END:
	case KL_KEY_BACKSPACE:
	case KL_KEY_DELETE:
	case WIDGETS_KEY_A:
		editing = 1;
		break;
	default:
		break;
	}

	/* A key that neither edits nor types is the page's. */
	character = kl_key_character(event->key, event->modifiers);
	if (!editing && character == 0U)
		return 0;

	/*
	 * The field has the keyboard, and the key is carried out at once in a
	 * frame of the field alone on one pixel, so that the page reads the
	 * new text before its next key (Enter right after the typing).  The
	 * one pixel would scroll the text out of the field (kl_field keeps the
	 * caret inside its width), so the scroll is kept as it was; the page's
	 * next frame scrolls it at the field's own width (ws090-p025, the User
	 * name that showed only its last letter).
	 */
	scroll = field->scroll;
	id = widgets_field_id(field);
	kl_ui_set_focus(widgets_ui, id, 0U);
	(void)kl_ui_key(widgets_ui, event->key, 1, event->modifiers);
	memset(&style, 0, sizeof(style));
	style.canvas = &widgets_tiny;
	style.text = widgets_text;
	style.theme = kl_theme_default();
	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;
	kl_ui_begin(widgets_ui, event->time * 1000U);
	(void)kl_field(widgets_ui, &style, id, &rect, field, NULL);
	(void)kl_ui_end(widgets_ui, event->time * 1000U);
	field->scroll = scroll;

	/* Succeeded: the field's key. */
	return 1;
}

/*
 * Draws a text field in a rectangle (libkeiland's: white, the accent's
 * edge with the keyboard, the caret, the selection, an input method's
 * composed text) with the keyboard when focused says the page gave it, of
 * a kind (SE_FIELD_*: a secret one as dots and, like a plain one, without
 * an input method), a placeholder while it is empty.  Returns what happened
 * (KL_FIELD_*).
 */
unsigned
se_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	struct kl_field *field,
	const struct kl_rect *rect,
	const char *placeholder,
	unsigned kind,
	int focused)
{
	struct kl_style style;
	uint32_t id;
	int has;
	unsigned changes;

	/* Nothing without the input. */
	if (widgets_ui == NULL)
		return 0U;

	/* Its kind, and the page's keyboard decides the field's. */
	field->secret = 0;
	if (kind == SE_FIELD_SECRET)
		field->secret = 1;
	field->plain = 0;
	if (kind == SE_FIELD_PLAIN)
		field->plain = 1;
	id = widgets_field_id(field);
	has = kl_ui_has_focus(widgets_ui, id, 0U);
	if (focused && !has)
		kl_ui_set_focus(widgets_ui, id, 0U);
	if (!focused && has)
		kl_ui_clear_focus(widgets_ui);

	/* libkeiland's field in Settings' canvas and text. */
	memset(&style, 0, sizeof(style));
	style.canvas = canvas;
	style.text = app->text;
	style.theme = kl_theme_default();
	changes = kl_field(widgets_ui, &style, id, rect, field, placeholder);
	return changes;
}

/* Reports a field's widget ID: where it lives in the program (each field is a part of the application's state, kept for its life). */
static uint32_t
widgets_field_id(
	const struct kl_field *field)
{
	uintptr_t place;

	/* Its address's low bits, never 0. */
	place = (uintptr_t)field;
	return (uint32_t)(place & 0xffffffffU) | 1U;
}

/*
 * Empties a text field, wiping what was typed (a key's text).
 */
void
se_field_clear(
	struct kl_field *field)
{
	volatile char *byte;
	size_t index;

	/* Every byte, through a volatile pointer so that the wipe stays. */
	byte = field->text;
	for (index = 0; index < sizeof(field->text); index++)
		byte[index] = '\0';

	/* Nothing typed, the caret at the start. */
	field->length = 0;
	field->caret = 0;
	field->anchor = 0;
	field->scroll = 0;
}

/* Reports a page's control's widget ID in the widgets' input (even, apart from the fields' odd ones). */
static uint32_t
widgets_control_id(
	int index)
{
	/* The index, doubled, past the low numbers. */
	return 0x40000000U + 2U * (uint32_t)index;
}

/*
 * Fills the style libkeiland's widgets draw Settings' with: its canvas,
 * text and the desktop's colours.  Settings' cards stand on its own ground
 * (the glass's veil, or its light gradient without glass), never on a
 * white panel, so libkeiland draws them as on glass either way.
 */
static void
widgets_style(
	struct se_app *app,
	struct kl_canvas *canvas,
	struct kl_style *style)
{
	/* The canvas (none to measure), the text, the theme, and the glass's look of the cards. */
	memset(style, 0, sizeof(*style));
	style->canvas = canvas;
	style->text = app->text;
	style->theme = kl_theme_default();
	style->glass = 1;
}
