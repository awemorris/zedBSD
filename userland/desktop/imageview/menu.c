/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of Image Viewer in the compositor's System Menu: File (Open, Close,
 * Quit), View (the fit, 100 %, the zoom, the turns, the full screen), Go
 * (the images of the folder) and Help; and the context menu of a right
 * press or a long press on the image, given to libkeiland as tables (WS131
 * p017: kl_window_set_menu, kl_window_popup_menu).  The viewer's state is
 * the actions' state (kl_window_set_action_state), which every item and
 * titlebar control of an action shows.  The compositor draws them and chooses an
 * item for its shortcut; the choice comes back as a KL_WINDOW_ACTION input
 * among the window's.  A compositor without the System Menu leaves the
 * viewer without menus, and the keys work as they do with them.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The submenus. */
#define MENU_FILE		1U
#define MENU_VIEW		2U
#define MENU_GO			3U
#define MENU_HELP		4U

/* The items of File. */
#define MENU_OPEN		10U
#define MENU_FILE_LINE		11U
#define MENU_CLOSE		12U
#define MENU_QUIT		13U
#define MENU_OPEN_WITH		14U
#define MENU_TRASH		15U
#define MENU_TRASH_LINE		16U

/*
 * The items of File > Open With (ws128-p005): one slot for each
 * application, the first's ID and each further one's the next, labelled
 * and shown for the image shown.
 */
#define MENU_OPENER_FIRST	60U

/* The items of View. */
#define MENU_FIT		20U
#define MENU_ACTUAL		21U
#define MENU_ZOOM_IN		22U
#define MENU_ZOOM_OUT		23U
#define MENU_VIEW_LINE		24U
#define MENU_ROTATE_RIGHT	25U
#define MENU_ROTATE_LEFT	26U
#define MENU_TURN_LINE		27U
#define MENU_PLAY		28U
#define MENU_FULLSCREEN		29U
#define MENU_SLIDESHOW		35U

/* The items of Go. */
#define MENU_PREVIOUS		30U
#define MENU_NEXT		31U
#define MENU_GO_LINE		32U
#define MENU_FIRST		33U
#define MENU_LAST		34U

/* The items of Help. */
#define MENU_ABOUT		40U

/* The items of the context menu (a menu of its own). */
#define CONTEXT_FIT		50U
#define CONTEXT_ACTUAL		51U
#define CONTEXT_ROTATE_RIGHT	52U
#define CONTEXT_ROTATE_LEFT	53U
#define CONTEXT_FULLSCREEN	54U
#define CONTEXT_LINE		55U
#define CONTEXT_OPEN		56U
#define CONTEXT_TRASH		57U
#define CONTEXT_TRASH_LINE	58U

