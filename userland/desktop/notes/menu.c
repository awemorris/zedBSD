/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of Notes: File, Edit, Page, Tool and View (ws175-p008: Edit's
 * images and text of the PDF, and Tool's Select and Text).
 *
 * The compositor draws them from the table given to libkeiland's window
 * (kl_window_set_menu, WS131 p018; the System Menu, WS070) and runs their
 * shortcuts: a shortcut's key is the menu's and does not reach the window,
 * so the keys Notes handles itself (main.c) are the ones without a menu
 * item and every key when there is no System Menu.  A choice arrives among
 * the window's inputs as a KL_WINDOW_ACTION input and waits in the
 * window's queue for the main loop; the state shown is the actions'.
 *
 * The shortcuts are the standard ones (design-input-notes.md D8): Ctrl+N a
 * new page, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+W close, Ctrl+Z undo,
 * Ctrl+Shift+Z redo (Ctrl+Y too, as a key of Notes'), Page Up and Page Down
 * the previous and next page, F11 fullscreen.
 */

#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The top-level items. */
#define MENU_FILE		1U
#define MENU_EDIT		2U
#define MENU_PAGE		3U
#define MENU_TOOL		4U
#define MENU_VIEW		5U

/* File. */
#define MENU_NEW_PAGE		10U
#define MENU_OPEN		11U
#define MENU_SAVE		12U
#define MENU_FILE_LINE		13U
#define MENU_CLOSE		14U
#define MENU_SAVE_AS		15U
#define MENU_SAVE_CLEAN		16U
#define MENU_OPEN_CLEAN		17U

/* Edit (ws175-p008: the images of the PDF). */
#define MENU_UNDO		20U
#define MENU_REDO		21U
#define MENU_EDIT_LINE		22U
#define MENU_INSERT_IMAGE	23U
#define MENU_REPLACE_IMAGE	24U
#define MENU_DELETE_OBJECT	25U
#define MENU_RESET_OBJECT	26U
#define MENU_EDIT_TEXT		27U

/* Page. */
#define MENU_PREVIOUS_PAGE	30U
#define MENU_NEXT_PAGE		31U

/* Tool. */
#define MENU_PEN		40U
#define MENU_HIGHLIGHTER	41U
#define MENU_ERASER		42U
#define MENU_SELECT		43U
#define MENU_TEXT		44U

/* View. */
#define MENU_FULLSCREEN		50U

/* The keysyms of the shortcuts' keys that are not letters. */
#define MENU_KEY_PAGE_UP	0xff55U
#define MENU_KEY_PAGE_DOWN	0xff56U
#define MENU_KEY_F11		0xffc8U

/* The menus, in the order they are shown. */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_NEW_PAGE, MENU_FILE, KL_MENU_ITEM_NORMAL, "New Page", NOTES_ACTION_NEW_PAGE, KL_MENU_ROLE_NEW, KL_MENU_CTRL, 'n' },
	{ MENU_OPEN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Open...", NOTES_ACTION_OPEN, KL_MENU_ROLE_OPEN, KL_MENU_CTRL, 'o' },
	{ MENU_SAVE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Save", NOTES_ACTION_SAVE, KL_MENU_ROLE_SAVE, KL_MENU_CTRL, 's' },
	{ MENU_SAVE_AS, MENU_FILE, KL_MENU_ITEM_NORMAL, "Save As...", NOTES_ACTION_SAVE_AS, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 's' },
	{ MENU_SAVE_CLEAN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Save Clean Copy...", NOTES_ACTION_SAVE_CLEAN, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPEN_CLEAN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Open Clean Copy", NOTES_ACTION_OPEN_CLEAN, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FILE_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLOSE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Close", NOTES_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL, 'w' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_UNDO, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Undo", NOTES_ACTION_UNDO, KL_MENU_ROLE_UNDO, KL_MENU_CTRL, 'z' },
	{ MENU_REDO, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Redo", NOTES_ACTION_REDO, KL_MENU_ROLE_REDO, KL_MENU_CTRL | KL_MENU_SHIFT, 'z' },
	{ MENU_EDIT_LINE, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_EDIT_TEXT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Edit Text", NOTES_ACTION_EDIT_TEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_INSERT_IMAGE, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Insert Image...", NOTES_ACTION_INSERT_IMAGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_REPLACE_IMAGE, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Replace Image...", NOTES_ACTION_REPLACE_IMAGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_DELETE_OBJECT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Delete", NOTES_ACTION_DELETE_OBJECT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_RESET_OBJECT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Reset", NOTES_ACTION_RESET_OBJECT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PAGE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Page", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PREVIOUS_PAGE, MENU_PAGE, KL_MENU_ITEM_NORMAL, "Previous Page", NOTES_ACTION_PREVIOUS_PAGE, KL_MENU_ROLE_NONE, 0U, MENU_KEY_PAGE_UP },
	{ MENU_NEXT_PAGE, MENU_PAGE, KL_MENU_ITEM_NORMAL, "Next Page", NOTES_ACTION_NEXT_PAGE, KL_MENU_ROLE_NONE, 0U, MENU_KEY_PAGE_DOWN },
	{ MENU_TOOL, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Tool", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PEN, MENU_TOOL, KL_MENU_ITEM_RADIO, "Pen", NOTES_ACTION_PEN, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_HIGHLIGHTER, MENU_TOOL, KL_MENU_ITEM_RADIO, "Marker", NOTES_ACTION_HIGHLIGHTER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ERASER, MENU_TOOL, KL_MENU_ITEM_RADIO, "Eraser", NOTES_ACTION_ERASER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SELECT, MENU_TOOL, KL_MENU_ITEM_RADIO, "Select", NOTES_ACTION_SELECT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_TEXT, MENU_TOOL, KL_MENU_ITEM_RADIO, "Text", NOTES_ACTION_TEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FULLSCREEN, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Fullscreen", NOTES_ACTION_FULLSCREEN, KL_MENU_ROLE_FULLSCREEN, 0U, MENU_KEY_F11 }
};

static void menu_action_state(struct notes_window *window, uint32_t action, int enabled, int checked);
static int menu_is(uint32_t tool, uint32_t action);

/*
 * Gives the compositor the window's menus.
 *
 * Returns 0, or an errno value (ENOTSUP without the System Menu, when the
 * keys of main.c are the only shortcuts).
 */
int
notes_menu_open(
	struct notes_window *window)
{
	int error;

	/* The table, shown on the window. */
	error = kl_window_set_menu(window->kui, menu_items, sizeof(menu_items) / sizeof(menu_items[0]));
	if (error != 0)
		return error;

	/* Succeeded: the compositor shows the menus. */
	window->menu_shown = 1;
	return 0;
}

/*
 * Shows a state in the menus: Undo and Redo enabled, the images' and the
 * pages' items, the tool's radio item and Fullscreen checked.
 */
void
notes_menu_refresh(
	struct notes_window *window,
	const struct notes_ui_state *state)
{
	int earlier;
	int later;

	/* Nothing to show without a menu. */
	if (!window->menu_shown)
		return;

	/* Undo and Redo while the history allows, and the images' items while they can do something. */
	menu_action_state(window, NOTES_ACTION_UNDO, state->can_undo, 0);
	menu_action_state(window, NOTES_ACTION_REDO, state->can_redo, 0);
	menu_action_state(window, NOTES_ACTION_INSERT_IMAGE, state->can_insert, 0);
	menu_action_state(window, NOTES_ACTION_REPLACE_IMAGE, state->can_replace, 0);
	menu_action_state(window, NOTES_ACTION_DELETE_OBJECT, state->selected, 0);
	menu_action_state(window, NOTES_ACTION_RESET_OBJECT, state->can_reset, 0);
	menu_action_state(window, NOTES_ACTION_EDIT_TEXT, state->can_edit_text, 0);
	menu_action_state(window, NOTES_ACTION_OPEN_CLEAN, state->can_open_clean, 0);

	/* The page before and the page after, when there are such pages. */
	earlier = 0;
	if (state->page > 0U)
		earlier = 1;
	later = 0;
	if (state->page + 1U < state->page_count)
		later = 1;
	menu_action_state(window, NOTES_ACTION_PREVIOUS_PAGE, earlier, 0);
	menu_action_state(window, NOTES_ACTION_NEXT_PAGE, later, 0);

	/* The tool's radio item, and Fullscreen. */
	menu_action_state(window, NOTES_ACTION_PEN, 1, menu_is(state->tool, NOTES_ACTION_PEN));
	menu_action_state(window, NOTES_ACTION_HIGHLIGHTER, 1, menu_is(state->tool, NOTES_ACTION_HIGHLIGHTER));
	menu_action_state(window, NOTES_ACTION_ERASER, 1, menu_is(state->tool, NOTES_ACTION_ERASER));
	menu_action_state(window, NOTES_ACTION_SELECT, 1, menu_is(state->tool, NOTES_ACTION_SELECT));
	menu_action_state(window, NOTES_ACTION_TEXT, 1, menu_is(state->tool, NOTES_ACTION_TEXT));
	menu_action_state(window, NOTES_ACTION_FULLSCREEN, 1, state->fullscreen);
}

/*
 * Queues a choice of the menus (a KL_WINDOW_ACTION input of the window:
 * its action and the item's ID) for the main loop.
 */
void
notes_menu_chosen(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	/* The log line the tests read. */
	printf("NOTES MENU item=%d action=%u serial=%u\n", (int)event->id, event->code, event->serial);
	fflush(stdout);

	/* A full queue drops the choice. */
	if (window->action_count >= NOTES_ACTIONS)
		return;

	/* Succeeded: the choice waits for the main loop. */
	window->actions[window->action_count] = event->code;
	window->action_count++;
}

/*
 * Takes the menus away.
 */
void
notes_menu_close(
	struct notes_window *window)
{
	/* The window's menu, where shown. */
	if (window->menu_shown)
		(void)kl_window_set_menu(window->kui, NULL, 0U);
	window->menu_shown = 0;
}

/* Sets an action's state (enabled, checked) in the window's menu. */
static void
menu_action_state(
	struct notes_window *window,
	uint32_t action,
	int enabled,
	int checked)
{
	unsigned state;

	/* The bits. */
	state = 0U;
	if (!enabled)
		state |= KL_ACTION_DISABLED;
	if (checked)
		state |= KL_ACTION_CHECKED;

	/* Kept by the window for its menu. */
	(void)kl_window_set_action_state(window->kui, action, state);
}

/* Tells whether the tool chosen is the one an item stands for (1) or not (0). */
static int
menu_is(
	uint32_t tool,
	uint32_t action)
{
	/* The item's tool. */
	if (tool == action)
		return 1;

	/* Another tool. */
	return 0;
}
