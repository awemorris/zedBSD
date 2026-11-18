/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of files: File, Edit, View, Go, Window and Help
 * (spec §36, §37).
 *
 * The compositor draws them (in the window's floating title bar, or in the
 * system bar while the window is docked) from the table given to
 * libkeiland's window (kl_window_set_menu, WS131 p020; the System Menu,
 * WS070).  A choice arrives among the window's inputs as a
 * KL_WINDOW_ACTION input; it is queued with them (FM_EVENT_ACTION, so a
 * shortcut and the keys typed after it are carried out in their order) and
 * carried out by fm_ui_action, after which the main loop tells the menus
 * the window's state as the actions' states and the table's labels.
 * Without the System Menu the window has no menus, and the keys still
 * work.  A context menu (ws071-p009) is a table of its own shown at the
 * press (kl_window_popup_menu), its rows' actions moved past the menus'
 * (FM_CONTEXT_ACTION) so that their states are their own.
 *
 * Only shortcuts with Ctrl or Alt are given to the compositor, which takes such a
 * key for the menu while its item is enabled; F2, Delete, Space, Enter
 * and the arrows stay the window's own, so a text field keeps them.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The top-level menus. */
#define MENU_FILE		1U
#define MENU_EDIT		2U
#define MENU_VIEW		3U
#define MENU_GO			4U
#define MENU_WINDOW		5U
#define MENU_HELP		6U

/* The submenus. */
#define MENU_OPEN_WITH		10U
#define MENU_SORT		12U
#define MENU_COLUMNS		13U
#define MENU_ALWAYS_WITH	14U

/* The items that do nothing yet (the views and the place of later versions). */
#define MENU_VIEW_COLUMNS	20U
#define MENU_VIEW_GALLERY	21U
#define MENU_GO_NETWORK		22U

/* The separators' IDs start here. */
#define MENU_LINE		30U

/* The separator of Always Open With, before Use System Default (past the fixed separators). */
#define MENU_ALWAYS_LINE	41U

/*
 * The actions of the items that are not the interface's: the two Open
 * With submenus (greyed without ways to open the selection) and the items
 * of later versions (always greyed).
 */
#define MENU_ACTION_OPEN_WITH	0x8000U
#define MENU_ACTION_ALWAYS_WITH	0x8001U
#define MENU_ACTION_LATER	0x8002U

/* An item that carries out an action has the action's ID moved past the others. */
#define MENU_ACTION_ID(action)	(1000U + (uint32_t)(action))

/* The keysyms of the shortcuts' keys that are not letters or digits. */
#define MENU_KEY_LEFT		0xff51U
#define MENU_KEY_UP		0xff52U
#define MENU_KEY_RIGHT		0xff53U
#define MENU_KEY_PAGE_UP	0xff55U
#define MENU_KEY_PAGE_DOWN	0xff56U

/* The modifiers of the shortcuts. */
#define MENU_CTRL		KL_MENU_CTRL
#define MENU_CTRL_SHIFT		(KL_MENU_CTRL | KL_MENU_SHIFT)
#define MENU_CTRL_ALT		(KL_MENU_CTRL | KL_MENU_ALT)
#define MENU_ALT		KL_MENU_ALT

/* The list columns the View menu names, after the name (which is always shown). */
#define MENU_COLUMN_COUNT	5

