/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The context menus of files (spec §15, design §10.2): what a
 * right press offers, on the selected items, on the empty part of a
 * folder, in the trash, or on a place of the sidebar, and the actions only
 * a context menu has.  The desktop (files --desktop, ws094-p005) has the
 * items' menu without what needs a window (a new tab, the information card)
 * and with Show in Files, and a menu of its own for the empty desktop.
 *
 * The compositor draws the menu at the press (menu.c); this part works out its
 * rows from the window's state and knows nothing of Wayland, so the host's
 * tests read the rows directly.
 */

#include "files.h"

#include <stdio.h>
#include <string.h>

/* The submenus' numbers, past any row's. */
#define CONTEXT_OPEN_WITH	100U
#define CONTEXT_VIEW		102U
#define CONTEXT_SORT		103U
#define CONTEXT_ALWAYS_WITH	104U
#define CONTEXT_MOVE_TO		105U

/* The number of a row that carries out an action: the action's, moved past the others. */
#define CONTEXT_ACTION_ID	1000U

static void context_items(struct fm_app *app, const struct fm_menu_state *state, struct fm_context *context);
static void context_trash(const struct fm_menu_state *state, struct fm_context *context);
static void context_empty(const struct fm_menu_state *state, struct fm_context *context);
static void context_desktop(const struct fm_menu_state *state, struct fm_context *context);
static void context_place(struct fm_app *app, struct fm_context *context);
static void context_drop(struct fm_context *context);
static void context_add(struct fm_context *context, unsigned parent, unsigned kind, const char *label, unsigned action, int enabled);
static void context_submenu(struct fm_context *context, unsigned id, const char *label, int enabled);
static void context_check(struct fm_context *context, unsigned parent, const char *label, unsigned action, int checked);
static int context_folder(struct fm_app *app);
static void context_move_to(struct fm_app *app, struct fm_context *context);
static int context_destination(struct fm_app *app, int place);
static void context_move(struct fm_app *app, int place);

/*
 * Works out the context menu of the last right press (app->context_*),
 * from what the window's menus would show now.
 */
void
fm_ui_context(
	struct fm_app *app,
	struct fm_context *context)
{
	struct fm_menu_state state;

	/* Nothing yet, and the state the rows are enabled by. */
	memset(context, 0, sizeof(*context));
	fm_ui_menu_state(app, &state);

	/* The choice of a drop dropped with "ask". */
	if (app->context_where == FM_CONTEXT_DROP) {
		context_drop(context);
		return;
	}

	/* A place of the sidebar. */
	if (app->context_where == FM_CONTEXT_PLACE) {
		context_place(app, context);
		return;
	}

	/* The empty desktop, or its items. */
	if (app->desktop) {
		if (app->context_where == FM_CONTEXT_EMPTY || state.selection == 0) {
			context_desktop(&state, context);
		} else {
			context_items(app, &state, context);
		}

		/* The desktop's menu is worked out. */
		return;
	}

	/* The empty part of a folder, or of the trash. */
	if (app->context_where == FM_CONTEXT_EMPTY || state.selection == 0) {
		context_empty(&state, context);
		return;
	}

	/* Items in the trash, or elsewhere. */
	if (state.trash != 0) {
		context_trash(&state, context);
		return;
	}

	/* Items anywhere else. */
	context_items(app, &state, context);
}

/*
 * Carries out an action only a context menu has.  Returns 1 when the
 * action was one of them, 0 otherwise.
 */
