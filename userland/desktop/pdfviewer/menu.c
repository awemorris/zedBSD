/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The menus of PDF Viewer in the compositor's System Menu: File (Open, Annotate
 * in Notes, Print (ws145-p007), Close, Quit), Edit (ws128-p004: Copy, Find, Find Next and
 * Previous), View (the sidebar of page thumbnails, the two
 * modes, the two fits, the zoom) and Go (the pages), given to libkeiland
 * as a table (WS131 p017: kl_window_set_menu).  The viewer's state is the
 * actions' state (kl_window_set_action_state), which every item and
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
#define MENU_EDIT		4U

/* The items of File. */
#define MENU_OPEN		10U
#define MENU_ANNOTATE		11U
#define MENU_PRINT		15U
#define MENU_FILE_LINE		12U
#define MENU_CLOSE		13U
#define MENU_QUIT		14U

/* The items of Edit (ws128-p004). */
#define MENU_COPY		40U
#define MENU_EDIT_LINE		41U
#define MENU_FIND		42U
#define MENU_FIND_NEXT		43U
#define MENU_FIND_PREVIOUS	44U

/* The items of View. */
#define MENU_THUMBNAILS		19U
#define MENU_THUMBNAILS_LINE	29U
#define MENU_SCROLL		20U
#define MENU_PAGES		21U
#define MENU_VIEW_LINE		22U
#define MENU_FIT_WIDTH		23U
#define MENU_FIT_PAGE		24U
#define MENU_ZOOM_LINE		25U
#define MENU_ZOOM_IN		26U
#define MENU_ZOOM_OUT		27U
#define MENU_ZOOM_RESET		28U

/* The items of Go. */
#define MENU_PREVIOUS		30U
#define MENU_NEXT		31U
#define MENU_GO_LINE		32U
#define MENU_FIRST		33U
#define MENU_LAST		34U

/* The keysyms of the shortcuts' keys that are not letters. */
#define MENU_KEY_PLUS		0x2bU
#define MENU_KEY_MINUS		0x2dU
#define MENU_KEY_ZERO		0x30U
#define MENU_KEY_F3		0xffc0U

