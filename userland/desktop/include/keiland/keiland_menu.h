/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Menu and the context menus, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_MENU_H
#define KEILAND_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The System Menu.
 *
 * A service is one connection's way to the compositor's menus; a menu is one tree
 * of items; a window menu shows a menu on one xdg_toplevel.  One menu may be
 * shown on several windows (an application menu shared by its windows); the
 * choice comes back through the window menu it was made on.
 *
 * Items are named by numbers the application chooses (not 0; unique in
 * their menu).  KL_MENU_ROOT is the parent of the top-level items (in
 * a terminal: Shell, Edit, View, Session, Help); a submenu item is the
 * parent of the items under it.  Every change is made between
 * kl_menu_begin and kl_menu_commit, and the compositor shows the
 * changes of one commit together.
 *
 * The compositor owns the looks and the input.  A checkbox or radio item is not
 * checked by the compositor when it is chosen; the application sets its state in
 * the next transaction.  A shortcut is shown in the menu and the compositor
 * chooses the item when its keys are pressed in the focused window.
 *
 * Every call returns 0 or an errno value, and a refused call sends nothing:
 * EINVAL (a malformed argument, a change outside a transaction, a checked
 * state on an item that cannot be checked), EEXIST (an ID in use), ENOENT
 * (an ID that names no item), EBUSY (a transaction already open), E2BIG (a
 * menu or a label too large), ENOMEM.  The requests go out when the
 * application flushes its Wayland connection; the choices arrive when it
 * dispatches the queue its xdg_toplevel is on.
 */
struct wl_display;
struct wl_seat;
struct xdg_toplevel;
struct kl_menu_service;
struct kl_menu;
struct kl_window_menu;

/* The parent of the top-level items. */
#define KL_MENU_ROOT		0U

/* The kinds of item. */
#define KL_MENU_ITEM_NORMAL	0U
#define KL_MENU_ITEM_SEPARATOR	1U
#define KL_MENU_ITEM_CHECKBOX	2U
#define KL_MENU_ITEM_RADIO	3U
#define KL_MENU_ITEM_SUBMENU	4U

/* What an item means to the system (the compositor may give it an icon or a place of its own). */
#define KL_MENU_ROLE_NONE		0U
#define KL_MENU_ROLE_ABOUT	1U
#define KL_MENU_ROLE_PREFERENCES	2U
#define KL_MENU_ROLE_QUIT		3U
#define KL_MENU_ROLE_UNDO		4U
#define KL_MENU_ROLE_REDO		5U
#define KL_MENU_ROLE_CUT		6U
#define KL_MENU_ROLE_COPY		7U
#define KL_MENU_ROLE_PASTE	8U
#define KL_MENU_ROLE_DELETE	9U
#define KL_MENU_ROLE_SELECT_ALL	10U
#define KL_MENU_ROLE_NEW		11U
#define KL_MENU_ROLE_OPEN		12U
#define KL_MENU_ROLE_SAVE		13U
#define KL_MENU_ROLE_CLOSE	14U
#define KL_MENU_ROLE_FIND		15U
#define KL_MENU_ROLE_HELP		16U
#define KL_MENU_ROLE_FULLSCREEN	17U
#define KL_MENU_ROLE_ZOOM_IN	18U
#define KL_MENU_ROLE_ZOOM_OUT	19U

/* The modifiers of a shortcut, whose key is an XKB keysym ('c', '+', 0xffc8 for F11). */
#define KL_MENU_SHIFT		1U
#define KL_MENU_CTRL		2U
#define KL_MENU_ALT		4U
#define KL_MENU_SUPER		8U

/*
 * What a window menu tells the application.  Any member may be NULL.
 *
 * activated: the user chose an item (its ID and action), by the seat's
 * input of the serial.  opened, closed: the popup of a submenu (a top-level
 * item included) opened or closed; an application may update the menu in
 * answer, and the compositor redraws the open popup.
 */
struct kl_window_menu_listener {
	void (*activated)(void *data, struct kl_window_menu *window_menu, uint32_t item, uint32_t action, struct wl_seat *seat, uint32_t serial);
	void (*opened)(void *data, struct kl_window_menu *window_menu, uint32_t item);
	void (*closed)(void *data, struct kl_window_menu *window_menu, uint32_t item);
};

/*
 * Opens the connection's menu service.
 *
 * Returns NULL with errno ENOTSUP when the compositor has no System Menu;
 * the application then draws its own menus.
 */
struct kl_menu_service *kl_menu_service_open(struct wl_display *display);

