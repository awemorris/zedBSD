/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the menus of files do (spec §36, §37): the action each
 * item reports, carried out on the window, and the state the menus show
 * (which items do something now, which are checked, the names of the ways
 * to open the selection).
 *
 * The compositor draws the menus and sends the actions (menu.c); this part knows
 * nothing of Wayland, so the host's tests drive it directly.  The keys
 * that the menus' shortcuts name are also handled by ui-input.c, for a
 * compositor without menus: the compositor takes such a key for the menu only
 * while its item is enabled.
 */

#include "files.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void menu_go_folder(struct fm_app *app, const char *name);
static void menu_go_kind(struct fm_app *app, unsigned kind, const char *path);
static void menu_openers(struct fm_app *app, struct fm_menu_state *state);
static int menu_in_field(struct fm_app *app);
static void menu_always(struct fm_app *app, int item, int opener);
static void menu_system_default(struct fm_app *app, int item);

/*
 * Carries out an action of the menus.
 */
void
fm_ui_action(
	struct fm_app *app,
	unsigned action)
{
	struct fm_tab *tab;
	int handled;
	int item;

	/* The log line the tests wait for, and a frame to show what changed. */
	fm_log("ACTION action=%u", action);
	tab = fm_ui_tab(app);
	app->dirty = 1;

	/* A list column shown or hidden (the name is always there). */
	if (action >= FM_ACTION_COLUMN_FIRST && action < FM_ACTION_COLUMN_FIRST + FM_COLUMN_COUNT) {
		if (action != FM_ACTION_COLUMN_FIRST + FM_COLUMN_NAME)
			app->columns ^= 1U << (action - FM_ACTION_COLUMN_FIRST);
		return;
	}

	/* The selection opened with one of its ways. */
	if (action >= FM_ACTION_OPEN_WITH_FIRST && action < FM_ACTION_OPEN_WITH_FIRST + FM_OPENERS) {
		item = fm_preview_item(app);
		fm_open_entry(app, item, (int)(action - FM_ACTION_OPEN_WITH_FIRST));
		return;
	}

	/* A way made the default of the selection's type, and the selection opened with it (ws093-p003). */
	if (action >= FM_ACTION_ALWAYS_WITH_FIRST && action < FM_ACTION_ALWAYS_WITH_FIRST + FM_OPENERS) {
		item = fm_preview_item(app);
		menu_always(app, item, (int)(action - FM_ACTION_ALWAYS_WITH_FIRST));
		return;
	}

	/* The selection's type given back to the system's default. */
	if (action == FM_ACTION_USE_SYSTEM_DEFAULT) {
		item = fm_preview_item(app);
		menu_system_default(app, item);
		return;
	}

	/* An action only the context menus have (ui-context.c). */
	handled = fm_ui_context_action(app, action);
	if (handled != 0)
		return;

	/* Each other action. */
	switch (action) {
	case FM_ACTION_NEW_WINDOW:
		app->request = FM_REQUEST_NEW_WINDOW;
		break;
	case FM_ACTION_NEW_FOLDER:
		fm_action_new_folder(app);
		break;
	case FM_ACTION_OPEN:
		fm_input_open_selection(app);
		break;
	case FM_ACTION_GET_INFO:
		fm_info_open(app);
		break;
	case FM_ACTION_TRASH:
		fm_action_trash(app);
		break;
	case FM_ACTION_CLOSE_WINDOW:
		app->request = FM_REQUEST_CLOSE;
		break;
	case FM_ACTION_UNDO:
		fm_action_undo(app, 0);
		break;
	case FM_ACTION_REDO:
		fm_action_undo(app, 1);
		break;
	case FM_ACTION_CUT:
		fm_action_copy(app, 1);
		break;
	case FM_ACTION_COPY:
		fm_action_copy(app, 0);
		break;
	case FM_ACTION_PASTE:
		fm_action_paste(app);
		break;
	case FM_ACTION_DUPLICATE:
		fm_action_duplicate(app);
		break;
	case FM_ACTION_SELECT_ALL:
		if (app->focus == FM_FOCUS_LOCATION)
			fm_field_select(&app->location, 0, app->location.length);
		else if (app->focus == FM_FOCUS_SEARCH)
			fm_field_select(&app->search_field, 0, app->search_field.length);
		else if (app->focus == FM_FOCUS_RENAME)
			fm_rename_select_all(app);
		else
			fm_select_all(tab);
		break;
	case FM_ACTION_RENAME:
		fm_action_rename_begin(app);
		break;
	case FM_ACTION_VIEW_ICONS:
		app->view = FM_VIEW_ICONS;
		tab->scroll = 0;
		break;
	case FM_ACTION_VIEW_LIST:
		app->view = FM_VIEW_LIST;
		tab->scroll = 0;
		break;
	case FM_ACTION_SORT_NAME:
		fm_input_sort_by(app, FM_SORT_NAME, 0);
		break;
	case FM_ACTION_SORT_KIND:
		fm_input_sort_by(app, FM_SORT_KIND, 0);
		break;
	case FM_ACTION_SORT_SIZE:
		fm_input_sort_by(app, FM_SORT_SIZE, 0);
		break;
	case FM_ACTION_SORT_MODIFIED:
		fm_input_sort_by(app, FM_SORT_MODIFIED, 0);
		break;
	case FM_ACTION_SHOW_SIDEBAR:
		app->show_sidebar = !app->show_sidebar;
		break;
	case FM_ACTION_SHOW_PREVIEW:
		app->show_preview = !app->show_preview;
		break;
	case FM_ACTION_SHOW_HIDDEN:
		app->show_hidden = !app->show_hidden;
		fm_ui_reload(app, tab);
		break;
	case FM_ACTION_BACK:
		fm_ui_back(app);
		break;
	case FM_ACTION_FORWARD:
		fm_ui_forward(app);
		break;
	case FM_ACTION_ENCLOSING:
		fm_input_enclosing(app);
		break;
	case FM_ACTION_GO_HOME:
		menu_go_kind(app, FM_LOCATION_FOLDER, app->home);
		break;
	case FM_ACTION_GO_DESKTOP:
		menu_go_folder(app, "Desktop");
		break;
	case FM_ACTION_GO_DOCUMENTS:
		menu_go_folder(app, "Documents");
		break;
	case FM_ACTION_GO_DOWNLOADS:
		menu_go_folder(app, "Downloads");
		break;
	case FM_ACTION_GO_RECENTS:
		menu_go_kind(app, FM_LOCATION_RECENTS, "");
		break;
	case FM_ACTION_GO_COMPUTER:
		menu_go_kind(app, FM_LOCATION_FOLDER, "/");
		break;
	case FM_ACTION_GO_TRASH:
		menu_go_kind(app, FM_LOCATION_TRASH, "");
		break;
	case FM_ACTION_GO_LOCATION:
		fm_input_location(app);
		break;
	case FM_ACTION_FIND:
		fm_search_focus(app);
		break;
	case FM_ACTION_MINIMIZE:
		app->request = FM_REQUEST_MINIMIZE;
		break;
	case FM_ACTION_ZOOM:
		app->request = FM_REQUEST_ZOOM;
		break;
	case FM_ACTION_HELP:
		fm_help_open(app, FM_HELP_GUIDE);
		break;
	case FM_ACTION_SHORTCUTS:
		fm_help_open(app, FM_HELP_SHORTCUTS);
		break;
	case FM_ACTION_ABOUT:
		fm_help_open(app, FM_HELP_ABOUT);
		break;
	case FM_ACTION_NEW_TAB:
		fm_tabs_duplicate(app);
		break;
	case FM_ACTION_CLOSE_TAB:
		fm_tabs_close(app, app->tab_index);
		break;
	case FM_ACTION_NEXT_TAB:
		fm_tabs_step(app, 1);
		break;
	case FM_ACTION_PREVIOUS_TAB:
		fm_tabs_step(app, -1);
		break;
	default:
		break;
	}
}