/* The menus, in the order they are shown. */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPEN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Open...", IV_ACTION_OPEN, KL_MENU_ROLE_OPEN, KL_MENU_CTRL, 'o' },
	{ MENU_OPEN_WITH, MENU_FILE, KL_MENU_ITEM_SUBMENU, "Open With", IV_ACTION_OPEN_WITH_MENU, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 1U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 1U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 2U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 2U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 3U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 3U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 4U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 4U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 5U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 5U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 6U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 6U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPENER_FIRST + 7U, MENU_OPEN_WITH, KL_MENU_ITEM_NORMAL, "-", IV_ACTION_OPEN_WITH_FIRST + 7U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FILE_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_TRASH, MENU_FILE, KL_MENU_ITEM_NORMAL, "Move to Trash", IV_ACTION_TRASH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_TRASH_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLOSE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Close", IV_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL, 'w' },
	{ MENU_QUIT, MENU_FILE, KL_MENU_ITEM_NORMAL, "Quit Image Viewer", IV_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIT, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Fit to Window", IV_ACTION_FIT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ACTUAL, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Actual Size", IV_ACTION_ACTUAL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ZOOM_IN, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom In", IV_ACTION_ZOOM_IN, KL_MENU_ROLE_ZOOM_IN, 0U, 0U },
	{ MENU_ZOOM_OUT, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom Out", IV_ACTION_ZOOM_OUT, KL_MENU_ROLE_ZOOM_OUT, 0U, 0U },
	{ MENU_VIEW_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ROTATE_RIGHT, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Rotate Right", IV_ACTION_ROTATE_RIGHT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ROTATE_LEFT, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Rotate Left", IV_ACTION_ROTATE_LEFT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_TURN_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PLAY, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Play Animation", IV_ACTION_PLAY, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FULLSCREEN, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Full Screen", IV_ACTION_FULLSCREEN, KL_MENU_ROLE_FULLSCREEN, 0U, 0U },
	{ MENU_SLIDESHOW, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Slideshow", IV_ACTION_SLIDESHOW, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GO, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Go", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PREVIOUS, MENU_GO, KL_MENU_ITEM_NORMAL, "Previous Image", IV_ACTION_PREVIOUS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_NEXT, MENU_GO, KL_MENU_ITEM_NORMAL, "Next Image", IV_ACTION_NEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GO_LINE, MENU_GO, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIRST, MENU_GO, KL_MENU_ITEM_NORMAL, "First Image", IV_ACTION_FIRST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LAST, MENU_GO, KL_MENU_ITEM_NORMAL, "Last Image", IV_ACTION_LAST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_HELP, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Help", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ABOUT, MENU_HELP, KL_MENU_ITEM_NORMAL, "About Image Viewer", IV_ACTION_ABOUT, KL_MENU_ROLE_ABOUT, 0U, 0U }
};

/* The context menu's items, all top-level. */
static const struct kl_menu_entry context_items[] = {
	{ CONTEXT_FIT, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Fit to Window", IV_ACTION_FIT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_ACTUAL, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Actual Size", IV_ACTION_ACTUAL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_ROTATE_RIGHT, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Rotate Right", IV_ACTION_ROTATE_RIGHT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_ROTATE_LEFT, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Rotate Left", IV_ACTION_ROTATE_LEFT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_FULLSCREEN, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Full Screen", IV_ACTION_FULLSCREEN, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_LINE, KL_MENU_ROOT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_OPEN, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Open...", IV_ACTION_OPEN, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_TRASH_LINE, KL_MENU_ROOT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ CONTEXT_TRASH, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Move to Trash", IV_ACTION_TRASH, KL_MENU_ROLE_NONE, 0U, 0U }
};

/* The place of Open With's first slot in the menus' table. */
#define MENU_OPENER_INDEX	3U

static int menu_send(struct iv_menu *menu);
static void menu_action_state(struct iv_menu *menu, uint32_t action, int enabled, int checked);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the viewer then
 * has no menus), or an errno value when the menus could not be made.
 */
int
iv_menu_open(
	struct iv_menu *menu,
	struct iv_window *window,
	const struct iv_state *state)
{
	int error;

	/* Nothing yet but the window; Open With's slots are not set yet (hidden). */
	memset(menu, 0, sizeof(*menu));
	menu->window = window;
	menu->opener_count = -1;

	/* The menus; a compositor without the System Menu leaves the viewer without them (the controls still take the state). */
	error = menu_send(menu);
	if (error == ENOTSUP) {
		iv_log("MENU none errno=%d", error);
		iv_menu_refresh(menu, state);
		return 0;
	}
	if (error != 0)
		return error;
	menu->shown_once = 1;

	/* The state it shows. */
	iv_menu_refresh(menu, state);

	/* Logs the menus for the tests. */
	iv_log("MENU ready items=%u", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));

	/* Succeeded: the menus are the compositor's to show. */
	return 0;
}

/*
 * Tells the menus and the titlebar's controls the viewer's state, as the
 * state of their actions, when it differs from what they show: the
 * image's actions enabled while one can be shown, the folder's by where it
 * is, the fit, the animation, the full screen and the slideshow checked.
 */
void
iv_menu_refresh(
	struct iv_menu *menu,
	const struct iv_state *state)
{
	int can_previous;
	int can_next;
	int can_go;
	int same;

	/* Nothing when the state is the one shown. */
	same = memcmp(state, &menu->shown, sizeof(*state));
	if (same == 0 && menu->sent)
		return;

	/* There is an image before the one shown, one after it, others in the folder. */
	can_previous = 0;
	if (state->has_image && state->index > 0)
		can_previous = 1;
	can_next = 0;
	if (state->has_image && state->index + 1 < state->count)
		can_next = 1;
	can_go = 0;
	if (state->has_image && state->count > 1)
		can_go = 1;

	/* The image's actions need one that can be shown; only an animated one plays. */
	menu_action_state(menu, IV_ACTION_FIT, state->can_show, state->fit);
	menu_action_state(menu, IV_ACTION_ACTUAL, state->can_show, 0);
	menu_action_state(menu, IV_ACTION_ZOOM_IN, state->can_show, 0);
	menu_action_state(menu, IV_ACTION_ZOOM_OUT, state->can_show, 0);
	menu_action_state(menu, IV_ACTION_ROTATE_RIGHT, state->can_show, 0);
	menu_action_state(menu, IV_ACTION_ROTATE_LEFT, state->can_show, 0);
	menu_action_state(menu, IV_ACTION_PLAY, state->animated, state->playing);
	menu_action_state(menu, IV_ACTION_FULLSCREEN, 1, state->fullscreen);

	/* The folder's. */
	menu_action_state(menu, IV_ACTION_PREVIOUS, can_previous, 0);
	menu_action_state(menu, IV_ACTION_NEXT, can_next, 0);
	menu_action_state(menu, IV_ACTION_FIRST, can_go, 0);
	menu_action_state(menu, IV_ACTION_LAST, can_go, 0);

	/* Open With, Move to Trash and the slideshow need an image (ws128-p005). */
	menu_action_state(menu, IV_ACTION_OPEN_WITH_MENU, state->has_image, 0);
	menu_action_state(menu, IV_ACTION_TRASH, state->has_image, 0);
	menu_action_state(menu, IV_ACTION_SLIDESHOW, state->has_image, state->slideshow);

	/* Shown. */
	menu->shown = *state;
	menu->sent = 1;
}

/*
 * Opens the context menu at a point of the window, for the press whose
 * serial the window kept (the right button's, or a long press's finger).
 */
void
iv_menu_context(
	struct iv_menu *menu,
	const struct iv_state *state,
	int x,
	int y)
{
	int error;

	/* Without an image nothing opens. */
	if (!state->has_image)
		return;

	/* The compositor shows it at the press (one still open is replaced); without the System Menu nothing opens. */
	error = kl_window_popup_menu(menu->window->kui, context_items, sizeof(context_items) / sizeof(context_items[0]), x, y);
	if (error != 0) {
		iv_log("CONTEXT-MENU none errno=%d", error);
		return;
	}

	/* The log line the tests read. */
	iv_log("CONTEXT-MENU open x=%d y=%d serial=%u", x, y, kl_window_press_serial(menu->window->kui));
}

/*
 * Shows the applications of File > Open With for the image shown: each
 * slot takes an application's name and is shown, the slots left over are
 * hidden (ws128-p005).  The same names again send nothing.
 */
void
iv_menu_openers(
	struct iv_menu *menu,
	char names[][IV_OPENER_NAME],
	int count)
{
	const char *first;
	int index;
	int same;
	int match;
	int error;

	/* The same number of names as the menu shows, each the same, sends nothing. */
	same = 0;
	if (count == menu->opener_count)
		same = 1;
	for (index = 0; same != 0 && index < count; index++) {
		match = strcmp(names[index], menu->openers[index]);
		if (match != 0)
			same = 0;
	}

	/* Nothing changed. */
	if (same != 0)
		return;

	/* The names kept, each slot's action shown or hidden. */
	for (index = 0; index < IV_OPENERS; index++) {
		if (index < count) {
			snprintf(menu->openers[index], IV_OPENER_NAME, "%s", names[index]);
			(void)kl_window_set_action_state(menu->window->kui, IV_ACTION_OPEN_WITH_FIRST + (uint32_t)index, 0U);
		} else {
			snprintf(menu->openers[index], IV_OPENER_NAME, "-");
			(void)kl_window_set_action_state(menu->window->kui, IV_ACTION_OPEN_WITH_FIRST + (uint32_t)index, KL_ACTION_HIDDEN);
		}
	}
	menu->opener_count = count;

	/* The menu with the slots' names; without the System Menu there is none to show. */
	if (!menu->shown_once)
		return;
	error = menu_send(menu);
	if (error != 0) {
		iv_log("MENU openers-failed errno=%d", error);
		return;
	}

	/* The log line the tests read: how many, and the first (the default). */
	first = "-";
	if (count > 0)
		first = names[0];
	iv_log("MENU openers count=%d first=%s", count, first);
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
iv_menu_close(
	struct iv_menu *menu)
{
	/* No menu on the window, and nothing of the menus is left. */
	if (menu->window != NULL && menu->shown_once)
		(void)kl_window_set_menu(menu->window->kui, NULL, 0U);
	memset(menu, 0, sizeof(*menu));
}

/* Gives libkeiland the window's menu, Open With's slots named as the image's applications are; 0 or an errno value. */
static int
menu_send(
	struct iv_menu *menu)
{
	struct kl_menu_entry entries[sizeof(menu_items) / sizeof(menu_items[0])];
	int index;
	int error;

	/* The table, with the slots' names once they are set. */
	memcpy(entries, menu_items, sizeof(entries));
	for (index = 0; index < IV_OPENERS && menu->opener_count >= 0; index++)
		entries[MENU_OPENER_INDEX + (unsigned)index].label = menu->openers[index];

	/* libkeiland sends what changed. */
	error = kl_window_set_menu(menu->window->kui, entries, sizeof(entries) / sizeof(entries[0]));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sets an action's state: enabled or greyed, checked or not (a refusal is logged). */
static void
menu_action_state(
	struct iv_menu *menu,
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
		iv_log("MENU update-failed errno=%d", error);
}