int
fm_ui_context_action(
	struct fm_app *app,
	unsigned action)
{
	const struct fm_tab *tab;
	struct fm_location location;
	int item;

	/* The selection moved to a place of the sidebar (Move To). */
	if (action >= FM_ACTION_MOVE_TO_FIRST && action < FM_ACTION_MOVE_TO_FIRST + FM_PLACES) {
		context_move(app, (int)(action - FM_ACTION_MOVE_TO_FIRST));
		return 1;
	}

	/* A folder of the selection in a window of its own (another process, as New Window starts). */
	if (action == FM_ACTION_OPEN_IN_NEW_WINDOW) {
		item = fm_preview_item(app);
		tab = fm_ui_tab(app);
		if (item < 0 || tab->listing.entries[item].folder == 0)
			return 1;
		snprintf(app->new_window_folder, sizeof(app->new_window_folder), "%s", tab->listing.entries[item].path);
		fm_log("CONTEXT new-window path=%s", app->new_window_folder);
		app->request = FM_REQUEST_NEW_WINDOW;
		return 1;
	}

	/* A folder of the selection in a new tab. */
	if (action == FM_ACTION_OPEN_IN_NEW_TAB) {
		item = fm_preview_item(app);
		tab = fm_ui_tab(app);
		if (item < 0 || tab->listing.entries[item].folder == 0)
			return 1;
		memset(&location, 0, sizeof(location));
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", tab->listing.entries[item].path);
		fm_tabs_new(app, &location);
		return 1;
	}

	/* The trash's actions on the selection and on the whole trash. */
	if (action == FM_ACTION_PUT_BACK) {
		fm_action_put_back(app);
		return 1;
	}

	/* The selection gone for good (after asking). */
	if (action == FM_ACTION_DELETE_NOW) {
		fm_action_delete(app);
		return 1;
	}

	/* The whole trash. */
	if (action == FM_ACTION_EMPTY_TRASH) {
		fm_action_empty_trash(app);
		return 1;
	}

	/* The pressed place of the sidebar: in a new tab, or off the sidebar. */
	if (action == FM_ACTION_PLACE_NEW_TAB) {
		if (app->context_place >= 0 && app->context_place < app->places.count)
			fm_tabs_new(app, &app->places.items[app->context_place].location);
		return 1;
	}

	/* The choice of a drop dropped with "ask": the operation, then the drop is carried out; or none, and the drop is given up. */
	if (action == FM_ACTION_DROP_MOVE || action == FM_ACTION_DROP_COPY || action == FM_ACTION_DROP_LINK) {
		app->drop_operation = FM_TASK_MOVE;
		if (action == FM_ACTION_DROP_COPY)
			app->drop_operation = FM_TASK_COPY;
		if (action == FM_ACTION_DROP_LINK)
			app->drop_operation = FM_TASK_LINK;
		app->drop_asking = 0;
		app->request = FM_REQUEST_DROP;
		return 1;
	}

	/* No operation. */
	if (action == FM_ACTION_DROP_CANCEL) {
		app->drop_asking = 0;
		app->request = FM_REQUEST_DROP_CANCEL;
		return 1;
	}

	/* The pressed favorite off the sidebar. */
	if (action == FM_ACTION_PLACE_REMOVE) {
		if (app->context_place >= 0 && app->context_place < app->places.count)
			fm_action_remove_favorite(app, app->context_place);
		return 1;
	}

	/* Not a context menu's own. */
	return 0;
}

/* Works out the rows for selected items outside the trash. */
static void
context_items(
	struct fm_app *app,
	const struct fm_menu_state *state,
	struct fm_context *context)
{
	int single;
	int folder;
	int openable;
	int index;

	/* One item selected, and whether it is a folder. */
	single = 0;
	if (state->selection == 1)
		single = 1;
	folder = context_folder(app);

	/* Opening: the default way, and a folder in a new tab (not on the desktop, which has no tabs). */
	context_add(context, 0U, FM_ROW_ITEM, "Open", FM_ACTION_OPEN, 1);
	if (single != 0 &&
	    folder != 0 &&
	    app->desktop == 0) {
		context_add(context, 0U, FM_ROW_ITEM, "Open in New Tab", FM_ACTION_OPEN_IN_NEW_TAB, state->tabs < FM_TABS);
		context_add(context, 0U, FM_ROW_ITEM, "Open in New Window", FM_ACTION_OPEN_IN_NEW_WINDOW, 1);
	}

	/* The other ways to open, in a submenu. */
	context_submenu(context, CONTEXT_OPEN_WITH, "Open With", state->opener_count > 0);
	for (index = 0; index < state->opener_count; index++)
		context_add(context, CONTEXT_OPEN_WITH, FM_ROW_ITEM, state->openers[index], FM_ACTION_OPEN_WITH_FIRST + (unsigned)index, 1);