/*
 * Works out the state the menus show: which items do something now, which
 * are checked, and the names of the variable items.
 */
void
fm_ui_menu_state(
	struct fm_app *app,
	struct fm_menu_state *state)
{
	const struct fm_location *location;
	struct fm_tab *tab;
	char **paths;
	uint64_t bytes;
	unsigned mode;
	size_t count;
	int error;

	/* Nothing is known yet. */
	memset(state, 0, sizeof(*state));
	tab = fm_ui_tab(app);
	location = &tab->history[tab->history_index].location;

	/* The selection, and the place: a folder (which takes new items) or the trash. */
	state->selection = (int)fm_select_count(tab, &bytes);
	if (location->kind == FM_LOCATION_FOLDER)
		state->folder = 1;
	if (location->kind == FM_LOCATION_TRASH)
		state->trash = 1;
	state->field = menu_in_field(app);

	/* Paste needs a folder and something on the clipboard. */
	error = fm_clip_get(&mode, &paths, &count);
	if (error == 0 && count != 0U && state->folder != 0)
		state->can_paste = 1;
	fm_paths_free(paths, count);

	/* The histories: undo, redo, back, forward, and a folder above. */
	if (app->undo.undo_count > 0)
		state->can_undo = 1;
	if (app->undo.redo_count > 0)
		state->can_redo = 1;
	if (tab->history_index > 0)
		state->can_back = 1;
	if (tab->history_index + 1 < tab->history_count)
		state->can_forward = 1;

