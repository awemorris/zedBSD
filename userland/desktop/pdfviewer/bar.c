/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The find field inside PDF Viewer's window (ws177-p043): where the
 * compositor shows no titlebar, Ctrl+F (Edit > Find) opens a panel at the
 * top right of the pages with libkeiland's field in it, drawn by kl_ui over
 * the frame after the viewer's own drawing.  The field is libkeiland's own
 * (an input method composes in it, the on-screen keyboard types in it, and
 * it has its undo and its clipboard).  As its text changes the viewer looks
 * for it; Enter goes to the next place, Shift+Enter to the one before, Esc
 * closes the panel; the place's number ("Match 3 of 17") stands at the
 * field's right.  A press outside the panel leaves the keyboard to the
 * pages; one on the field gives it back.
 *
 * The panel's state and place are the viewer's (find.c); this file is
 * the part that speaks to libkeiland.
 */

#include "window.h"

#include <stdio.h>
#include <string.h>

/* The field's id among the panel's widgets. */
#define BAR_FIELD		1U

/* The evdev codes of Enter and the keypad's. */
#define BAR_KEY_ENTER		28U
#define BAR_KEY_KP_ENTER	96U

static int bar_inside(const struct pv_app *app, double x, double y);
static int bar_canvas(struct pv_bar *bar, uint32_t *pixels, size_t stride, int width, int height);

/*
 * Makes the panel's widgets and opens its font (the viewer's, path), once.
 * Returns 0, or ENOMEM; a font that cannot be opened leaves the field
 * without words drawn, as the viewer's own text is.
 */
int
pv_bar_open(
	struct pv_bar *bar,
	const char *path)
{
	int error;

	/* Nothing yet. */
	memset(bar, 0, sizeof(*bar));

	/* The widgets. */
	bar->ui = kl_ui_create();
	if (bar->ui == NULL)
		return ENOMEM;

	/* The font, the viewer's. */
	error = kl_text_open(&bar->text, path, NULL);
	if (error != 0) {
		pv_log("BAR font-missing path=%s error=%d", path, error);
		return 0;
	}

	/* Succeeded: the panel can be shown. */
	bar->text_ready = 1;
	return 0;
}

/*
 * Frees the panel's widgets, its font and its canvas.
 */
void
pv_bar_close(
	struct pv_bar *bar)
{
	/* Each part that was made. */
	if (bar->canvas_ready)
		kl_canvas_release(&bar->canvas);
	if (bar->text_ready)
		kl_text_close(&bar->text);
	if (bar->ui != NULL)
		kl_ui_destroy(bar->ui);
	memset(bar, 0, sizeof(*bar));
}

/*
 * Gives the field the keyboard, with the words looked for last (the panel
 * just opened, or asked for again).
 */
void
pv_bar_focus(
	struct pv_bar *bar,
	struct pv_app *app)
{
	/* The widgets. */
	if (bar->ui == NULL)
		return;

	/* The words, all of them, and the keyboard. */
	kl_field_set(&bar->field, app->find_query);
	kl_ui_set_focus(bar->ui, BAR_FIELD, 0U);
	bar->focused = 1;
	app->dirty = 1;
}

/*
 * Takes one input of the window for the panel while it is open: the keys
 * and the input method's text while the field has the keyboard (Shift+Enter
 * goes back; the rest is the field's), the pointer and the fingers over
 * the panel.  A press elsewhere takes the keyboard from the field and is
 * the viewer's.  Returns 1 when the panel took the input.
 */
int
pv_bar_input(
	struct pv_bar *bar,
	struct pv_app *app,
	const struct kl_window_event *event)
{
	int inside;
	int shift;

	/* Only while it is open. */
	if (!app->bar_open || bar->ui == NULL)
		return 0;

	/* The keys and the text while the field has the keyboard. */
	switch (event->kind) {
	case KL_WINDOW_KEY:
		if (!bar->focused)
			return 0;

		/* Shift+Enter: the place before. */
		shift = 0;
		if ((event->modifiers & KL_MOD_SHIFT) != 0U)
			shift = 1;
		if (event->pressed &&
		    shift &&
		    (event->code == BAR_KEY_ENTER ||
		     event->code == BAR_KEY_KP_ENTER)) {
			pv_find_next(app, -1);
			return 1;
		}

		/* Any other: the field's. */
		(void)kl_ui_window_input(bar->ui, event);
		app->dirty = 1;
		return 1;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		if (!bar->focused)
			return 0;

		/* The input method's text, the field's. */
		(void)kl_ui_window_input(bar->ui, event);
		app->dirty = 1;
		return 1;
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_TOUCH_UP:
	case KL_WINDOW_TOUCH_CANCEL:
		if (!bar->touching)
			return 0;
		if (event->kind != KL_WINDOW_TOUCH_CANCEL && event->id != bar->touch_id)
			return 0;

		/* The finger that went down on the panel stays the panel's until it lifts. */
		(void)kl_ui_window_input(bar->ui, event);
		if (event->kind != KL_WINDOW_TOUCH_MOTION)
			bar->touching = 0;
		app->dirty = 1;
		return 1;
	case KL_WINDOW_MOTION:
	case KL_WINDOW_BUTTON:
	case KL_WINDOW_TOUCH_DOWN:
		break;
	default:
		return 0;
	}

	/* The pointer or a finger over the panel: the field's (a press there gives it the keyboard). */
	inside = bar_inside(app, event->x, event->y);
	if (inside) {
		(void)kl_ui_window_input(bar->ui, event);
		if (event->kind == KL_WINDOW_TOUCH_DOWN) {
			bar->touching = 1;
			bar->touch_id = event->id;
		}

		/* A press there gives the field the keyboard. */
		if (event->kind == KL_WINDOW_TOUCH_DOWN || (event->kind == KL_WINDOW_BUTTON && event->pressed)) {
			kl_ui_set_focus(bar->ui, BAR_FIELD, 0U);
			bar->focused = 1;
		}

		/* Drawn again. */
		app->dirty = 1;
		return 1;
	}

	/* A press elsewhere takes the keyboard from the field; the press is the viewer's. */
	if (event->kind == KL_WINDOW_TOUCH_DOWN || (event->kind == KL_WINDOW_BUTTON && event->pressed)) {
		if (bar->focused) {
			kl_ui_clear_focus(bar->ui);
			bar->focused = 0;
			app->dirty = 1;
		}
	}

	/* Not the panel's. */
	return 0;
}

