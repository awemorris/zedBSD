/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libkeiland's widgets in files (ws090-p023): one kl_ui for the window's
 * (or the desktop's) frame, which the sidebar's places, the buttons, the
 * field of the name being changed (rename.c) and the other parts drawn
 * with libkeiland record themselves in.  files keeps its own regions
 * (fm_ui_hit) for what a press does; the widgets' input follows the
 * pointer so that they light under it.
 */

#include "files.h"

#include <errno.h>
#include <string.h>


/*
 * Begins a frame of the widgets' input (made the first time); 0, or
 * ENOMEM when it cannot be made (the widgets are then not drawn lit).
 */
int
fm_widgets_begin(
	struct fm_app *app)
{
	/* Made the first time. */
	if (app->ui == NULL) {
		app->ui = kl_ui_create();
		if (app->ui == NULL)
			return ENOMEM;
	}

	/* Succeeded: the frame begins. */
	kl_ui_begin(app->ui, app->now * 1000U);
	return 0;
}

/*
 * Ends the frame of the widgets' input; the keys no widget took go.
 */
void
fm_widgets_end(
	struct fm_app *app)
{
	struct kl_event event;
	int taken;

	/* Nothing without the input. */
	if (app->ui == NULL)
		return;

	/* The frame, and what no widget took (files takes its own keys). */
	(void)kl_ui_end(app->ui, app->now * 1000U);
	for (;;) {
		taken = kl_ui_take(app->ui, &event);
		if (!taken)
			break;
	}
}

/*
 * Gives the widgets' input where the pointer is and its main button.
 */
void
fm_widgets_input(
	struct fm_app *app,
	const struct fm_event *event)
{
	/* Nothing without the input. */
	if (app->ui == NULL)
		return;

	/*
	 * The pointer's moves, its leaving, and the main button where it is.
	 * Every widget the widgets light (a place, a button) is also one of
	 * files' own regions, whose change the next frame draws
	 * (fm_input_hover_settle), so the widgets' own parts are not taken:
	 * they would hold every widget crossed since the last frame.
	 */
	switch (event->type) {
	case FM_EVENT_MOTION:
		(void)kl_ui_pointer_motion(app->ui, (double)event->x, (double)event->y);
		break;
	case FM_EVENT_LEAVE:
		(void)kl_ui_pointer_leave(app->ui);
		break;
	case FM_EVENT_BUTTON:
		(void)kl_ui_pointer_motion(app->ui, (double)event->x, (double)event->y);
		if (event->button == FM_BUTTON_LEFT)
			(void)kl_ui_pointer_button(app->ui, event->pressed, event->time * 1000U);
		break;
	default:
		break;
	}
}

/*
 * Lets go of the widgets' input.
 */
void
fm_widgets_release(
	struct fm_app *app)
{
	/* The input, and it is forgotten. */
	kl_ui_destroy(app->ui);
	app->ui = NULL;
}

/*
 * Fills the style libkeiland's widgets draw files' with: the canvas, the
 * text, the desktop's colours, and whether the window is glass.
 */
void
fm_style(
	struct fm_app *app,
	struct kl_canvas *canvas,
	struct kl_style *style)
{
	/* The four. */
	memset(style, 0, sizeof(*style));
	style->canvas = canvas;
	style->text = app->text;
	style->theme = kl_theme_default();
	style->glass = app->glass;
}

/*
 * Reports how wide libkeiland's button with a label is.
 */
int
fm_button_width(
	struct fm_app *app,
	const char *label)
{
	struct kl_style style;
	int width;

	/* libkeiland's measure. */
	fm_style(app, NULL, &style);
	width = kl_button_width(&style, label);
	return width;
}

/*
 * Draws libkeiland's button in a rectangle (flags KL_BUTTON_*) as one of
 * files' buttons (index, FM_HIT_BUTTON): files carries out its click; a
 * disabled one is not clickable.
 */
void
fm_button(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *rect,
	const char *label,
	int index,
	unsigned flags)
{
	struct kl_style style;

	/* The button in the frame's widgets' input (unlit without it). */
	fm_style(app, canvas, &style);
	(void)kl_button(app->ui, &style, FM_WIDGET_BUTTON + (uint32_t)index, rect, label, flags);

	/* An enabled one can be clicked. */
	if ((flags & KL_BUTTON_DISABLED) == 0U)
		fm_ui_hit(app, rect, FM_HIT_BUTTON, index);
}

/*
 * Draws libkeiland's button that is a picture alone in a rectangle (an
 * id among the widgets and its index, the icon pixels square, flags
 * KL_BUTTON_*), lit under the pointer (unlit without the widgets' input).
 * files keeps the region its click is carried out by (fm_ui_hit).
 */
void
fm_icon_button(
	struct fm_app *app,
	struct kl_canvas *canvas,
	uint32_t id,
	int index,
	const struct kl_rect *rect,
	enum kl_icon icon,
	int pixels,
	unsigned flags)
{
	struct kl_style style;

	/* The button in the frame's widgets' input. */
	fm_style(app, canvas, &style);
	(void)kl_icon_button(app->ui, &style, id, (uint32_t)index, rect, icon, pixels, flags);
}