	/* How many tabs the window has (the tab items need two, or room for one more). */
	state->tabs = app->tab_count;
	if (state->folder != 0 && location->path[1] != '\0')
		state->can_enclose = 1;

	/* How the items are shown. */
	state->view = app->view;
	state->sort = app->sort;
	state->columns = app->columns;
	state->sidebar = app->show_sidebar;
	state->preview = app->show_preview;
	state->hidden = app->show_hidden;

	/* The ways to open the selection. */
	menu_openers(app, state);
}

/* Goes to a folder under the home folder by its name, saying so when it is not there. */
static void
menu_go_folder(
	struct fm_app *app,
	const char *name)
{
	struct stat status;
	char path[FM_PATH_MAX];
	char message[128];
	int written;
	int error;

	/* The folder's path (a home folder too long for it has none of these). */
	written = snprintf(path, sizeof(path), "%s/%s", app->home, name);
	if (written < 0 || (size_t)written >= sizeof(path))
		return;

	/* A folder that is not there is not made; the pill says so. */
	error = stat(path, &status);
	if (error != 0) {
		snprintf(message, sizeof(message), "%s does not exist", name);
		fm_ui_message(app, message);
		return;
	}

	/* The tab goes there. */
	menu_go_kind(app, FM_LOCATION_FOLDER, path);
}

/* Goes to a place of a kind with its path. */
static void
menu_go_kind(
	struct fm_app *app,
	unsigned kind,
	const char *path)
{
	struct fm_location location;

	/* The place. */
	memset(&location, 0, sizeof(location));
	location.kind = kind;
	snprintf(location.path, sizeof(location.path), "%s", path);

	/* The tab goes there. */
	fm_ui_go(app, &location);
}

/*
 * Fills the ways to open the one selected file, read again only when the
 * file is another one (or changed) since they were last read.
 */
static void
menu_openers(
	struct fm_app *app,
	struct fm_menu_state *state)
{
	const struct fm_mime *mime;
	struct fm_entry *entry;
	struct fm_tab *tab;
	int item;
	int index;
	int match;

	/* Only one selected file has ways to open it. */
	tab = fm_ui_tab(app);
	item = fm_preview_item(app);
	if (state->selection != 1 || item < 0)
		return;
	entry = &tab->listing.entries[item];
	if (entry->folder != 0)
		return;