/*
 * The fixed items, in the order they are shown.  The ways to open the
 * selection and the list columns are added after them by
 * menu_build.
 */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_NEW_WINDOW), MENU_FILE, KL_MENU_ITEM_NORMAL, "New Window", FM_ACTION_NEW_WINDOW, KL_MENU_ROLE_NEW, MENU_CTRL, 'n' },
	{ MENU_ACTION_ID(FM_ACTION_NEW_TAB), MENU_FILE, KL_MENU_ITEM_NORMAL, "New Tab", FM_ACTION_NEW_TAB, KL_MENU_ROLE_NONE, MENU_CTRL, 't' },
	{ MENU_ACTION_ID(FM_ACTION_NEW_FOLDER), MENU_FILE, KL_MENU_ITEM_NORMAL, "New Folder", FM_ACTION_NEW_FOLDER, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'n' },
	{ MENU_ACTION_ID(FM_ACTION_OPEN), MENU_FILE, KL_MENU_ITEM_NORMAL, "Open", FM_ACTION_OPEN, KL_MENU_ROLE_OPEN, MENU_CTRL, 'o' },
	{ MENU_OPEN_WITH, MENU_FILE, KL_MENU_ITEM_SUBMENU, "Open With", MENU_ACTION_OPEN_WITH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ALWAYS_WITH, MENU_FILE, KL_MENU_ITEM_SUBMENU, "Always Open With", MENU_ACTION_ALWAYS_WITH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_GET_INFO), MENU_FILE, KL_MENU_ITEM_NORMAL, "Get Info", FM_ACTION_GET_INFO, KL_MENU_ROLE_NONE, MENU_CTRL, 'i' },
	{ MENU_ACTION_ID(FM_ACTION_TRASH), MENU_FILE, KL_MENU_ITEM_NORMAL, "Move to Trash", FM_ACTION_TRASH, KL_MENU_ROLE_DELETE, 0U, 0U },
	{ MENU_LINE + 1U, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_CLOSE_TAB), MENU_FILE, KL_MENU_ITEM_NORMAL, "Close Tab", FM_ACTION_CLOSE_TAB, KL_MENU_ROLE_NONE, MENU_CTRL, 'w' },
	{ MENU_ACTION_ID(FM_ACTION_CLOSE_WINDOW), MENU_FILE, KL_MENU_ITEM_NORMAL, "Close Window", FM_ACTION_CLOSE_WINDOW, KL_MENU_ROLE_CLOSE, MENU_CTRL_SHIFT, 'w' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_UNDO), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Undo", FM_ACTION_UNDO, KL_MENU_ROLE_UNDO, MENU_CTRL, 'z' },
	{ MENU_ACTION_ID(FM_ACTION_REDO), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Redo", FM_ACTION_REDO, KL_MENU_ROLE_REDO, MENU_CTRL_SHIFT, 'z' },
	{ MENU_LINE + 2U, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_CUT), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Cut", FM_ACTION_CUT, KL_MENU_ROLE_CUT, MENU_CTRL, 'x' },
	{ MENU_ACTION_ID(FM_ACTION_COPY), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Copy", FM_ACTION_COPY, KL_MENU_ROLE_COPY, MENU_CTRL, 'c' },
	{ MENU_ACTION_ID(FM_ACTION_PASTE), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Paste", FM_ACTION_PASTE, KL_MENU_ROLE_PASTE, MENU_CTRL, 'v' },
	{ MENU_ACTION_ID(FM_ACTION_DUPLICATE), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Duplicate", FM_ACTION_DUPLICATE, KL_MENU_ROLE_NONE, MENU_CTRL, 'd' },
	{ MENU_ACTION_ID(FM_ACTION_SELECT_ALL), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Select All", FM_ACTION_SELECT_ALL, KL_MENU_ROLE_SELECT_ALL, MENU_CTRL, 'a' },
	{ MENU_LINE + 3U, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_RENAME), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Rename", FM_ACTION_RENAME, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_VIEW_ICONS), MENU_VIEW, KL_MENU_ITEM_RADIO, "Icons", FM_ACTION_VIEW_ICONS, KL_MENU_ROLE_NONE, MENU_CTRL, '1' },
	{ MENU_ACTION_ID(FM_ACTION_VIEW_LIST), MENU_VIEW, KL_MENU_ITEM_RADIO, "List", FM_ACTION_VIEW_LIST, KL_MENU_ROLE_NONE, MENU_CTRL, '2' },
	{ MENU_VIEW_COLUMNS, MENU_VIEW, KL_MENU_ITEM_RADIO, "Columns", MENU_ACTION_LATER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW_GALLERY, MENU_VIEW, KL_MENU_ITEM_RADIO, "Gallery", MENU_ACTION_LATER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE + 4U, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SORT, MENU_VIEW, KL_MENU_ITEM_SUBMENU, "Sort By", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SORT_NAME), MENU_SORT, KL_MENU_ITEM_RADIO, "Name", FM_ACTION_SORT_NAME, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SORT_KIND), MENU_SORT, KL_MENU_ITEM_RADIO, "Kind", FM_ACTION_SORT_KIND, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SORT_SIZE), MENU_SORT, KL_MENU_ITEM_RADIO, "Size", FM_ACTION_SORT_SIZE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SORT_MODIFIED), MENU_SORT, KL_MENU_ITEM_RADIO, "Date Modified", FM_ACTION_SORT_MODIFIED, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_COLUMNS, MENU_VIEW, KL_MENU_ITEM_SUBMENU, "List Columns", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE + 5U, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SHOW_SIDEBAR), MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Show Sidebar", FM_ACTION_SHOW_SIDEBAR, KL_MENU_ROLE_NONE, MENU_CTRL_ALT, 's' },
	{ MENU_ACTION_ID(FM_ACTION_SHOW_PREVIEW), MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Show Preview", FM_ACTION_SHOW_PREVIEW, KL_MENU_ROLE_NONE, MENU_CTRL_ALT, 'p' },
	{ MENU_ACTION_ID(FM_ACTION_SHOW_HIDDEN), MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Show Hidden Files", FM_ACTION_SHOW_HIDDEN, KL_MENU_ROLE_NONE, MENU_CTRL, 'h' },
	{ MENU_GO, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Go", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_BACK), MENU_GO, KL_MENU_ITEM_NORMAL, "Back", FM_ACTION_BACK, KL_MENU_ROLE_NONE, MENU_ALT, MENU_KEY_LEFT },
	{ MENU_ACTION_ID(FM_ACTION_FORWARD), MENU_GO, KL_MENU_ITEM_NORMAL, "Forward", FM_ACTION_FORWARD, KL_MENU_ROLE_NONE, MENU_ALT, MENU_KEY_RIGHT },
	{ MENU_ACTION_ID(FM_ACTION_ENCLOSING), MENU_GO, KL_MENU_ITEM_NORMAL, "Enclosing Folder", FM_ACTION_ENCLOSING, KL_MENU_ROLE_NONE, MENU_CTRL, MENU_KEY_UP },
	{ MENU_LINE + 6U, MENU_GO, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_GO_HOME), MENU_GO, KL_MENU_ITEM_NORMAL, "Home", FM_ACTION_GO_HOME, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'h' },
	{ MENU_ACTION_ID(FM_ACTION_GO_DESKTOP), MENU_GO, KL_MENU_ITEM_NORMAL, "Desktop", FM_ACTION_GO_DESKTOP, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'd' },
	{ MENU_ACTION_ID(FM_ACTION_GO_DOCUMENTS), MENU_GO, KL_MENU_ITEM_NORMAL, "Documents", FM_ACTION_GO_DOCUMENTS, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'o' },
	{ MENU_ACTION_ID(FM_ACTION_GO_DOWNLOADS), MENU_GO, KL_MENU_ITEM_NORMAL, "Downloads", FM_ACTION_GO_DOWNLOADS, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'l' },
	{ MENU_ACTION_ID(FM_ACTION_GO_RECENTS), MENU_GO, KL_MENU_ITEM_NORMAL, "Recents", FM_ACTION_GO_RECENTS, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'r' },
	{ MENU_ACTION_ID(FM_ACTION_GO_COMPUTER), MENU_GO, KL_MENU_ITEM_NORMAL, "Computer", FM_ACTION_GO_COMPUTER, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'c' },
	{ MENU_GO_NETWORK, MENU_GO, KL_MENU_ITEM_NORMAL, "Network", MENU_ACTION_LATER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_GO_TRASH), MENU_GO, KL_MENU_ITEM_NORMAL, "Trash", FM_ACTION_GO_TRASH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE + 7U, MENU_GO, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_GO_LOCATION), MENU_GO, KL_MENU_ITEM_NORMAL, "Go to Location...", FM_ACTION_GO_LOCATION, KL_MENU_ROLE_NONE, MENU_CTRL, 'l' },
	{ MENU_ACTION_ID(FM_ACTION_FIND), MENU_GO, KL_MENU_ITEM_NORMAL, "Find", FM_ACTION_FIND, KL_MENU_ROLE_FIND, MENU_CTRL, 'f' },
	{ MENU_WINDOW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Window", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_MINIMIZE), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Minimize", FM_ACTION_MINIMIZE, KL_MENU_ROLE_NONE, MENU_CTRL, 'm' },
	{ MENU_ACTION_ID(FM_ACTION_ZOOM), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Zoom", FM_ACTION_ZOOM, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE + 9U, MENU_WINDOW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_PREVIOUS_TAB), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Previous Tab", FM_ACTION_PREVIOUS_TAB, KL_MENU_ROLE_NONE, MENU_CTRL, MENU_KEY_PAGE_UP },
	{ MENU_ACTION_ID(FM_ACTION_NEXT_TAB), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Next Tab", FM_ACTION_NEXT_TAB, KL_MENU_ROLE_NONE, MENU_CTRL, MENU_KEY_PAGE_DOWN },
	{ MENU_HELP, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Help", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_HELP), MENU_HELP, KL_MENU_ITEM_NORMAL, "File Manager Help", FM_ACTION_HELP, KL_MENU_ROLE_HELP, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_SHORTCUTS), MENU_HELP, KL_MENU_ITEM_NORMAL, "Keyboard Shortcuts", FM_ACTION_SHORTCUTS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LINE + 8U, MENU_HELP, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(FM_ACTION_ABOUT), MENU_HELP, KL_MENU_ITEM_NORMAL, "About Files", FM_ACTION_ABOUT, KL_MENU_ROLE_ABOUT, 0U, 0U }
};

/* The list columns the View menu names, in the list's order. */
static const unsigned menu_columns[MENU_COLUMN_COUNT] = {
	FM_COLUMN_KIND,
	FM_COLUMN_SIZE,
	FM_COLUMN_MODIFIED,
	FM_COLUMN_CHANGED,
	FM_COLUMN_OWNER
};

/* The labels of those columns. */
static const char *const menu_column_labels[MENU_COLUMN_COUNT] = {
	"Kind",
	"Size",
	"Date Modified",
	"Changed",
	"Owner"
};

/* How many items the menus have: the fixed ones, the ways to open (twice), a line, Use System Default, the columns. */
#define MENU_ENTRIES	(sizeof(menu_items) / sizeof(menu_items[0]) + 2U * FM_OPENERS + 2U + MENU_COLUMN_COUNT)

static size_t menu_build(const struct fm_menu_state *state, struct kl_menu_entry *entries);
static void menu_state(struct fm_menu *menu, const struct fm_menu_state *state);
static void menu_action_state(struct fm_menu *menu, uint32_t action, int enabled, int checked);
static void menu_action_hidden(struct fm_menu *menu, uint32_t action, int shown);
static int menu_same(unsigned value, unsigned named);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the window then
 * has no menus), or an errno value when the menus could not be made.  The
 * desktop's surface, which is not a window, has only the context menus.
 */
int
fm_menu_open(
	struct fm_menu *menu,
	struct fm_window *window,
	const struct fm_menu_state *state)
{
	struct kl_menu_entry entries[MENU_ENTRIES];
	size_t count;
	int error;

	/* Nothing yet but the window the choices go to. */
	memset(menu, 0, sizeof(*menu));
	menu->window = window;
	window->menu = menu;

	/* The desktop (files --desktop) has no window's menus, only the context menus. */
	if (window->desktop) {
		fm_log("MENU context-only");
		return 0;
	}

	/* The table; a compositor without the System Menu leaves the window without menus. */
	count = menu_build(state, entries);
	error = kl_window_set_menu(window->kui, entries, count);
	if (error == ENOTSUP) {
		fm_log("MENU none errno=%d", error);
		return 0;
	}
	if (error != 0)
		return error;
	menu->shown_once = 1;

	/* The state it shows. */
	menu_state(menu, state);

	/* Succeeded: the menus are the compositor's to show. */
	fm_log("MENU ready items=%u", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));
	return 0;
}