/*
 * Draws the open panel over the frame (pixels, premultiplied, the
 * window's size) and takes what its field reported: words changed are
 * looked for, Enter goes on, Esc closes it.  Then the window's text input
 * follows the field's keyboard (with its caret's place).  Returns 1 when
 * the panel wants the next frame drawn by itself.
 */
int
pv_bar_draw(
	struct pv_bar *bar,
	struct pv_app *app,
	struct kl_window *window,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height,
	uint64_t now_us)
{
	struct pv_bar_place place;
	struct kl_style style;
	struct kl_event event;
	struct kl_rect rect;
	const struct kl_theme *theme;
	char status[96];
	unsigned reported;
	int baseline;
	int again;
	int taken;
	int error;

	/* Closed: the field has no keyboard and asks for no text input. */
	if (!app->bar_open || bar->ui == NULL || !bar->text_ready) {
		if (bar->ui != NULL && bar->focused) {
			kl_ui_clear_focus(bar->ui);
			bar->focused = 0;
			kl_ui_window_text(bar->ui, window);
		}

		/* Nothing drawn. */
		return 0;
	}

	/* The canvas over the frame. */
	error = bar_canvas(bar, pixels, stride, width, height);
	if (error != 0)
		return 0;

	/* The style: the canvas, the font and the desktop's theme. */
	theme = kl_theme_default();
	memset(&style, 0, sizeof(style));
	style.canvas = &bar->canvas;
	style.text = &bar->text;
	style.theme = theme;
	style.glass = 0;

	/* The panel. */
	pv_find_bar_place(app, &place);
	kl_canvas_round(&bar->canvas, (float)place.x, (float)place.y, (float)place.width, (float)place.height, theme->card_radius, theme->panel);
	kl_canvas_round_border(&bar->canvas, (float)place.x, (float)place.y, (float)place.width, (float)place.height, theme->card_radius, 1.0f,
			       theme->panel_edge);

	/* The field. */
	kl_ui_begin(bar->ui, now_us);
	rect.x = place.field_x;
	rect.y = place.field_y;
	rect.width = place.field_width;
	rect.height = place.field_height;
	reported = kl_field(bar->ui, &style, BAR_FIELD, &rect, &bar->field, "Find");
	again = kl_ui_end(bar->ui, now_us);

	/* The place's number at its right. */
	pv_find_status(app, status, sizeof(status));
	if (status[0] != '\0') {
		baseline = kl_text_center(theme->text_small, place.y, place.height);
		(void)kl_text_draw_fit(&bar->text, &bar->canvas, place.status_x, baseline, status, theme->text_small, 0, PV_BAR_STATUS,
				       theme->text_secondary);
	}

	/* What no part took is nothing. */
	for (;;) {
		taken = kl_ui_take(bar->ui, &event);
		if (taken == 0)
			break;
	}

	/* The words changed: looked for. */
	if ((reported & KL_FIELD_CHANGED) != 0U)
		pv_find_text(app, bar->field.text);

	/* Enter: the next place, the field keeping the keyboard. */
	if ((reported & KL_FIELD_SUBMITTED) != 0U) {
		pv_find_next(app, 1);
		kl_ui_set_focus(bar->ui, BAR_FIELD, 0U);
		bar->focused = 1;
	}

	/* Esc: the panel closes. */
	if ((reported & KL_FIELD_CANCELLED) != 0U)
		pv_find_bar_close(app);

	/* The text input while the field has the keyboard, at its caret. */
	kl_ui_window_text(bar->ui, window);

	/* Whether it moves by itself. */
	return again;
}

/* Tells whether a point of the window is on the open panel. */
static int
bar_inside(
	const struct pv_app *app,
	double x,
	double y)
{
	struct pv_bar_place place;

	/* The panel's rectangle. */
	pv_find_bar_place(app, &place);
	if (x < (double)place.x || x >= (double)(place.x + place.width))
		return 0;
	if (y < (double)place.y || y >= (double)(place.y + place.height))
		return 0;

	/* On it. */
	return 1;
}

/* Keeps a canvas over the frame's pixels (made again when they change); returns 0 or the failure. */
static int
bar_canvas(
	struct pv_bar *bar,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	int error;

	/* The same pixels: the canvas stands. */
	if (bar->canvas_ready && bar->canvas.pixels == pixels && bar->canvas.stride == stride && bar->canvas.width == width &&
	    bar->canvas.height == height)
		return 0;

	/* A new one. */
	if (bar->canvas_ready)
		kl_canvas_release(&bar->canvas);
	bar->canvas_ready = 0;
	error = kl_canvas_init(&bar->canvas, pixels, stride, width, height);
	if (error != 0)
		return error;

	/* Succeeded. */
	bar->canvas_ready = 1;
	return 0;
}
