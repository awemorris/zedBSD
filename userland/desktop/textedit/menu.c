/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of Text Editor in the compositor's System Menu (plan/ws092/design.md
 * section 13): File, Edit, View and Help, and the context menu of the text
 * (Undo, Redo, Cut, Copy, Paste, Select All), given to libkeiland as tables
 * (WS131 p016: kl_window_set_menu, kl_window_popup_menu); the editor's
 * state is the actions' state (kl_window_set_action_state), which every
 * item of an action shows.  The compositor draws them as the menu bar in the
 * window's titlebar, its items underlined as every application's (BUG-248:
 * the window gives no titlebar controls, which would take the menu bar's
 * place), and chooses an item for its shortcut; the choice comes back as a
 * KL_WINDOW_ACTION input among the window's.  A compositor without the System Menu leaves
 * the editor without menus, and the keys work as they do with them.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The submenus. */
#define MENU_FILE		1U
#define MENU_EDIT		2U
#define MENU_VIEW		3U
#define MENU_HELP		4U

/* The items of File. */
#define MENU_NEW		10U
#define MENU_OPEN		11U
#define MENU_RECENT		18U
#define MENU_FILE_LINE		12U
#define MENU_SAVE		13U
#define MENU_SAVE_AS		14U
#define MENU_FILE_LINE_2	15U
#define MENU_CLOSE		16U
#define MENU_QUIT		17U

/* The items of Edit. */
#define MENU_UNDO		20U
#define MENU_REDO		21U
#define MENU_EDIT_LINE		22U
#define MENU_CUT		23U
#define MENU_COPY		24U
#define MENU_PASTE		25U
#define MENU_SELECT_ALL		26U
#define MENU_EDIT_LINE_2	27U
#define MENU_FIND		28U
#define MENU_FIND_NEXT		29U
#define MENU_FIND_PREVIOUS	30U
#define MENU_REPLACE		31U

/* The items of View. */
#define MENU_LINE_NUMBERS	40U
#define MENU_WORD_WRAP		41U
#define MENU_VIEW_LINE		42U
#define MENU_BIGGER		43U
#define MENU_SMALLER		44U
#define MENU_ACTUAL_SIZE	45U

/* The item of Help. */
#define MENU_ABOUT		50U

/* The items of the context menu. */
#define MENU_CONTEXT_UNDO	60U
#define MENU_CONTEXT_REDO	61U
#define MENU_CONTEXT_LINE	62U
#define MENU_CONTEXT_CUT	63U
#define MENU_CONTEXT_COPY	64U
#define MENU_CONTEXT_PASTE	65U
#define MENU_CONTEXT_LINE_2	66U
#define MENU_CONTEXT_ALL	67U

/*
 * The items of File > Open Recent (ws128-p003): one a file from the first,
 * and the line that says there is none.  They are made anew whenever the
 * recent files change (te_menu_recent).
 */
#define MENU_RECENT_FIRST	70U
#define MENU_RECENT_NONE	80U

/* The keysyms of the shortcuts' keys that are not letters. */
#define MENU_KEY_EQUAL		0x3dU
#define MENU_KEY_MINUS		0x2dU
#define MENU_KEY_ZERO		0x30U