/*
 * Opens a context menu at a point of the window (a right press's, or a
 * drop's), for the press or the drop the window had last.  A compositor
 * without context menus shows none, which the log says.
 */
void
fm_menu_context(
	struct fm_menu *menu,
	const struct fm_context *context,
	int x,
	int y,
	uint32_t context_serial)
{
	static const unsigned types[] = {
		KL_MENU_ITEM_NORMAL,
		KL_MENU_ITEM_SEPARATOR,
		KL_MENU_ITEM_CHECKBOX,
		KL_MENU_ITEM_NORMAL,
		KL_MENU_ITEM_SUBMENU
	};
	struct kl_menu_entry entries[FM_CONTEXT_ROWS];
	const struct fm_context_row *row;
	uint32_t action;
	unsigned index;
	int error;

	/* With nothing to show, nothing opens. */
	(void)context_serial;
	if (context->count == 0U)
		return;

	/* Each row, its action moved past the menus' with its own state. */
	memset(entries, 0, sizeof(entries));
	for (index = 0; index < context->count && index < FM_CONTEXT_ROWS; index++) {
		row = &context->rows[index];
		entries[index].id = row->id;
		entries[index].parent = row->parent;
		entries[index].type = types[row->kind];
		entries[index].label = row->label;
		action = 0U;
		if (row->action != 0U) {
			action = FM_CONTEXT_ACTION + row->action;
			menu_action_state(menu, action, row->enabled, row->checked);
		}
		entries[index].action = action;
	}

	/* The compositor shows it at the press (or, for a drop's choice, at the drop). */
	menu->context_done = 0;
	error = kl_window_popup_menu(menu->window->kui, entries, index, x, y);
	if (error != 0) {
		fm_log("CONTEXT-MENU none errno=%d", error);
		return;
	}

	/* The log line the tests read. */
	fm_log("CONTEXT-MENU open rows=%u x=%d y=%d serial=%u", context->count, x, y, menu->window->button_serial);
}

