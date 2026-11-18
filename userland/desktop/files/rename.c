/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The field of the name being changed (ws090-p010): libkeiland's text
 * field (kl_field) in files' widgets' input (ui-widgets.c), drawn where the
 * name was in the grid, the list or on the desktop.  While a name is being
 * changed the window's keys and an input method's text go to it
 * (fm_rename_input; the pointer goes to the widgets' input always); Enter and Esc come back from the frame
 * (fm_rename_take) and end the change.  The text input is asked for while
 * the field has the keyboard (kl_ui_window_text, main.c).
 */

#include "files.h"

#include <errno.h>
#include <string.h>

/* The field's widget ID in its own input. */
#define RENAME_ID		1U

/*
 * Starts the field with a name, the part before its extension selected
 * (the whole name when stem is the name's length), with the keyboard.
 * Returns 0, or ENOMEM when its input cannot be made.
 */
int
fm_rename_start(
	struct fm_app *app,
	const char *name,
	size_t stem)
{
	/* The widgets' input, made the first time. */
	if (app->ui == NULL) {
		app->ui = kl_ui_create();
		if (app->ui == NULL)
			return ENOMEM;
	}

	/* The name, its stem selected; not drawn yet. */
	kl_field_set(&app->rename, name);
	app->rename_shown = 0;
	if (stem > app->rename.length)
		stem = app->rename.length;
	app->rename.anchor = 0;
	app->rename.caret = stem;
	app->rename_flags = 0U;

	/* Succeeded: the field has the keyboard. */
	kl_ui_set_focus(app->ui, RENAME_ID, 0U);
	return 0;
}

/*
 * Selects the whole name being changed (Edit > Select All).
 */
void
fm_rename_select_all(
	struct fm_app *app)
{
	/* From its start to its end. */
	app->rename.anchor = 0;
	app->rename.caret = app->rename.length;
	app->dirty = 1;
}

/*
 * Gives an input of the window to the field while a name is being
 * changed: a key, the pointer, or an input method's text.  Returns 1 when
 * the field took it.
 */
int
fm_rename_input(
	struct fm_app *app,
	const struct fm_event *event)
{
	struct kl_window_event text;

	/* Only while a name is being changed. */
	if (app->focus != FM_FOCUS_RENAME || app->ui == NULL)
		return 0;
	app->dirty = 1;

	/* Each kind of input the field takes. */
	switch (event->type) {
	case FM_EVENT_KEY:
		(void)kl_ui_key(app->ui, event->key, event->pressed, event->modifiers);
		return 1;
	case FM_EVENT_TEXT:
	case FM_EVENT_TEXT_DELETE:
	case FM_EVENT_PREEDIT:
		break;
	default:
		return 0;
	}

	/* An input method's text, as the window gave it. */
	memset(&text, 0, sizeof(text));
	text.kind = KL_WINDOW_TEXT_COMMIT;
	if (event->type == FM_EVENT_TEXT_DELETE)
		text.kind = KL_WINDOW_TEXT_DELETE;
	else if (event->type == FM_EVENT_PREEDIT)
		text.kind = KL_WINDOW_TEXT_PREEDIT;
	memcpy(text.text, event->text, sizeof(text.text));
	text.text[sizeof(text.text) - 1U] = '\0';
	text.before = event->before;
	text.begin = -1;
	text.end = -1;

	/* Succeeded: the field takes it in its next frame. */
	(void)kl_ui_text(app->ui, &text);
	return 1;
}

/*
 * Draws the field in a rectangle (where the name was) and takes its input;
 * what happened (KL_FIELD_SUBMITTED, KL_FIELD_CANCELLED) waits for
 * fm_rename_take.
 */
void
fm_rename_draw(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	struct kl_style style;
	unsigned flags;

	/* Nothing without its input. */
	if (app->ui == NULL)
		return;

	/* libkeiland's field in the window's canvas and text, the desktop's colours. */
	memset(&style, 0, sizeof(style));
	style.canvas = canvas;
	style.text = app->text;
	style.theme = kl_theme_default();

	/* In the frame's widgets' input (fm_widgets_begin). */
	flags = kl_field(app->ui, &style, RENAME_ID, rect, &app->rename, NULL);

	/* What happened, for after the frame, and where it is (a press there stays the field's). */
	app->rename_flags |= flags;
	app->rename_rect = *rect;
	app->rename_shown = 1;
}

/*
 * Tells whether a place (window pixels) is on the field as last drawn.
 */
int
fm_rename_hit(
	const struct fm_app *app,
	int x,
	int y)
{
	/* No field drawn. */
	if (app->focus != FM_FOCUS_RENAME || !app->rename_shown)
		return 0;

	/* Outside its rectangle across or down. */
	if (x < app->rename_rect.x || x >= app->rename_rect.x + app->rename_rect.width)
		return 0;
	if (y < app->rename_rect.y || y >= app->rename_rect.y + app->rename_rect.height)
		return 0;

	/* Succeeded: on it. */
	return 1;
}

/*
 * Ends the change when the field was submitted (Enter: kept) or cancelled
 * (Esc) in the frame just drawn; on the desktop through
 * fm_desktop_rename_end.
 */
void
fm_rename_take(
	struct fm_app *app)
{
	unsigned flags;
	int commit;

	/* What the frame left, and nothing more. */
	flags = app->rename_flags;
	app->rename_flags = 0U;
	if (app->focus != FM_FOCUS_RENAME)
		return;

	/* Enter keeps the name, Esc gives it up; anything else goes on. */
	if ((flags & KL_FIELD_SUBMITTED) != 0U)
		commit = 1;
	else if ((flags & KL_FIELD_CANCELLED) != 0U)
		commit = 0;
	else
		return;

	/* The change ends, on the desktop or in the window. */
	if (app->desktop) {
		fm_desktop_rename_end(app, commit);
		return;
	}
	fm_action_rename_end(app, commit);
}