	/* The ways, read again for another file. */
	match = strcmp(entry->path, app->menu_openers_path);
	if (match != 0 || entry->modified != app->menu_openers_modified) {
		mime = fm_mime_sniff(entry->path, entry->mime);
		app->menu_opener_count = fm_apps_for(entry->path, mime, entry->mode, app->menu_openers, FM_OPENERS);
		app->menu_user_default = fm_apps_has_default(mime->type);
		snprintf(app->menu_openers_path, sizeof(app->menu_openers_path), "%s", entry->path);
		app->menu_openers_modified = entry->modified;
	}

	/* Their names. */
	state->opener_count = app->menu_opener_count;
	state->user_default = app->menu_user_default;
	for (index = 0; index < app->menu_opener_count; index++)
		snprintf(state->openers[index], sizeof(state->openers[index]), "%s", app->menu_openers[index].name);
}

/* Tells whether the keyboard types into a text field. */
static int
menu_in_field(
	struct fm_app *app)
{
	/* The location, the search and a name being changed are fields. */
	if (app->focus == FM_FOCUS_LOCATION)
		return 1;
	if (app->focus == FM_FOCUS_SEARCH)
		return 1;
	if (app->focus == FM_FOCUS_RENAME)
		return 1;

	/* The content has the keyboard. */
	return 0;
}

/*
 * Makes one of an item's ways the default of the item's type (Always Open
 * With), then opens the item with it; the status pill says what changed.
 */
static void
menu_always(
	struct fm_app *app,
	int item,
	int opener)
{
	struct fm_opener openers[FM_OPENERS];
	const struct fm_mime *mime;
	struct fm_entry *entry;
	struct fm_tab *tab;
	char message[192];
	int count;
	int error;

	/* Only an item of the listing. */
	tab = fm_ui_tab(app);
	if (item < 0 || (size_t)item >= tab->listing.count)
		return;

	/* The item, which must not be a folder (a folder opens in the window). */
	entry = &tab->listing.entries[item];
	if (entry->folder != 0)
		return;

	/* The item's type and its ways, of which the one chosen. */
	mime = fm_mime_sniff(entry->path, entry->mime);
	count = fm_apps_for(entry->path, mime, entry->mode, openers, FM_OPENERS);
	if (opener < 0 || opener >= count)
		return;

	/* The way becomes the type's default; a list that cannot be written says so. */
	error = fm_apps_set_default(mime->type, &openers[opener]);
	if (error != 0) {
		fm_ui_message(app, "Couldn't change the default app.");
		return;
	}

	/* The menus read the ways again. */
	app->menu_openers_path[0] = '\0';

	/* The item opens with its new default, now the first of its ways. */
	fm_open_entry(app, item, 0);

	/* The pill says which files open with the way from now on. */
	snprintf(message, sizeof(message), "%s files now open with %s", mime->kind, openers[opener].name);
	fm_ui_message(app, message);
}

/* Gives an item's type back to the system's default app (Use System Default); the pill says so. */
static void
menu_system_default(
	struct fm_app *app,
	int item)
{
	const struct fm_mime *mime;
	struct fm_entry *entry;
	struct fm_tab *tab;
	char message[192];
	int error;

	/* Only an item of the listing. */
	tab = fm_ui_tab(app);
	if (item < 0 || (size_t)item >= tab->listing.count)
		return;

	/* The item, which must not be a folder (a folder opens in the window). */
	entry = &tab->listing.entries[item];
	if (entry->folder != 0)
		return;

	/* The user's choice for the item's type leaves the list; a list that cannot be written says so. */
	mime = fm_mime_sniff(entry->path, entry->mime);
	error = fm_apps_clear_default(mime->type);
	if (error != 0) {
		fm_ui_message(app, "Couldn't change the default app.");
		return;
	}

	/* The menus read the ways again. */
	app->menu_openers_path[0] = '\0';

	/* The pill says the type opens as the system has it. */
	snprintf(message, sizeof(message), "%s files open with the system's default again", mime->kind);
	fm_ui_message(app, message);
}