	/* The ways that can become the default of the selection's type, and the way back to the system's (ws093-p003). */
	openable = 0;
	if (state->opener_count > 0)
		openable = 1;
	context_submenu(context, CONTEXT_ALWAYS_WITH, "Always Open With", openable);
	for (index = 0; index < state->opener_count; index++)
		context_add(context, CONTEXT_ALWAYS_WITH, FM_ROW_ITEM, state->openers[index], FM_ACTION_ALWAYS_WITH_FIRST + (unsigned)index, 1);

	/* A line, then Use System Default, enabled when the user chose a default for the type. */
	context_add(context, CONTEXT_ALWAYS_WITH, FM_ROW_LINE, "", 0U, 1);
	context_add(context, CONTEXT_ALWAYS_WITH, FM_ROW_ITEM, "Use System Default", FM_ACTION_USE_SYSTEM_DEFAULT, state->user_default);

	/* The clipboard. */
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Cut", FM_ACTION_CUT, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Copy", FM_ACTION_COPY, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Paste", FM_ACTION_PASTE, state->can_paste);

	/* Changing the items. */
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Rename", FM_ACTION_RENAME, single);
	context_add(context, 0U, FM_ROW_ITEM, "Duplicate", FM_ACTION_DUPLICATE, 1);
	context_move_to(app, context);

	/* The information (on the desktop, which has no card for it: the desktop's folder in Files), and the trash. */
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	if (app->desktop)
		context_add(context, 0U, FM_ROW_ITEM, "Show in Files", FM_ACTION_SHOW_IN_FILES, 1);
	else
		context_add(context, 0U, FM_ROW_ITEM, "Get Info", FM_ACTION_GET_INFO, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Move to Trash", FM_ACTION_TRASH, 1);
}

/* Works out the rows for selected items in the trash. */
static void
context_trash(
	const struct fm_menu_state *state,
	struct fm_context *context)
{
	/* Back where they were, or gone for good; the whole trash. */
	(void)state;
	context_add(context, 0U, FM_ROW_ITEM, "Put Back", FM_ACTION_PUT_BACK, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Delete Immediately", FM_ACTION_DELETE_NOW, 1);
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Empty Trash", FM_ACTION_EMPTY_TRASH, 1);
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Get Info", FM_ACTION_GET_INFO, 1);
}

/* Works out the rows for the empty part of a place: new items, paste, how the items are shown. */
static void
context_empty(
	const struct fm_menu_state *state,
	struct fm_context *context)
{
	/* The trash offers to be emptied. */
	if (state->trash != 0) {
		context_add(context, 0U, FM_ROW_ITEM, "Empty Trash", FM_ACTION_EMPTY_TRASH, 1);
		context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	}

	/* A folder takes new items and the clipboard's. */
	if (state->folder != 0) {
		context_add(context, 0U, FM_ROW_ITEM, "New Folder", FM_ACTION_NEW_FOLDER, 1);
		context_add(context, 0U, FM_ROW_ITEM, "Paste", FM_ACTION_PASTE, state->can_paste);
		context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	}

	/* The view: icons or a list. */
	context_submenu(context, CONTEXT_VIEW, "View", 1);
	context_check(context, CONTEXT_VIEW, "as Icons", FM_ACTION_VIEW_ICONS, state->view == FM_VIEW_ICONS);
	context_check(context, CONTEXT_VIEW, "as List", FM_ACTION_VIEW_LIST, state->view == FM_VIEW_LIST);

	/* The order. */
	context_submenu(context, CONTEXT_SORT, "Sort By", 1);
	context_check(context, CONTEXT_SORT, "Name", FM_ACTION_SORT_NAME, state->sort == FM_SORT_NAME);
	context_check(context, CONTEXT_SORT, "Kind", FM_ACTION_SORT_KIND, state->sort == FM_SORT_KIND);
	context_check(context, CONTEXT_SORT, "Size", FM_ACTION_SORT_SIZE, state->sort == FM_SORT_SIZE);
	context_check(context, CONTEXT_SORT, "Date Modified", FM_ACTION_SORT_MODIFIED, state->sort == FM_SORT_MODIFIED);

