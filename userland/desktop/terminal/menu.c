/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's menus: Shell, Edit, View, Session and Help.
 *
 * The compositor draws them (in the window's title bar, or in the system bar
 * while the window is docked) from the table given to libkeiland's window
 * (kl_window_set_menu, WS131 p018).  A choice arrives among the window's
 * inputs as a KL_WINDOW_ACTION input (window.c), queued for the main loop,
 * which then tells the actions' states (Copy and Paste enabled, the font's
 * size checked, Fullscreen and Treat Ambiguous-Width Characters as Wide
 * checked); libkeiland sends only what changed.  Without the System Menu
 * the terminal simply has no menus.
 */

#include "terminal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The top-level items. */
#define MENU_SHELL		1U
#define MENU_EDIT		2U
#define MENU_VIEW		3U
#define MENU_SESSION		4U
#define MENU_HELP		5U

/* Shell. */
#define MENU_NEW_WINDOW		10U
#define MENU_SHELL_LINE		11U
#define MENU_CLOSE		12U
#define MENU_NEW_TAB		13U
#define MENU_CLOSE_TAB		14U

/* Edit. */
#define MENU_COPY		20U
#define MENU_PASTE		21U
#define MENU_EDIT_LINE		22U
#define MENU_SELECT_ALL		23U
#define MENU_FIND_LINE		24U
#define MENU_FIND		25U
#define MENU_FIND_NEXT		26U
#define MENU_FIND_PREVIOUS	27U

/* View, and its Text Size submenu. */
#define MENU_ZOOM_IN		30U
#define MENU_ZOOM_OUT		31U
#define MENU_ZOOM_NORMAL	32U
#define MENU_TEXT_SIZE		33U
#define MENU_VIEW_LINE		34U
#define MENU_FULLSCREEN		35U
#define MENU_AMBIGUOUS_WIDE	36U
#define MENU_SIZE_SMALL		40U
#define MENU_SIZE_MEDIUM	41U
#define MENU_SIZE_LARGE		42U
#define MENU_SIZE_HUGE		43U

/* View > Theme and its themes (ws128-p006). */
#define MENU_THEME		44U
#define MENU_THEME_DARK		45U
#define MENU_THEME_LIGHT	46U
#define MENU_THEME_CONTRAST	47U

/* Session. */
#define MENU_INTERRUPT		50U
#define MENU_END_OF_FILE	51U
#define MENU_SESSION_LINE	52U
#define MENU_CLEAR		53U
#define MENU_RESET		54U

/* Help. */
#define MENU_ABOUT		60U

/* The keysyms of the shortcuts' keys that are not letters. */
#define MENU_KEY_PLUS		0x2bU
#define MENU_KEY_MINUS		0x2dU
#define MENU_KEY_ZERO		0x30U
#define MENU_KEY_F11		0xffc8U

