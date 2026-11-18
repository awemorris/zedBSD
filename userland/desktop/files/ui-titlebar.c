/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's titlebar (WS070's CONTROLS presentation,
 * plan/ws070/titlebar-design.md §11): what it shows of the window's state,
 * and what the window does when it is used.
 *
 * The compositor draws the controls (back, forward, home, the path, the search
 * field, the view, the preview and the operations' progress) in the
 * window's floating titlebar, or in the system bar while the window is
 * maximized.  titlebar.c gives it the state made here and queues what it
 * tells the window, which the main loop hands to fm_ui_titlebar.  The text
 * fields are the compositor's: the window hears their text as it is typed and
 * when their editing ends.
 */

#include "files.h"

#include <stdio.h>
#include <string.h>
#include <keiland/keiland.h>

static void titlebar_copy(char *text, size_t size, const char *source);
static void titlebar_activated(struct fm_app *app, uint32_t id, uint32_t detail);
static void titlebar_changed(struct fm_app *app, const struct fm_titlebar_event *event);
static void titlebar_done(struct fm_app *app, const struct fm_titlebar_event *event);

/*
 * Fills what the titlebar shows of the window's state now.
 */
void
fm_ui_titlebar_state(
	struct fm_app *app,
	struct fm_titlebar_state *state)
{
	static struct fm_crumb crumbs[FM_CRUMBS];
	const struct fm_location *location;
	const struct fm_task *task;
	struct fm_tab *tab;
	uint64_t total;
	uint64_t done;
	int index;

	/* Nothing yet; the place shown. */
	memset(state, 0, sizeof(*state));
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;

	/* The history's steps. */
	state->can_back = tab->history_index > 0;
	state->can_forward = tab->history_index + 1 < tab->history_count;

	/* The path's parts. */
	state->part_count = fm_ui_crumbs(app, crumbs, FM_CRUMBS);
	for (index = 0; index < state->part_count; index++)
		titlebar_copy(state->parts[index], sizeof(state->parts[index]), crumbs[index].label);

	/* The folder the path's field starts from: the folder shown, else the home folder. */
	if (location->kind == FM_LOCATION_FOLDER)
		titlebar_copy(state->path, sizeof(state->path), location->path);
	else
		titlebar_copy(state->path, sizeof(state->path), app->home);

	/* The query of a search shown. */
	if (location->kind == FM_LOCATION_SEARCH)
		titlebar_copy(state->query, sizeof(state->query), location->path);

	/* The view and the preview. */
	state->view = app->view;
	state->preview = app->show_preview;

	/* The first operation's share of its bytes and items, while operations run. */
	state->progress = FM_TITLEBAR_NO_PROGRESS;
	if (app->task_count > 0) {
		task = app->tasks[0];
		total = task->bytes_total + task->files_total * 4096U;
		done = task->bytes_done + task->files_done * 4096U;
		state->progress = (int)KL_PROGRESS_UNKNOWN;
		if (total != 0U && done >= total)
			state->progress = 1000;
		else if (total != 0U)
			state->progress = (int)(done * 1000U / total);
	}

	/* The text control the window last asked the keyboard for. */
	state->focus = app->control_focus;
	state->focus_serial = app->control_focus_serial;

	/* The path's field's suggestions, the last list made (ws127-p010). */
	state->suggest_serial = app->suggest_serial;
	state->suggest_count = app->suggest_count;
	for (index = 0; index < app->suggest_count; index++) {
		titlebar_copy(state->suggest_labels[index], sizeof(state->suggest_labels[index]), app->suggest_labels[index]);
		titlebar_copy(state->suggest_texts[index], sizeof(state->suggest_texts[index]), app->suggest_texts[index]);
	}
}

/*
 * Does what the titlebar tells the window: a control chosen, a text typed,
 * or a text field's editing ended.
 */
void
fm_ui_titlebar(
	struct fm_app *app,
	const struct fm_titlebar_event *event)
{
	/* The log line the tests read. */
	fm_log("TITLEBAR kind=%u id=%u detail=%u text=%s", event->kind, event->id, event->detail, event->text);

	/* Each kind. */
	switch (event->kind) {
	case FM_TITLEBAR_ACTIVATED:
		titlebar_activated(app, event->id, event->detail);
		break;
	case FM_TITLEBAR_CHANGED:
		titlebar_changed(app, event);
		break;
	case FM_TITLEBAR_DONE:
		titlebar_done(app, event);
		break;
	default:
		break;
	}

	/* A new frame shows what it did. */
	app->dirty = 1;
}

/*
 * Copies a text for the titlebar, cut on a character's boundary when it is
 * longer than the room.
 */
static void
titlebar_copy(
	char *text,
	size_t size,
	const char *source)
{
	size_t length;

	/* As much as fits. */
	length = strlen(source);
	if (length > size - 1U) {
		length = size - 1U;

		/* Back to the start of the character cut (UTF-8's continuation bytes are 10xxxxxx). */
		while (length > 0U && ((unsigned char)source[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The text and its end. */
	memcpy(text, source, length);
	text[length] = '\0';
}

/* Carries out a control chosen: detail is the path's part for the path. */
static void
titlebar_activated(
	struct fm_app *app,
	uint32_t id,
	uint32_t detail)
{
	static struct fm_crumb crumbs[FM_CRUMBS];
	struct fm_location location;
	int count;

	/* What each control does. */
	switch (id) {
	case FM_CONTROL_BACK:
		fm_ui_back(app);
		break;
	case FM_CONTROL_FORWARD:
		fm_ui_forward(app);
		break;
	case FM_CONTROL_HOME:
		/* The home folder itself (ws127-p011; Today is the dashboard's place of its own). */
		memset(&location, 0, sizeof(location));
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", app->home);
		fm_ui_go(app, &location);
		break;
	case FM_CONTROL_PATH:
		/*
		 * A part before the last goes there; the last, the place shown, makes
		 * the path a field to type a path in, all of it selected (ws127-p010,
		 * as Ctrl+L does).
		 */
		count = fm_ui_crumbs(app, crumbs, FM_CRUMBS);
		if ((int)detail < count - 1) {
			fm_ui_go(app, &crumbs[detail].location);
		} else {
			fm_input_location(app);
		}
		break;
	case FM_CONTROL_ICONS:
		app->view = FM_VIEW_ICONS;
		fm_ui_tab(app)->scroll = 0;
		break;
	case FM_CONTROL_LIST:
		app->view = FM_VIEW_LIST;
		fm_ui_tab(app)->scroll = 0;
		break;
	case FM_CONTROL_PREVIEW:
		app->show_preview = !app->show_preview;
		break;
	case FM_CONTROL_PROGRESS:
		app->show_tasks = !app->show_tasks;
		break;
	default:
		break;
	}
}

/* Takes a text control's text as it is typed: the search's searches a moment later. */
static void
titlebar_changed(
	struct fm_app *app,
	const struct fm_titlebar_event *event)
{
	/* The search field: as typed in the window's own field (fm_search_tick searches). */
	if (event->id == FM_CONTROL_SEARCH) {
		fm_field_set(&app->search_field, event->text);
		app->focus = FM_FOCUS_SEARCH;
		app->search_typed_at = app->now;
		if (app->search_typed_at == 0U)
			app->search_typed_at = 1;
		return;
	}

	/* The path's field: kept for its Enter, and its suggestions made a second after the typing rests (ws127-p010). */
	if (event->id == FM_CONTROL_PATH) {
		fm_field_set(&app->location, event->text);
		app->focus = FM_FOCUS_LOCATION;
		app->location_typed_at = app->now;
		if (app->location_typed_at == 0U)
			app->location_typed_at = 1;
	}
}

/* Ends a text control's editing: submitted, cancelled (Esc) or left. */
static void
titlebar_done(
	struct fm_app *app,
	const struct fm_titlebar_event *event)
{
	/* The search: Enter gives the keyboard to the results, Esc ends the search, leaving keeps it. */
	if (event->id == FM_CONTROL_SEARCH) {
		if (event->detail == KL_TEXT_CANCELLED) {
			fm_search_cancel(app);
			return;
		}

		/* Enter or leaving: the typing ends, the search goes on. */
		app->focus = FM_FOCUS_CONTENT;
		return;
	}

	/* The path: Enter goes to the folder typed; otherwise nothing changes (no suggestions are due any more). */
	if (event->id == FM_CONTROL_PATH) {
		app->focus = FM_FOCUS_CONTENT;
		app->location_typed_at = 0;
		if (event->detail != KL_TEXT_SUBMITTED)
			return;
		fm_field_set(&app->location, event->text);
		fm_input_location_go(app);
	}
}