	/* The hidden files. */
	context_check(context, 0U, "Show Hidden Files", FM_ACTION_SHOW_HIDDEN, state->hidden);
}

/*
 * Works out the rows for the empty desktop: a new folder and the
 * clipboard's items, the items put back in order, the desktop's folder in
 * Files, and the wallpaper (when Settings is there to change it).
 */
static void
context_desktop(
	const struct fm_menu_state *state,
	struct fm_context *context)
{
	int wallpaper;

	/* New items and the clipboard's. */
	context_add(context, 0U, FM_ROW_ITEM, "New Folder", FM_ACTION_NEW_FOLDER, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Paste", FM_ACTION_PASTE, state->can_paste);

	/* The items' order, and the folder in a window. */
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Clean Up", FM_ACTION_CLEAN_UP, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Show Desktop in Files", FM_ACTION_SHOW_IN_FILES, 1);

	/* The wallpaper, only when Settings can change it. */
	wallpaper = fm_desktop_can_change_wallpaper();
	if (wallpaper) {
		context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
		context_add(context, 0U, FM_ROW_ITEM, "Change Wallpaper\xe2\x80\xa6", FM_ACTION_CHANGE_WALLPAPER, 1);
	}

	/* Succeeded: the desktop's rows are listed. */
	return;
}

/* Works out the rows for a place of the sidebar. */
static void
context_place(
	struct fm_app *app,
	struct fm_context *context)
{
	const struct fm_place *place;
	int removable;

	/* The place pressed; none left means no rows. */
	if (app->context_place < 0 || app->context_place >= app->places.count)
		return;
	place = &app->places.items[app->context_place];

	/* In a new tab; a favorite folder can leave the sidebar. */
	context_add(context, 0U, FM_ROW_ITEM, "Open in New Tab", FM_ACTION_PLACE_NEW_TAB, app->tab_count < FM_TABS);
	removable = fm_place_is_favorite_folder(place);
	if (removable != 0) {
		context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
		context_add(context, 0U, FM_ROW_ITEM, "Remove from Sidebar", FM_ACTION_PLACE_REMOVE, 1);
	}
}

/* The rows of a drop's choice ("ask"): move, copy or link here, or cancel. */
static void
context_drop(
	struct fm_context *context)
{
	/* The three operations, a line, and none. */
	context_add(context, 0U, FM_ROW_ITEM, "Move Here", FM_ACTION_DROP_MOVE, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Copy Here", FM_ACTION_DROP_COPY, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Link Here", FM_ACTION_DROP_LINK, 1);
	context_add(context, 0U, FM_ROW_LINE, "", 0U, 1);
	context_add(context, 0U, FM_ROW_ITEM, "Cancel", FM_ACTION_DROP_CANCEL, 1);
}

/* Adds a row at the end, when there is room; its number follows the last. */
static void
context_add(
	struct fm_context *context,
	unsigned parent,
	unsigned kind,
	const char *label,
	unsigned action,
	int enabled)
{
	struct fm_context_row *row;

	/* A full menu takes no more rows. */
	if (context->count == FM_CONTEXT_ROWS)
		return;

	/* The row, numbered from 1; a row that carries out an action is numbered by it (the same in every menu). */
	row = &context->rows[context->count];
	memset(row, 0, sizeof(*row));
	row->id = context->count + 1U;
	if (action != 0U)
		row->id = CONTEXT_ACTION_ID + action;
	row->parent = parent;
	row->kind = kind;
	snprintf(row->label, sizeof(row->label), "%s", label);
	row->action = action;
	if (enabled != 0)
		row->enabled = 1;
	context->count++;
}

/* Adds a submenu's row, numbered by the submenu (its rows name it as their parent). */
static void
context_submenu(
	struct fm_context *context,
	unsigned id,
	const char *label,
	int enabled)
{
	/* The row, then its number replaced by the submenu's. */
	context_add(context, 0U, FM_ROW_SUBMENU, label, 0U, enabled);
	if (context->count > 0U && context->rows[context->count - 1U].kind == FM_ROW_SUBMENU)
		context->rows[context->count - 1U].id = id;
}

/* Tells whether the item the selection is shown by is a folder. */
static int
context_folder(
	struct fm_app *app)
{
	const struct fm_tab *tab;
	int item;