/* The window's menus, in the order they are shown. */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_NEW, MENU_FILE, KL_MENU_ITEM_NORMAL, "New", TE_ACTION_NEW, KL_MENU_ROLE_NEW, KL_MENU_CTRL, 'n' },
	{ MENU_OPEN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Open...", TE_ACTION_OPEN, KL_MENU_ROLE_OPEN, KL_MENU_CTRL, 'o' },
	{ MENU_RECENT, MENU_FILE, KL_MENU_ITEM_SUBMENU, "Open Recent", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FILE_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SAVE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Save", TE_ACTION_SAVE, KL_MENU_ROLE_SAVE, KL_MENU_CTRL, 's' },
	{ MENU_SAVE_AS, MENU_FILE, KL_MENU_ITEM_NORMAL, "Save As...", TE_ACTION_SAVE_AS, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 's' },
	{ MENU_FILE_LINE_2, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLOSE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Close", TE_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL, 'w' },
	{ MENU_QUIT, MENU_FILE, KL_MENU_ITEM_NORMAL, "Quit Text Editor", TE_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_UNDO, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Undo", TE_ACTION_UNDO, KL_MENU_ROLE_UNDO, KL_MENU_CTRL, 'z' },
	{ MENU_REDO, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Redo", TE_ACTION_REDO, KL_MENU_ROLE_REDO, KL_MENU_CTRL | KL_MENU_SHIFT, 'z' },
	{ MENU_EDIT_LINE, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CUT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Cut", TE_ACTION_CUT, KL_MENU_ROLE_CUT, KL_MENU_CTRL, 'x' },
	{ MENU_COPY, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Copy", TE_ACTION_COPY, KL_MENU_ROLE_COPY, KL_MENU_CTRL, 'c' },
	{ MENU_PASTE, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Paste", TE_ACTION_PASTE, KL_MENU_ROLE_PASTE, KL_MENU_CTRL, 'v' },
	{ MENU_SELECT_ALL, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Select All", TE_ACTION_SELECT_ALL, KL_MENU_ROLE_SELECT_ALL, KL_MENU_CTRL, 'a' },
	{ MENU_EDIT_LINE_2, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIND, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find...", TE_ACTION_FIND, KL_MENU_ROLE_FIND, KL_MENU_CTRL, 'f' },
	{ MENU_FIND_NEXT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Next", TE_ACTION_FIND_NEXT, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'g' },
	{ MENU_FIND_PREVIOUS, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Previous", TE_ACTION_FIND_PREVIOUS, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 'g' },
	{ MENU_REPLACE, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Replace...", TE_ACTION_REPLACE, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'h' },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE_NUMBERS, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Line Numbers", TE_ACTION_LINE_NUMBERS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_WORD_WRAP, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Word Wrap", TE_ACTION_WORD_WRAP, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_BIGGER, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Bigger Text", TE_ACTION_BIGGER, KL_MENU_ROLE_ZOOM_IN, KL_MENU_CTRL, MENU_KEY_EQUAL },
	{ MENU_SMALLER, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Smaller Text", TE_ACTION_SMALLER, KL_MENU_ROLE_ZOOM_OUT, KL_MENU_CTRL, MENU_KEY_MINUS },
	{ MENU_ACTUAL_SIZE, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Actual Size", TE_ACTION_ACTUAL_SIZE, KL_MENU_ROLE_NONE, KL_MENU_CTRL, MENU_KEY_ZERO },
	{ MENU_HELP, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Help", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ABOUT, MENU_HELP, KL_MENU_ITEM_NORMAL, "About Text Editor", TE_ACTION_ABOUT, KL_MENU_ROLE_ABOUT, 0U, 0U }
};

/* The context menu of the text, in its order. */
static const struct kl_menu_entry menu_context_items[] = {
	{ MENU_CONTEXT_UNDO, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Undo", TE_ACTION_UNDO, KL_MENU_ROLE_UNDO, 0U, 0U },
	{ MENU_CONTEXT_REDO, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Redo", TE_ACTION_REDO, KL_MENU_ROLE_REDO, 0U, 0U },
	{ MENU_CONTEXT_LINE, KL_MENU_ROOT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CONTEXT_CUT, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Cut", TE_ACTION_CUT, KL_MENU_ROLE_CUT, 0U, 0U },
	{ MENU_CONTEXT_COPY, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Copy", TE_ACTION_COPY, KL_MENU_ROLE_COPY, 0U, 0U },
	{ MENU_CONTEXT_PASTE, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Paste", TE_ACTION_PASTE, KL_MENU_ROLE_PASTE, 0U, 0U },
	{ MENU_CONTEXT_LINE_2, KL_MENU_ROOT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CONTEXT_ALL, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Select All", TE_ACTION_SELECT_ALL, KL_MENU_ROLE_SELECT_ALL, 0U, 0U }
};

static int menu_send(struct te_menu *menu);
static void menu_action_state(struct te_menu *menu, uint32_t action, int enabled, int checked);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the editor then
 * has no menus), or an errno value when the menus could not be made.
 */
int
te_menu_open(
	struct te_menu *menu,
	struct te_window *window,
	const struct te_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(menu, 0, sizeof(*menu));
	menu->window = window;

	/* The menus; a compositor without the System Menu leaves the editor without them. */
	error = menu_send(menu);
	if (error == ENOTSUP) {
		te_log("MENU none errno=%d", error);
		return 0;
	}
	if (error != 0)
		return error;
	menu->shown_once = 1;

	/* The state they show. */
	te_menu_refresh(menu, state);

	/* Succeeded: the menus are the compositor's to show. */
	te_log("MENU ready items=%u", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));
	return 0;
}

/*
 * Tells the menus (and the context menu of the same actions) the editor's
 * state when it differs from what they show.
 */
void
te_menu_refresh(
	struct te_menu *menu,
	const struct te_state *state)
{
	int same;

	/* Nothing when the state is the one shown. */
	same = memcmp(state, &menu->shown, sizeof(*state));
	if (same == 0 && menu->sent)
		return;

	/* Each action's state: Undo, Redo, Cut and Copy when they can act, Save with changes, View's switches checked. */
	menu_action_state(menu, TE_ACTION_UNDO, state->can_undo, 0);
	menu_action_state(menu, TE_ACTION_REDO, state->can_redo, 0);
	menu_action_state(menu, TE_ACTION_CUT, state->selected, 0);
	menu_action_state(menu, TE_ACTION_COPY, state->selected, 0);
	menu_action_state(menu, TE_ACTION_SAVE, state->modified, 0);
	menu_action_state(menu, TE_ACTION_LINE_NUMBERS, 1, state->line_numbers);
	menu_action_state(menu, TE_ACTION_WORD_WRAP, 1, state->wrap);

	/* Shown. */
	menu->shown = *state;
	menu->sent = 1;
}

/*
 * Shows the recent files in File > Open Recent (ws128-p003): an item for
 * each file, newest first, greyed when the file is no longer there;
 * without any, one greyed line says so.
 */
void
te_menu_recent(
	struct te_menu *menu,
	const struct te_app *app)
{
	struct kl_menu_entry *entry;
	const char *name;
	const char *slash;
	size_t index;
	int error;

	/* A file an item, named by its file name; one that is gone is greyed. */
	menu->recent_count = 0U;
	for (index = 0; index < app->recent_count && index < TE_RECENT_MAX; index++) {
		name = app->recent[index];
		slash = strrchr(name, '/');
		if (slash != NULL && slash[1] != '\0')
			name = slash + 1;
		entry = &menu->recent[menu->recent_count];
		memset(entry, 0, sizeof(*entry));
		entry->id = MENU_RECENT_FIRST + (uint32_t)index;
		entry->parent = MENU_RECENT;
		entry->type = KL_MENU_ITEM_NORMAL;
		entry->label = name;
		entry->action = TE_ACTION_RECENT_FIRST + (uint32_t)index;
		entry->role = KL_MENU_ROLE_NONE;
		menu->recent_count++;
		menu_action_state(menu, entry->action, app->recent_present[index], 0);
	}

	/* No recent file: a greyed line says so. */
	if (menu->recent_count == 0U) {
		entry = &menu->recent[0];
		memset(entry, 0, sizeof(*entry));
		entry->id = MENU_RECENT_NONE;
		entry->parent = MENU_RECENT;
		entry->type = KL_MENU_ITEM_NORMAL;
		entry->label = "No Recent Files";
		entry->action = TE_ACTION_RECENT_NONE;
		entry->role = KL_MENU_ROLE_NONE;
		menu->recent_count = 1U;
		menu_action_state(menu, TE_ACTION_RECENT_NONE, 0, 0);
	}

	/* The menu with them; a compositor without the System Menu has none to show. */
	if (!menu->shown_once)
		return;
	error = menu_send(menu);
	if (error != 0) {
		te_log("MENU recent-failed errno=%d", error);
		return;
	}

	/* The log says how many are shown. */
	te_log("MENU recent count=%lu", (unsigned long)app->recent_count);
}

/*
 * Opens the context menu of the text at a place of the window, for its
 * last press.
 */
void
te_menu_popup(
	struct te_menu *menu,
	int x,
	int y)
{
	int error;

	/* The menu at the place; without the System Menu nothing opens. */
	error = kl_window_popup_menu(menu->window->kui, menu_context_items, sizeof(menu_context_items) / sizeof(menu_context_items[0]), x, y);
	if (error != 0) {
		te_log("MENU popup-failed errno=%d", error);
		return;
	}

	/* Succeeded: the menu is open. */
	te_log("MENU popup x=%d y=%d", x, y);
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
te_menu_close(
	struct te_menu *menu)
{
	/* No menu on the window, and nothing kept. */
	if (menu->window != NULL && menu->shown_once)
		(void)kl_window_set_menu(menu->window->kui, NULL, 0U);
	memset(menu, 0, sizeof(*menu));
}

/* Gives libkeiland the window's menu: the table, with File > Open Recent's items after it. */
static int
menu_send(
	struct te_menu *menu)
{
	struct kl_menu_entry entries[sizeof(menu_items) / sizeof(menu_items[0]) + TE_RECENT_MAX];
	size_t count;
	int error;

	/* The table, and the recent files' items (their parent is Open Recent). */
	count = sizeof(menu_items) / sizeof(menu_items[0]);
	memcpy(entries, menu_items, sizeof(menu_items));
	memcpy(entries + count, menu->recent, sizeof(menu->recent[0]) * menu->recent_count);
	count += menu->recent_count;

	/* libkeiland sends what changed. */
	error = kl_window_set_menu(menu->window->kui, entries, count);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sets an action's state: enabled or greyed, checked or not (a refusal is logged). */
static void
menu_action_state(
	struct te_menu *menu,
	uint32_t action,
	int enabled,
	int checked)
{
	unsigned state;
	int error;

	/* The bits. */
	state = 0U;
	if (!enabled)
		state |= KL_ACTION_DISABLED;
	if (checked)
		state |= KL_ACTION_CHECKED;

	/* Kept by the window for its menu, its controls and its popups. */
	error = kl_window_set_action_state(menu->window->kui, action, state);
	if (error != 0 && error != ENOTSUP)
		te_log("MENU update-failed errno=%d", error);
}
