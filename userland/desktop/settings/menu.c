/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of Settings: File, Edit (Find), View, Go (the history and every page, by
 * group), Window and Help.
 *
 * The compositor draws them (in the window's floating title bar, or in the
 * system bar while the window is docked) from the table given to
 * libkeiland's window (kl_window_set_menu, WS131 p019; the System Menu,
 * WS070).  A choice arrives among the window's inputs as a
 * KL_WINDOW_ACTION input; it is queued with them (SE_EVENT_ACTION,
 * window.c) and carried out by se_ui_action, after which the main loop
 * tells the menus the window's state as the actions' states.  Without the
 * System Menu the window has no menus, and the keys still work.
 */

#include "window.h"

#include <errno.h>
#include <string.h>

/* The top-level menus. */
#define MENU_FILE		1U
#define MENU_VIEW		2U
#define MENU_GO			3U
#define MENU_WINDOW		4U
#define MENU_HELP		5U
#define MENU_EDIT		6U

/* The Go menu's submenus, one a group of pages. */
#define MENU_GROUP_FIRST	10U

/* The separators' IDs start here. */
#define MENU_LINE		30U

/* An item that carries out an action has the action's ID moved past the others. */
#define MENU_ACTION_ID(action)	(1000U + (uint32_t)(action))

/* The keysyms of the shortcuts' keys that are not letters. */
#define MENU_KEY_LEFT		0xff51U
#define MENU_KEY_RIGHT		0xff53U

/* The modifiers of the shortcuts. */
#define MENU_CTRL		KL_MENU_CTRL
#define MENU_CTRL_SHIFT		(KL_MENU_CTRL | KL_MENU_SHIFT)
#define MENU_CTRL_ALT		(KL_MENU_CTRL | KL_MENU_ALT)
#define MENU_ALT		KL_MENU_ALT

/*
 * The fixed items, in the order they are shown.  The pages are added
 * after them, under Go's groups, by menu_build.
 */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_CLOSE_WINDOW), MENU_FILE, KL_MENU_ITEM_NORMAL, "Close Window", SE_ACTION_CLOSE_WINDOW, KL_MENU_ROLE_CLOSE, MENU_CTRL, 'w' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_FIND), MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find", SE_ACTION_FIND, KL_MENU_ROLE_FIND, MENU_CTRL, 'f' },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_SHOW_SIDEBAR), MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Show Sidebar", SE_ACTION_SHOW_SIDEBAR, KL_MENU_ROLE_NONE, MENU_CTRL_ALT, 's' },
	{ MENU_GO, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Go", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_BACK), MENU_GO, KL_MENU_ITEM_NORMAL, "Back", SE_ACTION_BACK, KL_MENU_ROLE_NONE, MENU_ALT, MENU_KEY_LEFT },
	{ MENU_ACTION_ID(SE_ACTION_FORWARD), MENU_GO, KL_MENU_ITEM_NORMAL, "Forward", SE_ACTION_FORWARD, KL_MENU_ROLE_NONE, MENU_ALT, MENU_KEY_RIGHT },
	{ MENU_ACTION_ID(SE_ACTION_HOME), MENU_GO, KL_MENU_ITEM_NORMAL, "Home", SE_ACTION_HOME, KL_MENU_ROLE_NONE, MENU_CTRL_SHIFT, 'h' },
	{ MENU_LINE, MENU_GO, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GROUP_FIRST + SE_GROUP_CONNECTIVITY, MENU_GO, KL_MENU_ITEM_SUBMENU, "Connectivity", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GROUP_FIRST + SE_GROUP_PERSONALIZATION, MENU_GO, KL_MENU_ITEM_SUBMENU, "Personalization", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GROUP_FIRST + SE_GROUP_DEVICES, MENU_GO, KL_MENU_ITEM_SUBMENU, "Devices", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GROUP_FIRST + SE_GROUP_SYSTEM, MENU_GO, KL_MENU_ITEM_SUBMENU, "System", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_WINDOW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Window", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_MINIMIZE), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Minimize", SE_ACTION_MINIMIZE, KL_MENU_ROLE_NONE, MENU_CTRL, 'm' },
	{ MENU_ACTION_ID(SE_ACTION_ZOOM), MENU_WINDOW, KL_MENU_ITEM_NORMAL, "Zoom", SE_ACTION_ZOOM, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_HELP, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Help", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTION_ID(SE_ACTION_ABOUT), MENU_HELP, KL_MENU_ITEM_NORMAL, "About Kei", SE_ACTION_ABOUT, KL_MENU_ROLE_ABOUT, 0U, 0U }
};

/* How many items the menus have: the fixed ones, and a page's after Home. */
#define MENU_ENTRIES	(sizeof(menu_items) / sizeof(menu_items[0]) + SE_PAGES - 1U)

static size_t menu_build(struct kl_menu_entry *entries);
static void menu_action_state(struct se_menu *menu, uint32_t action, int enabled, int checked);
static int menu_same(unsigned value, unsigned named);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the window then
 * has no menus), or an errno value when the menus could not be made.
 */