/*
 * Closes a menu service; the menus and window menus made from it stay.
 */
void kl_menu_service_close(struct kl_menu_service *service);

/*
 * Makes an empty menu; NULL with errno set when it cannot.
 */
struct kl_menu *kl_menu_create(struct kl_menu_service *service);

/*
 * Destroys a menu; windows showing it show no menu.
 */
void kl_menu_destroy(struct kl_menu *menu);

/*
 * Starts a transaction: the changes that follow are shown together at the commit.
 */
int kl_menu_begin(struct kl_menu *menu);

/*
 * Ends a transaction, and the compositor shows its changes at once.
 */
int kl_menu_commit(struct kl_menu *menu);

/*
 * Adds an item as the last child of a parent.
 */
int kl_menu_append(struct kl_menu *menu, uint32_t id, uint32_t parent, unsigned type, const char *label, uint32_t action);

/*
 * Adds an item before one of a parent's children (0 appends).
 */
int kl_menu_insert(struct kl_menu *menu, uint32_t id, uint32_t parent, uint32_t before, unsigned type, const char *label, uint32_t action);

/*
 * Removes an item and everything under it.
 */
int kl_menu_remove(struct kl_menu *menu, uint32_t id);

/*
 * Sets an item's label (UTF-8, at most 255 bytes).
 */
int kl_menu_set_label(struct kl_menu *menu, uint32_t id, const char *label);

/*
 * Sets the action an item's choice reports.
 */
int kl_menu_set_action(struct kl_menu *menu, uint32_t id, uint32_t action);

/*
 * Sets whether an item can be chosen (it is shown pale when it cannot).
 */
int kl_menu_set_enabled(struct kl_menu *menu, uint32_t id, int enabled);

/*
 * Sets whether an item is shown at all.
 */
int kl_menu_set_visible(struct kl_menu *menu, uint32_t id, int visible);

/*
 * Sets whether a checkbox or radio item is checked.
 */
int kl_menu_set_checked(struct kl_menu *menu, uint32_t id, int checked);

/*
 * Sets an item's role (KL_MENU_ROLE_*).
 */
int kl_menu_set_role(struct kl_menu *menu, uint32_t id, unsigned role);

/*
 * Sets an item's icon by its icon-theme name ("" for none).
 */
int kl_menu_set_icon_name(struct kl_menu *menu, uint32_t id, const char *icon_name);

/*
 * Sets an item's shortcut: KL_MENU_* modifiers and an XKB keysym (0 removes it).
 */
int kl_menu_set_shortcut(struct kl_menu *menu, uint32_t id, unsigned modifiers, uint32_t keysym);

/*
 * Makes the place on a window that shows a menu; NULL with errno set when it cannot.
 */
struct kl_window_menu *kl_window_menu_create(struct kl_menu_service *service, struct xdg_toplevel *toplevel,
							  const struct kl_window_menu_listener *listener, void *data);

/*
 * Shows a menu on the window (NULL shows none).
 */
int kl_window_menu_set(struct kl_window_menu *window_menu, struct kl_menu *menu);

/*
 * Destroys a window's place for a menu; the window shows none.
 */
void kl_window_menu_destroy(struct kl_window_menu *window_menu);

/*
 * Context menus: a menu's top-level items shown once as a popup at a point of
 * a surface, in answer to a press (its seat and serial; the compositor opens
 * only for the latest press).  The compositor owns the looks and the input as
 * for the menubar.  activated: the user chose an item (its ID and action);
 * done: the context menu closed, after a choice or without one -- told once,
 * last; the application destroys it then.  Either member may be NULL.
 */
struct wl_surface;
struct kl_context_menu;
struct kl_context_menu_listener {
	void (*activated)(void *data, struct kl_context_menu *context_menu, uint32_t item, uint32_t action, uint32_t serial);
	void (*done)(void *data, struct kl_context_menu *context_menu);
};

/*
 * Opens a menu as a context menu at (x, y) of a surface; NULL with errno
 * set: ENOTSUP for a compositor without context menus, ENOMEM, or EINVAL
 * when the listener cannot be installed.
 */
struct kl_context_menu *kl_menu_popup(struct kl_menu_service *service, struct kl_menu *menu, struct wl_surface *surface,
						  int32_t x, int32_t y, struct wl_seat *seat, uint32_t serial,
						  const struct kl_context_menu_listener *listener, void *data);

/*
 * Destroys a context menu; one still open closes without telling.
 */
void kl_context_menu_destroy(struct kl_context_menu *context_menu);

#ifdef __cplusplus
}
#endif

#endif