/* The menus, in the order they are shown. */
static const struct kl_menu_entry menu_items[] = {
	{ MENU_FILE, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_OPEN, MENU_FILE, KL_MENU_ITEM_NORMAL, "Open...", PV_ACTION_OPEN, KL_MENU_ROLE_OPEN, KL_MENU_CTRL, 'o' },
	{ MENU_ANNOTATE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Annotate in Notes", PV_ACTION_ANNOTATE, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'e' },
	{ MENU_PRINT, MENU_FILE, KL_MENU_ITEM_NORMAL, "Print", PV_ACTION_PRINT, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'p' },
	{ MENU_FILE_LINE, MENU_FILE, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_CLOSE, MENU_FILE, KL_MENU_ITEM_NORMAL, "Close", PV_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL, 'w' },
	{ MENU_QUIT, MENU_FILE, KL_MENU_ITEM_NORMAL, "Quit PDF Viewer", PV_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ MENU_EDIT, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_COPY, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Copy", PV_ACTION_COPY, KL_MENU_ROLE_COPY, KL_MENU_CTRL, 'c' },
	{ MENU_EDIT_LINE, MENU_EDIT, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIND, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find...", PV_ACTION_FIND, KL_MENU_ROLE_FIND, KL_MENU_CTRL, 'f' },
	{ MENU_FIND_NEXT, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Next", PV_ACTION_FIND_NEXT, KL_MENU_ROLE_NONE, 0U, MENU_KEY_F3 },
	{ MENU_FIND_PREVIOUS, MENU_EDIT, KL_MENU_ITEM_NORMAL, "Find Previous", PV_ACTION_FIND_PREVIOUS, KL_MENU_ROLE_NONE, KL_MENU_SHIFT, MENU_KEY_F3 },
	{ MENU_VIEW, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THUMBNAILS, MENU_VIEW, KL_MENU_ITEM_CHECKBOX, "Page Thumbnails", PV_ACTION_THUMBNAILS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_THUMBNAILS_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_SCROLL, MENU_VIEW, KL_MENU_ITEM_RADIO, "Continuous Scroll", PV_ACTION_MODE_SCROLL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PAGES, MENU_VIEW, KL_MENU_ITEM_RADIO, "Single Page", PV_ACTION_MODE_PAGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_VIEW_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIT_WIDTH, MENU_VIEW, KL_MENU_ITEM_RADIO, "Fit Width", PV_ACTION_FIT_WIDTH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIT_PAGE, MENU_VIEW, KL_MENU_ITEM_RADIO, "Fit Page", PV_ACTION_FIT_PAGE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ZOOM_LINE, MENU_VIEW, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_ZOOM_IN, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom In", PV_ACTION_ZOOM_IN, KL_MENU_ROLE_ZOOM_IN, KL_MENU_CTRL, MENU_KEY_PLUS },
	{ MENU_ZOOM_OUT, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Zoom Out", PV_ACTION_ZOOM_OUT, KL_MENU_ROLE_ZOOM_OUT, KL_MENU_CTRL, MENU_KEY_MINUS },
	{ MENU_ZOOM_RESET, MENU_VIEW, KL_MENU_ITEM_NORMAL, "Reset Zoom", PV_ACTION_ZOOM_RESET, KL_MENU_ROLE_NONE, KL_MENU_CTRL, MENU_KEY_ZERO },
	{ MENU_GO, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Go", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_PREVIOUS, MENU_GO, KL_MENU_ITEM_NORMAL, "Previous Page", PV_ACTION_PREVIOUS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_NEXT, MENU_GO, KL_MENU_ITEM_NORMAL, "Next Page", PV_ACTION_NEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_GO_LINE, MENU_GO, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_FIRST, MENU_GO, KL_MENU_ITEM_NORMAL, "First Page", PV_ACTION_FIRST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ MENU_LAST, MENU_GO, KL_MENU_ITEM_NORMAL, "Last Page", PV_ACTION_LAST, KL_MENU_ROLE_NONE, 0U, 0U }
};


static void menu_action_state(struct pv_menu *menu, uint32_t action, int enabled, int checked);

/*
 * Gives the compositor the window's menus, showing a state.
 *
 * Returns 0, also when the compositor has no System Menu (the viewer then
 * has no menus), or an errno value when the menus could not be made.
 */
int
pv_menu_open(
	struct pv_menu *menu,
	struct pv_window *window,
	const struct pv_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(menu, 0, sizeof(*menu));
	menu->window = window;

	/* The menus; a compositor without the System Menu leaves the viewer without them (the controls still take the state). */
	error = kl_window_set_menu(window->kui, menu_items, sizeof(menu_items) / sizeof(menu_items[0]));
	if (error == ENOTSUP) {
		pv_log("MENU none errno=%d", error);
		pv_menu_refresh(menu, state);
		return 0;
	}
	if (error != 0)
		return error;
	menu->shown_once = 1;

	/* The state it shows. */
	pv_menu_refresh(menu, state);

	/* Succeeded: the menus are the compositor's to show. */
	pv_log("MENU ready items=%u", (unsigned)(sizeof(menu_items) / sizeof(menu_items[0])));
	return 0;
}

/*
 * Tells the menus and the titlebar's controls the viewer's state, as the
 * state of their actions, when it differs from what they show: the
 * actions that need a document or another page enabled, the mode's, the
 * fit's and the thumbnails' checked.
 */
void
pv_menu_refresh(
	struct pv_menu *menu,
	const struct pv_state *state)
{
	int can_previous;
	int can_next;
	int scrolling;
	int paging;
	int fitting_width;
	int fitting_page;
	int same;

	/* Nothing when the state is the one shown. */
	same = memcmp(state, &menu->shown, sizeof(*state));
	if (same == 0 && menu->sent)
		return;

	/* What the state allows and what it has chosen. */
	can_previous = 0;
	if (state->has_document && state->page > 0)
		can_previous = 1;
	can_next = 0;
	if (state->has_document && state->page + 1 < state->count)
		can_next = 1;
	scrolling = 0;
	if (state->mode == PV_MODE_SCROLL)
		scrolling = 1;
	paging = 0;
	if (state->mode == PV_MODE_PAGE)
		paging = 1;
	fitting_width = 0;
	if (state->fit == PV_FIT_WIDTH)
		fitting_width = 1;
	fitting_page = 0;
	if (state->fit == PV_FIT_PAGE)
		fitting_page = 1;

	/* The actions that need a document or another page. */
	menu_action_state(menu, PV_ACTION_ANNOTATE, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_PRINT, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_COPY, state->has_selection, 0);
	menu_action_state(menu, PV_ACTION_FIND, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_FIND_NEXT, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_FIND_PREVIOUS, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_PREVIOUS, can_previous, 0);
	menu_action_state(menu, PV_ACTION_NEXT, can_next, 0);
	menu_action_state(menu, PV_ACTION_FIRST, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_LAST, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_ZOOM_IN, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_ZOOM_OUT, state->has_document, 0);
	menu_action_state(menu, PV_ACTION_PAGE_INFO, state->has_document, 0);

	/* The ones in force checked. */
	menu_action_state(menu, PV_ACTION_THUMBNAILS, state->has_document, state->thumbnails);
	menu_action_state(menu, PV_ACTION_MODE_SCROLL, 1, scrolling);
	menu_action_state(menu, PV_ACTION_MODE_PAGE, 1, paging);
	menu_action_state(menu, PV_ACTION_FIT_WIDTH, state->has_document, fitting_width);
	menu_action_state(menu, PV_ACTION_FIT_PAGE, state->has_document, fitting_page);

	/* Shown. */
	menu->shown = *state;
	menu->sent = 1;
}

/*
 * Takes the menus away from the compositor (before the window goes).
 */
void
pv_menu_close(
	struct pv_menu *menu)
{
	/* No menu on the window, and nothing kept. */
	if (menu->window != NULL && menu->shown_once)
		(void)kl_window_set_menu(menu->window->kui, NULL, 0U);
	memset(menu, 0, sizeof(*menu));
}

/* Sets an action's state: enabled or greyed, checked or not (a refusal is logged). */
static void
menu_action_state(
	struct pv_menu *menu,
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
		pv_log("MENU update-failed errno=%d", error);
}
