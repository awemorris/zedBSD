/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Titlebar Presentation, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_TITLEBAR_H
#define KEILAND_TITLEBAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_seat;
struct xdg_toplevel;

/*
 * The Titlebar Presentation.
 *
 * The compositor draws a window's titlebar: its mark and title, a presentation,
 * and the window's buttons, in the floating titlebar or, while the window
 * is maximized, in the system bar.  The presentation is one of three
 * models the application gives: the menu (the System Menu of <keiland/keiland_menu.h>,
 * the default), controls (back, forward, a breadcrumb, a search field, a view
 * selector...), or tabs.  The application gives only what they mean;
 * the compositor decides how they look and where they go, and tells the
 * application what the user does with them.  Changes are made in
 * transactions, like a menu's.  Every call that returns an int returns 0
 * or an errno value.
 */
struct kl_titlebar;

/*
 * The presentation modes.  A sheet (KL_VERSION 20) has no titlebar of its
 * own: the window hangs under its parent's titlebar
 * (xdg_toplevel_set_parent), in front of the parent, which takes no input but
 * its titlebar's while the sheet is open.  A compositor older than the sheet
 * refuses it (ENOTSUP) and the window stays a window of its own.
 */
#define KL_TITLEBAR_MENU		0U
#define KL_TITLEBAR_CONTROLS	1U
#define KL_TITLEBAR_TABS		2U
#define KL_TITLEBAR_SHEET		3U

/* The controls' roles, which decide how the compositor draws them. */
#define KL_CONTROL_BACK		1U
#define KL_CONTROL_FORWARD	2U
#define KL_CONTROL_HOME		3U
#define KL_CONTROL_UP		4U
#define KL_CONTROL_BREADCRUMB	5U
#define KL_CONTROL_SEARCH		6U
#define KL_CONTROL_VIEW_GRID	7U
#define KL_CONTROL_VIEW_LIST	8U
#define KL_CONTROL_VIEW_COLUMNS	9U
#define KL_CONTROL_SORT		10U
#define KL_CONTROL_FILTER		11U
#define KL_CONTROL_SIDEBAR	12U
#define KL_CONTROL_PREVIEW	13U
#define KL_CONTROL_PROGRESS	14U
#define KL_CONTROL_PRIMARY_ACTION	15U
#define KL_CONTROL_GENERIC	16U

/*
 * A page's reload (KL_VERSION 106): a square with the reload icon, beside
 * the navigation.  The compositor's titlebar protocol has the role from
 * its version 6 (role 17); to an older compositor libkeiland sends a
 * generic control instead, shown with its label.  Where libkeiland draws
 * the titlebar itself (Windows) the icon is its own.
 */
#define KL_CONTROL_RELOAD		17U

/* The controls' priorities: the order they give way in when the room runs short. */
#define KL_PRIORITY_PRIMARY	0U
#define KL_PRIORITY_NORMAL	1U
#define KL_PRIORITY_SECONDARY	2U

/* A progress control's value that says the share done is not known. */
#define KL_PROGRESS_UNKNOWN	1001U

/*
 * A breadcrumb's value: always shown as a box with its text (or its parts
 * when it has none), which a press edits, as a browser's address field
 * (KL_VERSION 94); 0, the default, shows the parts.
 */
#define KL_BREADCRUMB_BOXED	1U

/* The tabs' flags, and the tab strip's options. */
#define KL_TAB_ACTIVE		1U
#define KL_TAB_ATTENTION		2U
#define KL_TAB_CLOSABLE		4U
#define KL_TABS_NEW_BUTTON	1U

/*
 * How a text control takes the keyboard (a search as a field with its text
 * selected; with _EDIT, to go on editing it, the caret at its end and nothing
 * selected; a breadcrumb with _EDIT as a path to edit), and how its editing
 * ended.
 */
#define KL_FOCUS_FIELD		0U
#define KL_FOCUS_EDIT		1U
#define KL_TEXT_SUBMITTED		0U
#define KL_TEXT_CANCELLED		1U
#define KL_TEXT_LEFT		2U

/*
 * What the compositor tells the application about its titlebar: a control chosen
 * (detail is a breadcrumb's part, 0 otherwise), a text control's text as
 * it is typed and when its editing ends, a tab chosen or closed, the
 * new-tab button, and the overflow popup opening.  Any may be NULL.
 *
 * The compositor gives tabs the keyboard too, when the window's menu has no
 * shortcut for the key: Ctrl+Tab and Ctrl+PageDown activate the next tab,
 * Ctrl+Shift+Tab and Ctrl+PageUp the one before (tab_activated).  Closing
 * a tab and a new tab are the application's keys (its menu's shortcuts),
 * since a terminal's shell needs Ctrl+W and Ctrl+T.
 *
 * drop_target (KL_VERSION 7): while a drag and drop (wl_data_device)
 * is over a part of a breadcrumb in the titlebar, the compositor makes the
 * window's surface the drag's target (its data device hears enter, motion
 * and drop at the pointer's place, above the surface) and tells the part
 * here first (id and detail as for control_activated); id 0 says the drag
 * is over none of the controls now.  A drop then goes to that part's folder.
 */
