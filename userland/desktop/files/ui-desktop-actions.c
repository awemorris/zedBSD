/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the desktop's context menus and keys do to its items (files
 * --desktop, ws094-p005, plan/ws094/design.md §4).
 *
 * Most actions are the file manager's own (fm_ui_action: the ways to
 * open, the clipboard, Duplicate, the trash, New Folder, undo);
 * the desktop carries out itself what differs from a window: Open (a
 * folder opens in a new Files window, not in place), the change of a name
 * (the item keeps its cell under its new name), Show in Files, Clean Up
 * and Change Wallpaper.
 */

#include "files.h"

#include "userland/desktop/paths.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The keys of the file operations (evdev codes). */
#define DESKTOP_KEY_DELETE	111U
#define DESKTOP_KEY_F2		60U
#define DESKTOP_KEY_C		46U
#define DESKTOP_KEY_X		45U
#define DESKTOP_KEY_V		47U
#define DESKTOP_KEY_D		32U
#define DESKTOP_KEY_Z		44U
#define DESKTOP_KEY_N		49U

/* The programs the desktop starts: Files for a folder, Settings for the wallpaper, and Settings' page. */
#define DESKTOP_FILES		KEILAND_BINDIR "/files"
#define DESKTOP_SETTINGS	KEILAND_BINDIR "/settings"
#define DESKTOP_WALLPAPER_PAGE	"wallpaper"

static void desktop_show_in_files(struct fm_app *app);
static void desktop_clean_up(struct fm_app *app);
static void desktop_change_wallpaper(void);
static void desktop_folder(struct fm_app *app, char *folder, size_t size);

/*
 * Carries out an action of the desktop's context menu (or a menu's action
 * the desktop was sent).
 */
void
fm_desktop_action(
	struct fm_app *app,
	unsigned action)
{
	/* A frame to show what changed. */
	app->dirty = 1;

	/* The actions the desktop carries out itself; the others are the file manager's. */
	switch (action) {
	case FM_ACTION_OPEN:
		fm_log("DESKTOP open via=menu");
		fm_desktop_open_selected(app);
		break;
	case FM_ACTION_SHOW_IN_FILES:
		desktop_show_in_files(app);
		break;
	case FM_ACTION_CLEAN_UP:
		desktop_clean_up(app);
		break;
	case FM_ACTION_CHANGE_WALLPAPER:
		desktop_change_wallpaper();
		break;
	default:
		fm_ui_action(app, action);
		break;
	}

	/* Succeeded: the chosen action has been dispatched. */
	return;
}

/*
 * Handles a key of the file operations on the desktop: Delete moves the
 * selection to the trash, F2 changes a name, Ctrl+C, X and V copy, cut and
 * paste, Ctrl+D duplicates, Ctrl+Z undoes (with Shift, redoes), and
 * Ctrl+Shift+N makes a folder.  Returns 1 when the key was one of them.
 */
int
fm_desktop_operation_key(
	struct fm_app *app,
	const struct fm_event *event)
{
	unsigned action;

	/* The key with exactly its modifiers, as an action. */
	action = FM_ACTION_NONE;
	if (event->key == DESKTOP_KEY_DELETE && event->modifiers == 0U) {
		action = FM_ACTION_TRASH;
	} else if (event->key == DESKTOP_KEY_F2 && event->modifiers == 0U) {
		action = FM_ACTION_RENAME;
	} else if (event->key == DESKTOP_KEY_C && event->modifiers == FM_MOD_CTRL) {
		action = FM_ACTION_COPY;
	} else if (event->key == DESKTOP_KEY_X && event->modifiers == FM_MOD_CTRL) {
		action = FM_ACTION_CUT;
	} else if (event->key == DESKTOP_KEY_V && event->modifiers == FM_MOD_CTRL) {
		action = FM_ACTION_PASTE;
	} else if (event->key == DESKTOP_KEY_D && event->modifiers == FM_MOD_CTRL) {
		action = FM_ACTION_DUPLICATE;
	} else if (event->key == DESKTOP_KEY_Z && event->modifiers == FM_MOD_CTRL) {
		action = FM_ACTION_UNDO;
	} else if (event->key == DESKTOP_KEY_Z && event->modifiers == (FM_MOD_CTRL | FM_MOD_SHIFT)) {
		action = FM_ACTION_REDO;
	} else if (event->key == DESKTOP_KEY_N && event->modifiers == (FM_MOD_CTRL | FM_MOD_SHIFT)) {
		action = FM_ACTION_NEW_FOLDER;
	}

	/* Not a key of the operations. */
	if (action == FM_ACTION_NONE)
		return 0;

	/* The action, as the menu would carry it out. */
	fm_desktop_action(app, action);
	return 1;
}