/* The menus, in the order they are shown. */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_SHELL, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Shell", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_NEW_WINDOW, MENU_SHELL, KL_MENU_ITEM_NORMAL, "New Window", TERMINAL_ACTION_NEW_WINDOW, KL_MENU_ROLE_NEW, KL_MENU_CTRL | KL_MENU_SHIFT, 'n' },
	{ MENU_NEW_TAB, MENU_SHELL, KL_MENU_ITEM_NORMAL, "New Tab", TERMINAL_ACTION_NEW_TAB, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 't' },
	{ MENU_SHELL_LINE, MENU_SHELL, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLOSE_TAB, MENU_SHELL, KL_MENU_ITEM_NORMAL, "Close Tab", TERMINAL_ACTION_CLOSE_TAB, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 'w' },
	{ MENU_CLOSE, MENU_SHELL, KL_MENU_ITEM_NORMAL, "Close Window", TERMINAL_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL | KL_MENU_SHIFT, 'q' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_COPY, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Copy", TERMINAL_ACTION_COPY, KL_MENU_ROLE_COPY, KL_MENU_CTRL | KL_MENU_SHIFT, 'c' },
	{ MENU_PASTE, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Paste", TERMINAL_ACTION_PASTE, KL_MENU_ROLE_PASTE, KL_MENU_CTRL | KL_MENU_SHIFT, 'v' },
	{ MENU_EDIT_LINE, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SELECT_ALL, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Select All", TERMINAL_ACTION_SELECT_ALL, KL_MENU_ROLE_SELECT_ALL, KL_MENU_CTRL | KL_MENU_SHIFT, 'a' },
	{ MENU_FIND_LINE, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIND, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find...", TERMINAL_ACTION_FIND, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 'f' },
	{ MENU_FIND_NEXT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Next", TERMINAL_ACTION_FIND_NEXT, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 'g' },
	{ MENU_FIND_PREVIOUS, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Previous", TERMINAL_ACTION_FIND_PREVIOUS, KL_MENU_ROLE_NONE, KL_MENU_CTRL | KL_MENU_SHIFT, 'h' },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ZOOM_IN, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom In", TERMINAL_ACTION_ZOOM_IN, KL_MENU_ROLE_ZOOM_IN, KL_MENU_CTRL, MENU_KEY_PLUS },
	{ MENU_ZOOM_OUT, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom Out", TERMINAL_ACTION_ZOOM_OUT, KL_MENU_ROLE_ZOOM_OUT, KL_MENU_CTRL, MENU_KEY_MINUS },
	{ MENU_ZOOM_NORMAL, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Normal Size", TERMINAL_ACTION_ZOOM_NORMAL, KL_MENU_ROLE_NONE, KL_MENU_CTRL, MENU_KEY_ZERO },
	{ MENU_TEXT_SIZE, MENU_VIEW, KL_MENU_ITEM_SUBMENU, "Text Size", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SIZE_SMALL, MENU_TEXT_SIZE, KL_MENU_ITEM_RADIO, "Small", TERMINAL_ACTION_SIZE_SMALL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SIZE_MEDIUM, MENU_TEXT_SIZE, KL_MENU_ITEM_RADIO, "Medium", TERMINAL_ACTION_SIZE_MEDIUM, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SIZE_LARGE, MENU_TEXT_SIZE, KL_MENU_ITEM_RADIO, "Large", TERMINAL_ACTION_SIZE_LARGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SIZE_HUGE, MENU_TEXT_SIZE, KL_MENU_ITEM_RADIO, "Huge", TERMINAL_ACTION_SIZE_HUGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THEME, MENU_VIEW, KL_MENU_ITEM_SUBMENU, "Theme", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THEME_DARK, MENU_THEME, KL_MENU_ITEM_RADIO, "Dark", TERMINAL_ACTION_THEME_DARK, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THEME_LIGHT, MENU_THEME, KL_MENU_ITEM_RADIO, "Light", TERMINAL_ACTION_THEME_LIGHT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THEME_CONTRAST, MENU_THEME, KL_MENU_ITEM_RADIO, "High Contrast", TERMINAL_ACTION_THEME_CONTRAST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FULLSCREEN, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Fullscreen", TERMINAL_ACTION_FULLSCREEN, KL_MENU_ROLE_FULLSCREEN, 0U, MENU_KEY_F11 },
	{ MENU_AMBIGUOUS_WIDE, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Treat Ambiguous-Width Characters as Wide", TERMINAL_ACTION_AMBIGUOUS_WIDE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SESSION, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Session", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_INTERRUPT, MENU_SESSION, KL_MENU_ITEM_NORMAL, "Send Interrupt", TERMINAL_ACTION_INTERRUPT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_END_OF_FILE, MENU_SESSION, KL_MENU_ITEM_NORMAL, "Send End of File", TERMINAL_ACTION_END_OF_FILE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SESSION_LINE, MENU_SESSION, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLEAR, MENU_SESSION, KL_MENU_ITEM_NORMAL, "Clear Screen", TERMINAL_ACTION_CLEAR, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_RESET, MENU_SESSION, KL_MENU_ITEM_NORMAL, "Reset Terminal", TERMINAL_ACTION_RESET, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_HELP, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Help", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ABOUT, MENU_HELP, KL_MENU_ITEM_NORMAL, "About Terminal", TERMINAL_ACTION_ABOUT, KL_MENU_ROLE_ABOUT, 0U, 0U }
};

static int menu_state(struct terminal_window *window, const struct terminal_menu_state *state);
static void menu_action_state(struct terminal_window *window, uint32_t action, int enabled, int checked);
static int menu_same(unsigned value, unsigned named);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the terminal then
 * has no menus), or -1 with errno set when the menus could not be made.
 */
int
terminal_menu_open(
	struct terminal_window *window,
	const struct terminal_menu_state *state)
{
	int error;

	/* The table; a compositor without the System Menu leaves the terminal without menus. */
	error = kl_window_set_menu(window->kui, menu_items, sizeof(menu_items) / sizeof(menu_items[0]));
	if (error == ENOTSUP) {
		printf("ZTERM MENU none errno=%d\n", error);
		return 0;
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	window->menu_shown = 1;

	/* The state it shows. */
	error = menu_state(window, state);
	if (error != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: the menus are the compositor's to show. */
	printf("ZTERM MENU ready items=%u\n", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));
	return 0;
}

/*
 * Tells the menus the terminal's state when it differs from what they show.
 */
void
terminal_menu_refresh(
	struct terminal_window *window,
	const struct terminal_menu_state *state)
{
	int same;
	int error;

	/* Without menus nothing is sent. */
	if (!window->menu_shown)
		return;

	/* Nor when the state is the one the menus show. */
	same = memcmp(state, &window->menu_state, sizeof(*state));
	if (same == 0)
		return;

	/* The new state; a refusal is reported and the menus stay as they were. */
	error = menu_state(window, state);
	if (error != 0)
		printf("ZTERM MENU update-failed errno=%d\n", error);
}

/*
 * Takes the oldest action chosen and not yet carried out
 * (TERMINAL_ACTION_NONE when there is none).
 */
uint32_t
terminal_menu_take(
	struct terminal_window *window)
{
	uint32_t action;

	/* Nothing waits. */
	if (window->action_count == 0U)
		return TERMINAL_ACTION_NONE;

	/* The oldest leaves the queue. */
	action = window->actions[0];
	window->action_count--;
	memmove(window->actions, window->actions + 1, window->action_count * sizeof(window->actions[0]));

	/* Succeeded: the action to carry out. */
	return action;
}

/*
 * Queues an action chosen in the menus (a KL_WINDOW_ACTION input of the
 * window: its action and the item's ID) for the main loop.
 */
void
terminal_menu_chosen(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	/* The log line the tests read. */
	printf("ZTERM MENU item=%d action=%u serial=%u\n", (int)event->id, event->code, event->serial);
	fflush(stdout);

	/* A full queue drops the choice (the user has chosen sixteen things in one round). */
	if (window->action_count == TERMINAL_ACTIONS)
		return;

	/* The count is how many choices wait for the main loop (terminal_menu_take). */
	window->actions[window->action_count] = event->code;
	window->action_count++;
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
terminal_menu_close(
	struct terminal_window *window)
{
	/* The window's menu (none kept without one). */
	if (window->menu_shown)
		(void)kl_window_set_menu(window->kui, NULL, 0U);
	window->menu_shown = 0;
}

/*
 * Shows a state in the menus through the actions' states: Copy and Paste
 * enabled, Zoom In and Out within the sizes, the size's radio item,
 * Fullscreen and Treat Ambiguous-Width Characters as Wide checked, the
 * theme's radio item and Find Next and Find Previous enabled (ws128-p006).
 */
static int
menu_state(
	struct terminal_window *window,
	const struct terminal_menu_state *state)
{
	int contrast;
	int larger;
	int smaller;
	int light;
	int dark;

	/* Zooming stops at the largest and the smallest size. */
	larger = 0;
	if (state->pixels < TERMINAL_PIXELS_MAX)
		larger = 1;
	smaller = 0;
	if (state->pixels > TERMINAL_PIXELS_MIN)
		smaller = 1;

	/* Copy needs a selection, Paste the clipboard's text; zooming while there is room. */
	menu_action_state(window, TERMINAL_ACTION_COPY, state->selection, 0);
	menu_action_state(window, TERMINAL_ACTION_PASTE, state->clipboard, 0);
	menu_action_state(window, TERMINAL_ACTION_ZOOM_IN, larger, 0);
	menu_action_state(window, TERMINAL_ACTION_ZOOM_OUT, smaller, 0);

	/* The size's radio item, or none for a size between them. */
	menu_action_state(window, TERMINAL_ACTION_SIZE_SMALL, 1, menu_same(state->pixels, TERMINAL_PIXELS_SMALL));
	menu_action_state(window, TERMINAL_ACTION_SIZE_MEDIUM, 1, menu_same(state->pixels, TERMINAL_PIXELS_MEDIUM));
	menu_action_state(window, TERMINAL_ACTION_SIZE_LARGE, 1, menu_same(state->pixels, TERMINAL_PIXELS_LARGE));
	menu_action_state(window, TERMINAL_ACTION_SIZE_HUGE, 1, menu_same(state->pixels, TERMINAL_PIXELS_HUGE));

	/* Fullscreen while the window is, and Treat Ambiguous-Width Characters as Wide while it is on (ws128-p009). */
	menu_action_state(window, TERMINAL_ACTION_FULLSCREEN, 1, state->fullscreen);
	menu_action_state(window, TERMINAL_ACTION_AMBIGUOUS_WIDE, 1, state->ambiguous_wide);

	/* The theme's radio item: the light one, the contrast one, or the dark one for any other (ws128-p006). */
	dark = 0;
	light = 0;
	contrast = 0;
	if (state->theme == TERMINAL_THEME_LIGHT) {
		light = 1;
	} else if (state->theme == TERMINAL_THEME_CONTRAST) {
		contrast = 1;
	} else {
		dark = 1;
	}
	menu_action_state(window, TERMINAL_ACTION_THEME_DARK, 1, dark);
	menu_action_state(window, TERMINAL_ACTION_THEME_LIGHT, 1, light);
	menu_action_state(window, TERMINAL_ACTION_THEME_CONTRAST, 1, contrast);

	/* Finding again needs a text looked for. */
	menu_action_state(window, TERMINAL_ACTION_FIND_NEXT, state->can_find_again, 0);
	menu_action_state(window, TERMINAL_ACTION_FIND_PREVIOUS, state->can_find_again, 0);

	/* Succeeded: the menus show the state. */
	window->menu_state = *state;
	printf("ZTERM MENU state selection=%d clipboard=%d pixels=%u fullscreen=%d ambiguous_wide=%d theme=%u find=%d\n", state->selection, state->clipboard, state->pixels, state->fullscreen, state->ambiguous_wide, state->theme, state->can_find_again);
	fflush(stdout);
	return 0;
}

/* Sets an action's state (enabled, checked) in the window's menu. */
static void
menu_action_state(
	struct terminal_window *window,
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
	error = kl_window_set_action_state(window->kui, action, state);
	if (error != 0 && error != ENOTSUP)
		printf("ZTERM MENU update-failed errno=%d\n", error);
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
