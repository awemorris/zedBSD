/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The places of files that are not one folder: the search
 * results (spec §7) and the recent files (spec §19), and the search field
 * that leads to the first.
 *
 * Typing in the search field starts a search a moment after the last key
 * (FM_SEARCH_DELAY_MS), in a place of its own in the tab's history; typing
 * more changes that place instead of adding another, and Esc goes back to
 * where the search started.
 */

#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <keiland/keiland.h>

/* How long after the last key a search starts, and how long it may run a round, in milliseconds. */
#define SEARCH_DELAY_MS		150U
#define SEARCH_BUDGET_MS	8U

/* How many recent files Recents shows. */
#define SEARCH_RECENTS		64U

/* The name of this program in the recent list. */
#define SEARCH_APPLICATION	"files"

/*
 * The recent list read for Recents.  It is large (a path a entry), so it
 * lives here rather than on the stack; it is filled each time Recents is
 * read and used only then.
 */
static struct kl_recent_item search_recents[SEARCH_RECENTS];

static void search_finish(struct fm_app *app, struct fm_tab *tab);
static void search_home_text(struct fm_app *app, const char *folder, char *text, size_t size);

/*
 * Gives the keyboard to the search field (Ctrl+F), with the query shown
 * now selected: the compositor is asked to give the titlebar's search field the
 * keyboard (fm_titlebar_state).
 */
void
fm_search_focus(
	struct fm_app *app)
{
	struct fm_tab *tab;
	const struct fm_location *location;

	/* The field shows the query of a search being shown, else nothing. */
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;
	if (location->kind == FM_LOCATION_SEARCH)
		fm_field_set(&app->search_field, location->path);
	else
		fm_field_set(&app->search_field, "");
	app->focus = FM_FOCUS_SEARCH;
	app->control_focus = FM_CONTROL_SEARCH;
	app->control_focus_serial++;
	app->dirty = 1;
}

/*
 * Handles a key while the search field has the focus: typing waits a
 * moment and searches, Enter gives the keyboard to the results, Esc ends
 * the search.
 */
void
fm_search_key(
	struct fm_app *app,
	const struct fm_event *event)
{
	unsigned result;

	/* The field's answer. */
	result = fm_field_key(&app->search_field, event->key, event->modifiers);

	/* A change searches a moment later. */
	if (result == FM_FIELD_CHANGED) {
		app->search_typed_at = app->now;
		return;
	}

	/* Enter: the keyboard goes to the results. */
	if (result == FM_FIELD_ENTER) {
		app->focus = FM_FOCUS_CONTENT;
		return;
	}

	/* Esc: the search ends. */
	if (result == FM_FIELD_CANCEL)
		fm_search_cancel(app);
}

/*
 * Ends the search (Esc in the search field): the field is emptied and the
 * tab goes back from the search.
 */
void
fm_search_cancel(
	struct fm_app *app)
{
	struct fm_tab *tab;

	/* The field, the typing and the walk. */
	fm_field_set(&app->search_field, "");
	app->focus = FM_FOCUS_CONTENT;
	app->search_typed_at = 0;
	fm_search_stop(&app->search);

	/* The place before the search. */
	tab = fm_ui_tab(app);
	if (tab->history[tab->history_index].location.kind == FM_LOCATION_SEARCH)
		fm_ui_back(app);
	app->dirty = 1;
}

/*
 * Lets time pass for the search: a query typed a moment ago is searched,
 * and a search in progress walks on.
 */
void
fm_search_tick(
	struct fm_app *app)
{
	struct fm_location location;
	struct fm_visit *visit;
	struct fm_tab *tab;
	const char *folder;
	int more;

	/* The query typed, once the typing pauses. */
	tab = fm_ui_tab(app);
	visit = &tab->history[tab->history_index];
	if (app->search_typed_at != 0U && app->now >= app->search_typed_at + SEARCH_DELAY_MS) {
		app->search_typed_at = 0;

		/* An emptied field ends the search. */
		if (app->search_field.length == 0U) {
			fm_search_stop(&app->search);
			if (visit->location.kind == FM_LOCATION_SEARCH)
				fm_ui_back(app);
			return;
		}

		/* A search shown changes its query; otherwise one starts from the folder shown. */
		if (visit->location.kind == FM_LOCATION_SEARCH) {
			snprintf(visit->location.path, sizeof(visit->location.path), "%s", app->search_field.text);
			fm_ui_reload(app, tab);
		} else {
			folder = fm_current_folder(app);
			if (folder == NULL)
				folder = app->home;
			snprintf(app->search_folder, sizeof(app->search_folder), "%s", folder);
			memset(&location, 0, sizeof(location));
			location.kind = FM_LOCATION_SEARCH;
			snprintf(location.path, sizeof(location.path), "%s", app->search_field.text);
			fm_ui_go(app, &location);
			app->focus = FM_FOCUS_SEARCH;
		}

		/* A new frame shows it. */
		app->dirty = 1;
	}