/*
 * Ends the change of an item's name on the desktop (Enter, Esc, or a press
 * elsewhere): with commit the item is renamed, and its place, shown and
 * saved, follows it to its new name, so it stays in its cell.
 */
void
fm_desktop_rename_end(
	struct fm_app *app,
	int commit)
{
	struct stat status;
	char old_name[FM_NAME_MAX];
	char new_name[FM_PATH_MAX];
	char old_path[FM_PATH_MAX];
	char new_path[2 * FM_PATH_MAX + 2];
	char *slash;
	int old_there;
	int new_there;
	int error;

	/* The names and paths before and after, taken before the change ends. */
	snprintf(old_path, sizeof(old_path), "%s", app->rename_path);
	slash = strrchr(old_path, '/');
	old_name[0] = '\0';
	if (slash != NULL)
		snprintf(old_name, sizeof(old_name), "%s", slash + 1);
	snprintf(new_name, sizeof(new_name), "%s", app->rename.text);

	/* The file manager's change of the name (it refuses a bad or taken name and says so). */
	fm_action_rename_end(app, commit);

	/* Given up, or a path without a folder: no place to follow. */
	if (commit == 0 || slash == NULL)
		return;

	/* The item is renamed when the old path is gone and the new one is there. */
	*slash = '\0';
	snprintf(new_path, sizeof(new_path), "%s/%s", old_path, new_name);
	old_there = lstat(app->rename_path, &status);
	if (old_there == 0)
		return;

	/* Only an existing new path can receive the old saved place. */
	new_there = lstat(new_path, &status);
	if (new_there != 0)
		return;

	/* Its place under the new name. */
	error = fm_desktop_layout_rename(&app->desk, old_name, new_name);
	fm_log("DESKTOP rename from=%s to=%s error=%d", old_name, new_name, error);

	/* Succeeded: the saved-place rename has been reported. */
	return;
}

/*
 * Tells whether the desktop's context menu offers Change Wallpaper: only
 * when Settings, which has the wallpaper's page, is installed.
 */
int
fm_desktop_can_change_wallpaper(void)
{
	int status;

	/* Settings, runnable. */
	status = access(DESKTOP_SETTINGS, X_OK);
	if (status != 0)
		return 0;

	/* Succeeded: the wallpaper can be changed there. */
	return 1;
}

/* Opens the desktop's folder in a new Files window. */
static void
desktop_show_in_files(
	struct fm_app *app)
{
	char *arguments[3];
	char folder[FM_PATH_MAX];
	int error;

	/* The desktop's folder (the items' selection is not handed over). */
	desktop_folder(app, folder, sizeof(folder));

	/* Files, on it. */
	arguments[0] = DESKTOP_FILES;
	arguments[1] = folder;
	arguments[2] = NULL;
	error = fm_apps_spawn(arguments);
	fm_log("DESKTOP show-in-files path=%s error=%d", folder, error);

	/* Succeeded: the Files launch has been reported. */
	return;
}

/* Puts the items back in order (Clean Up): the places the user gave are forgotten and the items laid out by name. */
static void
desktop_clean_up(
	struct fm_app *app)
{
	int error;

	/* No saved places (the file goes), nor the places shown. */
	error = fm_desktop_clean_up(&app->desk);
	if (error != 0)
		fm_ui_message(app, "Couldn't clean up the desktop");

	/* Succeeded: any layout-write failure has been shown. */
	return;
}

/* Opens Settings on its wallpaper's page. */
static void
desktop_change_wallpaper(void)
{
	char *arguments[3];
	int error;

	/* Settings, on the page. */
	arguments[0] = DESKTOP_SETTINGS;
	arguments[1] = DESKTOP_WALLPAPER_PAGE;
	arguments[2] = NULL;
	error = fm_apps_spawn(arguments);
	fm_log("DESKTOP change-wallpaper error=%d", error);

	/* Succeeded: the Settings launch has been reported. */
	return;
}

/* Finds the folder the desktop shows (the tab's place). */
static void
desktop_folder(
	struct fm_app *app,
	char *folder,
	size_t size)
{
	struct fm_tab *tab;

	/* The place the tab shows now. */
	tab = fm_ui_tab(app);
	snprintf(folder, size, "%s", tab->history[tab->history_index].location.path);

	/* Succeeded: the caller has the desktop folder path. */
	return;
}