/*
 * Queues an item chosen in the menus or in a context menu (a
 * KL_WINDOW_ACTION input of the window: its action and the item's ID)
 * among the window's inputs.
 */
void
fm_menu_chosen(
	struct fm_menu *menu,
	const struct kl_window_event *event)
{
	uint32_t action;

	/* A context menu's row: its own action. */
	if (event->code > FM_CONTEXT_ACTION) {
		action = event->code - FM_CONTEXT_ACTION;
		fm_log("CONTEXT-MENU item=%d action=%u serial=%u", (int)event->id, action, event->serial);
		fm_window_action(menu->window, action);
		return;
	}

	/* An item of the menus, after the inputs that came before it. */
	fm_log("MENU item=%d action=%u serial=%u", (int)event->id, event->code, event->serial);
	fm_window_action(menu->window, event->code);
}

/*
 * Tells the menus the window's state when it differs from what they show
 * (the table again when the ways to open the selection changed their
 * names).
 */
void
fm_menu_refresh(
	struct fm_menu *menu,
	const struct fm_menu_state *state)
{
	struct kl_menu_entry entries[MENU_ENTRIES];
	size_t count;
	int same;
	int error;

	/* A context menu the compositor closed is over now. */
	menu->context_done = 0;

	/* Without menus nothing is sent; nor when the state is the one the menus show. */
	if (!menu->shown_once)
		return;
	same = memcmp(state, &menu->shown, sizeof(*state));
	if (menu->sent && same == 0)
		return;

	/* The table with the ways' names (libkeiland sends only what changed). */
	count = menu_build(state, entries);
	error = kl_window_set_menu(menu->window->kui, entries, count);
	if (error != 0)
		fm_log("MENU update-failed errno=%d", error);

	/* The state. */
	menu_state(menu, state);
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
fm_menu_close(
	struct fm_menu *menu)
{
	/* The window's menu, where shown; the window's inputs go nowhere here any more. */
	if (menu->shown_once &&
	    menu->window != NULL &&
	    menu->window->kui != NULL)
		(void)kl_window_set_menu(menu->window->kui, NULL, 0U);
	if (menu->window != NULL)
		menu->window->menu = NULL;

	/* Nothing is left. */
	memset(menu, 0, sizeof(*menu));
}

/* Fills the table of the menus: the fixed items, the ways to open the selection (named by the state), Use System Default, the columns; returns how many. */
static size_t
menu_build(
	const struct fm_menu_state *state,
	struct kl_menu_entry *entries)
{
	struct kl_menu_entry *entry;
	size_t count;
	unsigned index;

	/* The fixed items in their order. */
	count = sizeof(menu_items) / sizeof(menu_items[0]);
	memcpy(entries, menu_items, sizeof(menu_items));

	/* The ways to open the selection, each named (the unused ones hidden by their state). */
	for (index = 0; index < FM_OPENERS; index++) {
		entry = &entries[count++];
		memset(entry, 0, sizeof(*entry));
		entry->id = MENU_ACTION_ID(FM_ACTION_OPEN_WITH_FIRST + index);
		entry->parent = MENU_OPEN_WITH;
		entry->type = KL_MENU_ITEM_NORMAL;
		entry->label = "-";
		if ((int)index < state->opener_count)
			entry->label = state->openers[index];
		entry->action = FM_ACTION_OPEN_WITH_FIRST + index;
	}

	/* The ways that can become the default of the selection's type (ws093-p003). */
	for (index = 0; index < FM_OPENERS; index++) {
		entry = &entries[count++];
		memset(entry, 0, sizeof(*entry));
		entry->id = MENU_ACTION_ID(FM_ACTION_ALWAYS_WITH_FIRST + index);
		entry->parent = MENU_ALWAYS_WITH;
		entry->type = KL_MENU_ITEM_NORMAL;
		entry->label = "-";
		if ((int)index < state->opener_count)
			entry->label = state->openers[index];
		entry->action = FM_ACTION_ALWAYS_WITH_FIRST + index;
	}

	/* A line, then the way back to the system's default. */
	entry = &entries[count++];
	memset(entry, 0, sizeof(*entry));
	entry->id = MENU_ALWAYS_LINE;
	entry->parent = MENU_ALWAYS_WITH;
	entry->type = KL_MENU_ITEM_SEPARATOR;
	entry->label = "";
	entry = &entries[count++];
	memset(entry, 0, sizeof(*entry));
	entry->id = MENU_ACTION_ID(FM_ACTION_USE_SYSTEM_DEFAULT);
	entry->parent = MENU_ALWAYS_WITH;
	entry->type = KL_MENU_ITEM_NORMAL;
	entry->label = "Use System Default";
	entry->action = FM_ACTION_USE_SYSTEM_DEFAULT;

	/* The list columns. */
	for (index = 0; index < MENU_COLUMN_COUNT; index++) {
		entry = &entries[count++];
		memset(entry, 0, sizeof(*entry));
		entry->id = MENU_ACTION_ID(FM_ACTION_COLUMN_FIRST + menu_columns[index]);
		entry->parent = MENU_COLUMNS;
		entry->type = KL_MENU_ITEM_CHECKBOX;
		entry->label = menu_column_labels[index];
		entry->action = FM_ACTION_COLUMN_FIRST + menu_columns[index];
	}

	/* Succeeded: the table. */
	return count;
}

/* Shows a state in the menus as the actions' states. */
static void
menu_state(
	struct fm_menu *menu,
	const struct fm_menu_state *state)
{
	unsigned index;
	unsigned bit;
	int selected;
	int editable;
	int renamable;
	int openers;
	int undo;
	int redo;
	int paste;
	int shown;
	int more_tabs;
	int tabs;
	int column;

	/* A selection to act on, outside a text field (whose keys stay its own) and outside the trash. */
	selected = 0;
	if (state->selection > 0 && state->field == 0)
		selected = 1;
	editable = selected;
	if (state->trash != 0)
		editable = 0;

	/* One item to rename, and ways to open the selection. */
	renamable = 0;
	if (editable != 0 && state->selection == 1)
		renamable = 1;
	openers = 0;
	if (state->opener_count > 0)
		openers = 1;

	/* The histories and the clipboard, which a text field's own keys leave alone. */
	undo = 0;
	redo = 0;
	paste = 0;
	if (state->field == 0) {
		undo = state->can_undo;
		redo = state->can_redo;
		paste = state->can_paste;
	}

	/* File, and Edit: the histories, the clipboard and the selection. */
	menu_action_state(menu, FM_ACTION_NEW_FOLDER, state->folder, 0);
	menu_action_state(menu, FM_ACTION_OPEN, selected, 0);
	menu_action_state(menu, MENU_ACTION_OPEN_WITH, openers, 0);
	menu_action_state(menu, FM_ACTION_TRASH, editable, 0);
	menu_action_state(menu, FM_ACTION_UNDO, undo, 0);
	menu_action_state(menu, FM_ACTION_REDO, redo, 0);
	menu_action_state(menu, FM_ACTION_CUT, editable, 0);
	menu_action_state(menu, FM_ACTION_COPY, selected, 0);
	menu_action_state(menu, FM_ACTION_PASTE, paste, 0);
	menu_action_state(menu, FM_ACTION_DUPLICATE, editable, 0);
	menu_action_state(menu, FM_ACTION_RENAME, renamable, 0);

	/* View: the view and the sort as radio items, the panels as checkboxes; the views of later versions off. */
	menu_action_state(menu, FM_ACTION_VIEW_ICONS, 1, menu_same(state->view, FM_VIEW_ICONS));
	menu_action_state(menu, FM_ACTION_VIEW_LIST, 1, menu_same(state->view, FM_VIEW_LIST));
	menu_action_state(menu, MENU_ACTION_LATER, 0, 0);
	menu_action_state(menu, FM_ACTION_SORT_NAME, 1, menu_same(state->sort, FM_SORT_NAME));
	menu_action_state(menu, FM_ACTION_SORT_KIND, 1, menu_same(state->sort, FM_SORT_KIND));
	menu_action_state(menu, FM_ACTION_SORT_SIZE, 1, menu_same(state->sort, FM_SORT_SIZE));
	menu_action_state(menu, FM_ACTION_SORT_MODIFIED, 1, menu_same(state->sort, FM_SORT_MODIFIED));
	menu_action_state(menu, FM_ACTION_SHOW_SIDEBAR, 1, state->sidebar);
	menu_action_state(menu, FM_ACTION_SHOW_PREVIEW, 1, state->preview);
	menu_action_state(menu, FM_ACTION_SHOW_HIDDEN, 1, state->hidden);

	/* Go and Window: the history's steps, the folder above, the tabs (room for another, others to go to). */
	more_tabs = 0;
	if (state->tabs < FM_TABS)
		more_tabs = 1;
	tabs = 0;
	if (state->tabs > 1)
		tabs = 1;
	menu_action_state(menu, FM_ACTION_BACK, state->can_back, 0);
	menu_action_state(menu, FM_ACTION_FORWARD, state->can_forward, 0);
	menu_action_state(menu, FM_ACTION_NEW_TAB, more_tabs, 0);
	menu_action_state(menu, FM_ACTION_PREVIOUS_TAB, tabs, 0);
	menu_action_state(menu, FM_ACTION_NEXT_TAB, tabs, 0);
	menu_action_state(menu, FM_ACTION_ENCLOSING, state->can_enclose, 0);

	/* The ways to open the selection, the rest hidden, in both submenus (ws093-p003). */
	for (index = 0; index < FM_OPENERS; index++) {
		shown = 0;
		if ((int)index < state->opener_count)
			shown = 1;
		menu_action_hidden(menu, FM_ACTION_OPEN_WITH_FIRST + index, shown);
		menu_action_hidden(menu, FM_ACTION_ALWAYS_WITH_FIRST + index, shown);
	}
	menu_action_state(menu, MENU_ACTION_ALWAYS_WITH, openers, 0);
	menu_action_state(menu, FM_ACTION_USE_SYSTEM_DEFAULT, state->user_default, 0);

	/* The list columns shown. */
	for (index = 0; index < MENU_COLUMN_COUNT; index++) {
		bit = 1U << menu_columns[index];
		column = 0;
		if ((state->columns & bit) != 0U)
			column = 1;
		menu_action_state(menu, FM_ACTION_COLUMN_FIRST + menu_columns[index], 1, column);
	}

	/* Succeeded: the menus show the state. */
	menu->shown = *state;
	menu->sent = 1;
	fm_log("MENU state selection=%d folder=%d paste=%d undo=%d back=%d view=%u sort=%u openers=%d", state->selection, state->folder, state->can_paste, state->can_undo, state->can_back, state->view, state->sort, state->opener_count);
}

/* Sets an action's state (enabled, checked) in the window's menus and context menus. */
static void
menu_action_state(
	struct fm_menu *menu,
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

	/* Kept by the window. */
	error = kl_window_set_action_state(menu->window->kui, action, state);
	if (error != 0 && error != ENOTSUP)
		fm_log("MENU update-failed errno=%d", error);
}

/* Shows or hides an action's item (enabled while shown). */
static void
menu_action_hidden(
	struct fm_menu *menu,
	uint32_t action,
	int shown)
{
	int error;

	/* Shown, or hidden. */
	if (shown) {
		error = kl_window_set_action_state(menu->window->kui, action, 0U);
	} else {
		error = kl_window_set_action_state(menu->window->kui, action, KL_ACTION_HIDDEN);
	}
	if (error != 0 && error != ENOTSUP)
		fm_log("MENU update-failed errno=%d", error);
}

/* Tells whether a value is the one an item names (1) or not (0). */
static int
menu_same(
	unsigned value,
	unsigned named)
{
	/* The item's value. */
	if (value == named)
		return 1;

	/* Another value. */
	return 0;
}
