/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of Text Editor that speak to the compositor through libkeiland's
 * window (the window, its presenter, the clipboard and the primary
 * selection, WS090) and libkeiland's extensions: the editor's queue of
 * inputs (queue.c), the menus (shown in the titlebar's menu bar) and the
 * context menu (menu.c), and the glass (glass.c).  The host tests build
 * the rest of the program without them.
 */

#ifndef TEXTEDIT_WINDOW_H
#define TEXTEDIT_WINDOW_H

#include "textedit.h"

#include <keiland/keiland.h>

/* How many inputs wait for the editor at most. */
#define TE_WINDOW_EVENTS	256U

/*
 * The editor's window: libkeiland's window, and the queue of the editor's
 * inputs -- the window's pointer, keys and focus turned into te_event
 * values by the main loop, and the actions of the menus and the file
 * chooser -- in the order they came.  It lives for the whole run.
 */
struct te_window {
	struct kl_window *kui;

	/* The pointer's place and the modifiers held (TE_MOD_*), which every queued input carries. */
	int pointer_x;
	int pointer_y;
	uint32_t modifiers;

	/* The inputs waiting, a ring: the oldest's slot and how many. */
	struct te_event events[TE_WINDOW_EVENTS];
	unsigned event_first;
	unsigned event_count;
};

/*
 * What the menus show of the editor: whether a change can
 * be undone and redone, whether text is selected, whether the document
 * has unsaved changes, and the two View switches.
 */
struct te_state {
	int can_undo;
	int can_redo;
	int selected;
	int modified;
	int line_numbers;
	int wrap;
};

/* The action of File > Open Recent's line that says there is none (always greyed). */
#define TE_ACTION_RECENT_NONE	99U

/*
 * The window's menus as given to libkeiland (menu.c, WS131 p016): the
 * window, whether the menu was given (not without the compositor's System
 * Menu), the state the actions last showed and whether it was sent, and
 * File > Open Recent's items (ws128-p003).
 */
struct te_menu {
	struct te_window *window;
	int shown_once;
	struct te_state shown;
	int sent;
	struct kl_menu_entry recent[TE_RECENT_MAX];
	size_t recent_count;
};

/*
 * The window's glass (glass.c): the window, whether it is glass (the
 * window then keeps no ground of its own), and the card it was last told
 * of (libkeiland sends only a change).
 */
struct te_glass {
	struct te_window *window;
	int on;
	struct te_rect shown;
};

/* The editor's queue of inputs (queue.c). */
int te_window_take(struct te_window *window, struct te_event *event);
struct te_event *te_window_push(struct te_window *window, enum te_event_type type);
void te_window_act(struct te_window *window, uint32_t action);

/* The menus and the context menu (menu.c). */
int te_menu_open(struct te_menu *menu, struct te_window *window, const struct te_state *state);
void te_menu_refresh(struct te_menu *menu, const struct te_state *state);
void te_menu_popup(struct te_menu *menu, int x, int y);
void te_menu_recent(struct te_menu *menu, const struct te_app *app);
void te_menu_close(struct te_menu *menu);

/* The glass (glass.c). */
int te_glass_open(struct te_glass *glass, struct te_window *window, const struct te_app *app, int see_through);
void te_glass_refresh(struct te_glass *glass, const struct te_app *app);
void te_glass_close(struct te_glass *glass);


#endif