	/* The item, when there is one. */
	item = fm_preview_item(app);
	if (item < 0)
		return 0;

	/* A folder. */
	tab = fm_ui_tab(app);
	if (tab->listing.entries[item].folder != 0)
		return 1;

	/* Not a folder. */
	return 0;
}

/*
 * Adds the Move To submenu (spec §15): the sidebar's favorite folders and
 * mounted volumes, each a row that moves the selection there.  The folder
 * shown is left out, and so is a place the selection is in already.
 */
static void
context_move_to(
	struct fm_app *app,
	struct fm_context *context)
{
	int destinations;
	int place;
	int usable;

	/* How many places can take the selection. */
	destinations = 0;
	for (place = 0; place < app->places.count; place++) {
		usable = context_destination(app, place);
		if (usable != 0)
			destinations++;
	}

	/* The submenu, enabled when it has a row. */
	context_submenu(context, CONTEXT_MOVE_TO, "Move To", destinations > 0);
	for (place = 0; place < app->places.count; place++) {
		usable = context_destination(app, place);
		if (usable == 0)
			continue;
		context_add(context, CONTEXT_MOVE_TO, FM_ROW_ITEM, app->places.items[place].label, FM_ACTION_MOVE_TO_FIRST + (unsigned)place, 1);
	}
}

/* Tells whether a place of the sidebar can take the selection by Move To. */
static int
context_destination(
	struct fm_app *app,
	int place)
{
	const struct fm_place *item;
	const char *shown;
	int differs;

	/* A place that is not there takes nothing. */
	item = &app->places.items[place];
	if (item->missing != 0)
		return 0;

	/* A favorite folder (Home among them), or a mounted volume of the locations. */
	if (item->section == FM_SECTION_FAVORITES) {
		if (item->location.kind != FM_LOCATION_FOLDER)
			return 0;
	} else if (item->section == FM_SECTION_LOCATIONS) {
		if (item->location.kind != FM_LOCATION_FOLDER || item->icon != KL_ICON_VOLUME)
			return 0;
	} else {
		return 0;
	}

	/* A place without a folder cannot hold items. */
	if (item->location.path[0] == '\0')
		return 0;

	/* The folder shown is where the items are already. */
	shown = fm_current_folder(app);
	if (shown != NULL) {
		differs = strcmp(shown, item->location.path);
		if (differs == 0)
			return 0;
	}

	/* Succeeded: the place can take the selection. */
	return 1;
}

/* Moves the selection to a place of the sidebar, as a drop there would (undone with Ctrl+Z). */
static void
context_move(
	struct fm_app *app,
	int place)
{
	char **paths;
	size_t count;
	int usable;
	int error;

	/* Only a place the menu offered. */
	if (place < 0 || place >= app->places.count)
		return;
	usable = context_destination(app, place);
	if (usable == 0)
		return;

	/* The selection's paths. */
	error = fm_selected_paths(app, &paths, &count);
	if (error != 0 || count == 0) {
		fm_paths_free(paths, count);
		return;
	}

	/* A move task like a drop's, which asks about taken names. */
	fm_log("CONTEXT move-to place=%d path=%s count=%zu", place, app->places.items[place].location.path, count);
	error = fm_action_transfer(app, FM_TASK_MOVE, paths, count, app->places.items[place].location.path);
	if (error != 0)
		fm_ui_message(app, "The items could not be moved");
	fm_paths_free(paths, count);
}

/* Adds a row that can be checked (a view, an order), checked or not. */
static void
context_check(
	struct fm_context *context,
	unsigned parent,
	const char *label,
	unsigned action,
	int checked)
{
	/* The row, then its mark. */
	context_add(context, parent, FM_ROW_CHECK, label, action, 1);
	if (checked != 0 && context->count > 0U)
		context->rows[context->count - 1U].checked = 1;
}