struct kl_titlebar_listener {
	void (*control_activated)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t detail, struct wl_seat *seat, uint32_t serial);
	void (*text_changed)(void *data, struct kl_titlebar *titlebar, uint32_t id, const char *text);
	void (*text_done)(void *data, struct kl_titlebar *titlebar, uint32_t id, const char *text, unsigned how);
	void (*tab_activated)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t serial);
	void (*tab_close_requested)(void *data, struct kl_titlebar *titlebar, uint32_t id);
	void (*new_tab_requested)(void *data, struct kl_titlebar *titlebar, uint32_t serial);
	void (*overflow_menu_opened)(void *data, struct kl_titlebar *titlebar);
	void (*drop_target)(void *data, struct kl_titlebar *titlebar, uint32_t id, uint32_t detail);
};

/*
 * Gives a window its titlebar presentation, in menu mode until changed;
 * NULL with errno set (ENOTSUP for a compositor without it).
 */
struct kl_titlebar *kl_titlebar_create(struct wl_display *display, struct xdg_toplevel *toplevel,
						   const struct kl_titlebar_listener *listener, void *data);

/*
 * Takes the titlebar presentation away; the window shows its menu again.
 */
void kl_titlebar_destroy(struct kl_titlebar *titlebar);

/*
 * Starts a transaction; the changes until kl_titlebar_commit are shown together.
 */
int kl_titlebar_begin(struct kl_titlebar *titlebar);

/*
 * Ends a transaction; the compositor shows its changes at once.
 */
int kl_titlebar_commit(struct kl_titlebar *titlebar);

/*
 * Chooses the presentation (KL_TITLEBAR_*).
 */
int kl_titlebar_set_mode(struct kl_titlebar *titlebar, unsigned mode);

/*
 * Adds a control at the end: its ID (not 0), role, priority, the segmented group it joins (0 for none) and label.
 */
int kl_titlebar_add_control(struct kl_titlebar *titlebar, uint32_t id, unsigned role, unsigned priority, unsigned group, const char *label);

/*
 * Removes a control.
 */
int kl_titlebar_remove_control(struct kl_titlebar *titlebar, uint32_t id);

/*
 * Sets a control's label.
 */
int kl_titlebar_set_control_label(struct kl_titlebar *titlebar, uint32_t id, const char *label);

/*
 * Sets whether a control works now and whether it is checked.
 */
int kl_titlebar_set_control_state(struct kl_titlebar *titlebar, uint32_t id, int enabled, int checked);

/*
 * Sets a progress control's share done, in thousandths (KL_PROGRESS_UNKNOWN
 * when not known), or a breadcrumb's look: KL_BREADCRUMB_BOXED (KL_VERSION
 * 94) shows it always as a box, as a browser's address field, and 0 as its
 * parts.  ENOTSUP for a breadcrumb when the compositor is older
 * (kl_titlebar_v1 before version 5).
 */
int kl_titlebar_set_control_value(struct kl_titlebar *titlebar, uint32_t id, unsigned value);

/*
 * Sets a search's or a breadcrumb's text and what it shows when empty.
 */
int kl_titlebar_set_control_text(struct kl_titlebar *titlebar, uint32_t id, const char *text, const char *placeholder);

/*
 * Sets a breadcrumb's parts, from the first (the outermost) to the last.
 */
int kl_titlebar_set_breadcrumb(struct kl_titlebar *titlebar, uint32_t id, const char *const *segments, size_t count);

/*
 * Adds a tab at the end, closable and not active.
 */
int kl_titlebar_add_tab(struct kl_titlebar *titlebar, uint32_t id, const char *title);

/*
 * Removes a tab.
 */
int kl_titlebar_remove_tab(struct kl_titlebar *titlebar, uint32_t id);

/*
 * Sets a tab's title and flags (KL_TAB_*).
 */
int kl_titlebar_set_tab(struct kl_titlebar *titlebar, uint32_t id, const char *title, unsigned flags);

/*
 * Sets the tab strip's options (KL_TABS_NEW_BUTTON).
 */
int kl_titlebar_set_tabs_options(struct kl_titlebar *titlebar, unsigned options);

/*
 * Gives the keyboard to a committed search or breadcrumb control (KL_FOCUS_*), outside a transaction.
 */
int kl_titlebar_focus_control(struct kl_titlebar *titlebar, uint32_t id, unsigned mode);

/* The most suggestions a field shows (kl_titlebar_set_suggestions). */
#define KL_TITLEBAR_SUGGESTIONS_MAX	12U

/*
 * Gives a committed search or breadcrumb field suggestions, outside a
 * transaction: count pairs, each a label shown in a list under the field and
 * the text it puts in the field when chosen (Up, Down and Enter, or a click
 * or a tap; the field then reports the text as changed, as typing does).  The
 * list shows while the field has the keyboard and goes when its text changes
 * or its editing ends; count 0 takes it away.  Returns 0, ENOTSUP for a
 * compositor without suggestions (one older than KL_VERSION 25), ENOENT,
 * EINVAL or E2BIG.
 */
int kl_titlebar_set_suggestions(struct kl_titlebar *titlebar, uint32_t id, const char *const *labels, const char *const *texts, size_t count);

#ifdef __cplusplus
}
#endif

#endif