int
se_menu_open(
	struct se_menu *menu,
	struct se_window *window,
	const struct se_menu_state *state)
{
	struct kl_menu_entry entries[MENU_ENTRIES];
	size_t count;
	int error;

	/* Nothing yet but the window the choices go to. */
	memset(menu, 0, sizeof(*menu));
	menu->window = window;

	/* The fixed items, then each page under its group; a compositor without the System Menu leaves the window without menus. */
	count = menu_build(entries);
	error = kl_window_set_menu(window->kui, entries, count);
	if (error == ENOTSUP) {
		se_log("MENU none errno=%d", error);
		return 0;
	}
	if (error != 0)
		return error;
	menu->shown_once = 1;

	/* The state it shows. */
	se_menu_refresh(menu, state);

	/* Succeeded: the menus are the compositor's to show. */
	se_log("MENU ready items=%u", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));
	return 0;
}

/*
 * Tells the menus the window's state when it differs from what they show:
 * the history's steps, the list of pages, and the page shown (checked
 * among the pages).
 */
void
se_menu_refresh(
	struct se_menu *menu,
	const struct se_menu_state *state)
{
	unsigned id;
	int same;

	/* Without menus nothing is sent; nor when the state is the one the menus show. */
	if (!menu->shown_once)
		return;
	same = memcmp(state, &menu->shown, sizeof(*state));
	if (menu->sent != 0 && same == 0)
		return;

	/* The history's steps, and the list of pages. */
	menu_action_state(menu, SE_ACTION_BACK, state->can_back, 0);
	menu_action_state(menu, SE_ACTION_FORWARD, state->can_forward, 0);
	menu_action_state(menu, SE_ACTION_SHOW_SIDEBAR, 1, state->sidebar);

	/* The page shown, checked among the pages. */
	for (id = SE_PAGE_HOME + 1; id < SE_PAGES; id++)
		menu_action_state(menu, SE_ACTION_PAGE_FIRST + id, 1, menu_same(state->page, id));

	/* Succeeded: the menus show the state. */
	menu->shown = *state;
	menu->sent = 1;
	se_log("MENU state back=%d forward=%d sidebar=%d page=%s", state->can_back, state->can_forward, state->sidebar, se_pages[state->page].word);
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
se_menu_close(
	struct se_menu *menu)
{
	/* The window's menu, where shown. */
	if (menu->shown_once &&
	    menu->window != NULL &&
	    menu->window->kui != NULL)
		(void)kl_window_set_menu(menu->window->kui, NULL, 0U);

	/* Nothing is left. */
	memset(menu, 0, sizeof(*menu));
}

/* Fills the table of the menus: the fixed items, then each page after Home as a radio item under its group; returns how many. */
static size_t
menu_build(
	struct kl_menu_entry *entries)
{
	size_t count;
	unsigned id;

	/* The fixed items in their order. */
	count = sizeof(menu_items) / sizeof(menu_items[0]);
	memcpy(entries, menu_items, sizeof(menu_items));

	/* Each page after Home. */
	for (id = SE_PAGE_HOME + 1; id < SE_PAGES; id++) {
		memset(&entries[count], 0, sizeof(entries[count]));
		entries[count].id = MENU_ACTION_ID(SE_ACTION_PAGE_FIRST + id);
		entries[count].parent = MENU_GROUP_FIRST + se_pages[id].group;
		entries[count].type = KL_MENU_ITEM_RADIO;
		entries[count].label = se_pages[id].name;
		entries[count].action = SE_ACTION_PAGE_FIRST + id;
		count++;
	}

	/* Succeeded: the table. */
	return count;
}

/* Sets an action's state (enabled, checked) in the window's menu. */
static void
menu_action_state(
	struct se_menu *menu,
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

	/* Kept by the window for its menu. */
	error = kl_window_set_action_state(menu->window->kui, action, state);
	if (error != 0 && error != ENOTSUP)
		se_log("MENU update-failed errno=%d", error);
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