	/* A search in progress walks on. */
	if (app->search.active == 0)
		return;
	more = fm_search_step(&app->search, &tab->listing, SEARCH_BUDGET_MS);
	app->dirty = 1;
	if (more == 0)
		search_finish(app, tab);
}

/*
 * Changes where the search looks (the scope's chips) and searches again.
 */
void
fm_search_scope(
	struct fm_app *app,
	unsigned scope)
{
	/* The scope, and the search again. */
	app->search_scope = scope;
	fm_ui_reload(app, fm_ui_tab(app));
	app->dirty = 1;
}

/*
 * Reads the items of a place that is not one folder into the tab's
 * listing: the search's (started, results come in the ticks), or the
 * recent files.
 */
void
fm_search_load(
	struct fm_app *app,
	struct fm_tab *tab)
{
	const struct fm_location *location;
	struct fm_entry *entry;
	struct stat status;
	const char *base;
	char folder[KL_RECENT_PATH_MAX];
	char *slash;
	size_t count;
	size_t index;
	int folder_entry;
	int keep;
	int error;

	/* The place. */
	location = &tab->history[tab->history_index].location;

	/* A search: the walk starts in the scope's folder. */
	if (location->kind == FM_LOCATION_SEARCH) {
		base = app->search_folder;
		if (app->search_scope == FM_SCOPE_HOME)
			base = app->home;
		else if (app->search_scope == FM_SCOPE_COMPUTER)
			base = "/";
		if (base[0] == '\0')
			base = app->home;
		fm_search_start(&app->search, location->path, base, app->show_hidden);
		fm_log("SEARCH start query=%s base=%s", location->path, base);
		return;
	}

	/* The recent files, newest first (folders and files that are gone are left out); whether the list is stopped too (ws177-p008). */
	if (location->kind == FM_LOCATION_RECENTS) {
		keep = 1;
		error = kl_recent_keep(&keep);
		app->recents_off = 0;
		if (error == 0 && keep == 0)
			app->recents_off = 1;
		fm_log("RECENTS kept=%d", keep);
		error = kl_recent_list(search_recents, SEARCH_RECENTS, &count);
		for (index = 0; error == 0 && index < count; index++) {
			error = stat(search_recents[index].path, &status);
			folder_entry = 0;
			if (error == 0)
				folder_entry = S_ISDIR(status.st_mode);
			if (error != 0 || folder_entry != 0) {
				error = 0;
				continue;
			}

			/* The folder and the name of the file. */
			snprintf(folder, sizeof(folder), "%.4095s", search_recents[index].path);
			slash = strrchr(folder, '/');
			if (slash == NULL)
				continue;
			*slash = '\0';
			entry = fm_dir_add(&tab->listing, folder, slash + 1);
			if (entry == NULL)
				break;
			search_home_text(app, folder, folder, sizeof(folder));
			entry->detail = strdup(folder);
			entry->extra_time = (time_t)search_recents[index].time;
		}

		/* Recents are read. */
		return;
	}
}

/*
 * Adds a file the user opened to the desktop's recent list.
 */
void
fm_recent_add(
	const char *path)
{
	int error;

	/* The list keeps it; a failure only costs the entry. */
	error = kl_recent_add(path, SEARCH_APPLICATION);
	if (error != 0)
		fm_log("RECENT add failed path=%s error=%d", path, error);
}

/* Finishes a search: the results sorted as the window sorts, and the outcome logged. */
static void
search_finish(
	struct fm_app *app,
	struct fm_tab *tab)
{
	char folder[FM_PATH_MAX];
	size_t index;

	/* Each result's folder shown from the home folder (~/...). */
	for (index = 0; index < tab->listing.count; index++) {
		if (tab->listing.entries[index].detail == NULL)
			continue;
		search_home_text(app, tab->listing.entries[index].detail, folder, sizeof(folder));
		free(tab->listing.entries[index].detail);
		tab->listing.entries[index].detail = strdup(folder);
	}

	/* The results in the window's order. */
	fm_dir_sort(&tab->listing, app->sort, app->sort_reverse);
	fm_log("SEARCH done query=%s results=%lu visited=%lu", tab->history[tab->history_index].location.path, (unsigned long)tab->listing.count, app->search.visited);
}

/* Writes a folder as the window shows where an item is: under the home folder as ~/..., else as it is. */
static void
search_home_text(
	struct fm_app *app,
	const char *folder,
	char *text,
	size_t size)
{
	char copy[FM_PATH_MAX];
	size_t length;
	int match;

	/* A copy, since the text may be the folder itself. */
	snprintf(copy, sizeof(copy), "%s", folder);

	/* Under the home folder: ~ and the rest. */
	length = strlen(app->home);
	match = strncmp(copy, app->home, length);
	if (match == 0 && length > 1U && (copy[length] == '/' || copy[length] == '\0')) {
		snprintf(text, size, "~%s", copy + length);
		return;
	}

	/* Anywhere else: the path. */
	snprintf(text, size, "%s", copy);
}
