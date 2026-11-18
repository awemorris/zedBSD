/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Menu (WS070, plan/ws070/design.md section 4): the wrapper of
 * the compositor's xdg_toplevel_menu_v1 protocol.
 *
 * A menu keeps a mirror of its items' IDs, parents and types as the
 * requests sent so far leave them, so that a call the compositor would
 * refuse -- and answer with a protocol error that ends the whole
 * connection -- is refused here as one failed call instead.  The protocol's
 * objects live on the application's queues: the manager and the menus on
 * the display's default queue, a window menu on its toplevel's queue, so
 * the choices arrive where the application dispatches its window's events.
 */

#include <keiland/keiland.h>

#include "ui/internal.h"

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include "userland/desktop/libwayland/xdg-toplevel-menu-v1-client-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The oldest version of the protocol this library speaks, and the newest (2: context menus). */
#define MENU_VERSION		1U
#define MENU_VERSION_CONTEXT	2U

/* The compositor's bounds of one menu (plan/ws070/design.md section 2.2). */
#define MENU_ITEMS_MAX		1024U
#define MENU_DEPTH_MAX		8U
#define MENU_TEXT_MAX		255U
#define MENU_ROLE_LAST		19U
#define MENU_MODIFIERS_ALL	15U

/*
 * A connection's way to the System Menu: the display and the bound
 * xdg_menu_manager_v1.  It lives until kl_menu_service_close.
 */
struct kl_menu_service {
	struct wl_display *display;
	struct xdg_menu_manager_v1 *manager;
	uint32_t version;
};

/* One item as the mirror knows it: its ID, its parent and its type. */
struct menu_entry {
	uint32_t id;
	uint32_t parent;
	unsigned type;
};

/*
 * One menu: its xdg_menu_v1, the mirror of its items (in no particular
 * order), whether a transaction is open, and the serial of the last one.
 */
struct kl_menu {
	struct xdg_menu_v1 *proxy;
	struct menu_entry *entries;
	unsigned count;
	unsigned capacity;
	unsigned updating;
	uint32_t serial;
};

/* A window's place for a menu: its xdg_toplevel_menu_v1 and the application's listener. */
struct kl_window_menu {
	struct xdg_toplevel_menu_v1 *proxy;
	const struct kl_window_menu_listener *listener;
	void *data;
};

/* A context menu: its xdg_context_menu_v1 and the application's listener. */
struct kl_context_menu {
	struct xdg_context_menu_v1 *proxy;
	const struct kl_context_menu_listener *listener;
	void *data;
};

static void menu_activated(void *data, struct xdg_toplevel_menu_v1 *proxy, uint32_t item, uint32_t action, struct wl_seat *seat, uint32_t serial);
static void menu_opened(void *data, struct xdg_toplevel_menu_v1 *proxy, uint32_t item);
static void menu_closed(void *data, struct xdg_toplevel_menu_v1 *proxy, uint32_t item);
static void menu_context_activated(void *data, struct xdg_context_menu_v1 *proxy, uint32_t item, uint32_t action, uint32_t serial);
static void menu_context_done(void *data, struct xdg_context_menu_v1 *proxy);
static int menu_find(const struct kl_menu *menu, uint32_t id);
static unsigned menu_depth(const struct kl_menu *menu, uint32_t id);
static int menu_check_add(const struct kl_menu *menu, uint32_t id, uint32_t parent, uint32_t before, unsigned type, const char *label);
static int menu_room(struct kl_menu *menu);
static int menu_change(const struct kl_menu *menu, uint32_t id);

/* The window menu's events, handed on to the application's listener. */
static const struct xdg_toplevel_menu_v1_listener menu_place_listener = {
	menu_activated, menu_opened, menu_closed
};

/* A context menu's events, handed on to the application's listener. */
static const struct xdg_context_menu_v1_listener menu_context_listener = {
	menu_context_activated, menu_context_done
};

/*
 * Opens the connection's menu service: the compositor's xdg_menu_manager_v1,
 * bound from an application's registry or found by a search of the
 * library's own (on a queue of its own, so no event of the application's
 * is dispatched by it).
 */
struct kl_menu_service *
kl_menu_service_open(
	struct wl_display *display)
{
	struct kl_menu_service *service;
	struct keiui_global_search search;
	int error;

	/* The manager's global. */
	error = keiui_global_find(&search, display, "xdg_menu_manager_v1");
	if (error != 0) {
		keiui_global_end(&search);
		errno = error;
		return NULL;
	}

	/* The service's record, when the manager was announced. */
	service = NULL;
	if (search.name != 0U)
		service = calloc(1, sizeof(*service));

	/* The manager, bound at the newest version both sides speak, on the application's default queue. */
	if (service != NULL) {
		service->display = display;
		service->version = MENU_VERSION;
		if (search.version >= MENU_VERSION_CONTEXT)
			service->version = MENU_VERSION_CONTEXT;
		service->manager = keiui_global_bind(&search, &xdg_menu_manager_v1_interface, service->version);
	}

	/* The search ends. */
	keiui_global_end(&search);

	/* A compositor without the System Menu: the application draws its own menus. */
	if (search.name == 0U) {
		errno = ENOTSUP;
		return NULL;
	}

	/* The service or its binding could not be made. */
	if (service == NULL || service->manager == NULL) {
		free(service);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: menus can be made. */
	return service;
}

/*
 * Closes a menu service; the menus and window menus made from it stay.
 */
void
kl_menu_service_close(
	struct kl_menu_service *service)
{
	/* No service, nothing to close. */
	if (service == NULL)
		return;

	/* The binding goes, then the record. */
	xdg_menu_manager_v1_destroy(service->manager);
	free(service);
}

/*
 * Makes an empty menu.
 */
struct kl_menu *
kl_menu_create(
	struct kl_menu_service *service)
{
	struct kl_menu *menu;

	/* The record, with an empty mirror. */
	menu = calloc(1, sizeof(*menu));
	if (menu == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The protocol object. */
	menu->proxy = xdg_menu_manager_v1_create_menu(service->manager);
	if (menu->proxy == NULL) {
		free(menu);
		errno = ENOMEM;
		return NULL;
	}

	/* Succeeded: the menu is empty until its first commit. */
	return menu;
}

/*
 * Destroys a menu; windows showing it show no menu.
 */
void
kl_menu_destroy(
	struct kl_menu *menu)
{
	/* No menu, nothing to destroy. */
	if (menu == NULL)
		return;

	/* The protocol object, the mirror, the record. */
	xdg_menu_v1_destroy(menu->proxy);
	free(menu->entries);
	free(menu);
}

/*
 * Starts a transaction.
 */
int
kl_menu_begin(
	struct kl_menu *menu)
{
	/* One at a time. */
	if (menu->updating)
		return EBUSY;

	/* A new serial names the transaction, which is open until its commit. */
	menu->serial++;
	menu->updating = 1;

	/* The compositor hears it begin. */
	xdg_menu_v1_begin_update(menu->proxy, menu->serial);

	/* Succeeded: changes may follow. */
	return 0;
}

/*
 * Ends a transaction; the compositor shows its changes at once.
 */
int
kl_menu_commit(
	struct kl_menu *menu)
{
	/* Only an open transaction ends. */
	if (!menu->updating)
		return EINVAL;

	/* The transaction is closed. */
	menu->updating = 0;

	/* The commit names its serial. */
	xdg_menu_v1_commit(menu->proxy, menu->serial);

	/* Succeeded: the changes are shown together. */
	return 0;
}

/*
 * Adds an item as the last child of a parent.
 */
int
kl_menu_append(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t parent,
	unsigned type,
	const char *label,
	uint32_t action)
{
	int error;

	/* The same as an insert before nothing. */
	error = kl_menu_insert(menu, id, parent, 0U, type, label, action);
	if (error != 0)
		return error;

	/* Succeeded: the item is the parent's last child. */
	return 0;
}

/*
 * Adds an item before one of a parent's children (0 appends).
 */
int
kl_menu_insert(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t parent,
	uint32_t before,
	unsigned type,
	const char *label,
	uint32_t action)
{
	struct menu_entry *entry;
	int error;

	/* No label is an empty one. */
	if (label == NULL)
		label = "";

	/* What the compositor would refuse is refused here. */
	error = menu_check_add(menu, id, parent, before, type, label);
	if (error != 0)
		return error;

	/* Room in the mirror. */
	error = menu_room(menu);
	if (error != 0)
		return error;

	/* The mirror learns the item. */
	entry = &menu->entries[menu->count];
	entry->id = id;
	entry->parent = parent;
	entry->type = type;
	menu->count++;

	/* The request: an append when nothing is named to go before. */
	if (before == 0U) {
		xdg_menu_v1_append_item(menu->proxy, id, parent, type, label, action);
	} else {
		xdg_menu_v1_insert_item(menu->proxy, id, parent, before, type, label, action);
	}

	/* Succeeded: the item is in the transaction. */
	return 0;
}

/*
 * Removes an item and everything under it.
 */
int
kl_menu_remove(
	struct kl_menu *menu,
	uint32_t id)
{
	unsigned char *doomed;
	unsigned index;
	unsigned kept;
	unsigned more;
	int found;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* A mark for each entry that goes. */
	doomed = calloc(menu->count, 1);
	if (doomed == NULL)
		return ENOMEM;

	/* The item goes first (menu_change found it). */
	found = menu_find(menu, id);
	doomed[found] = 1;

	/* The marks spread to children until no new one is made. */
	more = 1;
	while (more) {
		more = 0;
		for (index = 0; index < menu->count; index++) {
			/* An entry already marked is passed. */
			if (doomed[index])
				continue;

			/* A child of a doomed entry goes too, and another pass follows. */
			found = menu_find(menu, menu->entries[index].parent);
			if (found >= 0 && doomed[found]) {
				doomed[index] = 1;
				more = 1;
			}
		}
	}

	/* The rest close up. */
	kept = 0;
	for (index = 0; index < menu->count; index++) {
		/* A doomed entry is dropped. */
		if (doomed[index])
			continue;

		/* The others are kept, in order. */
		menu->entries[kept] = menu->entries[index];
		kept++;
	}

	/* The mirror keeps the rest, and the marks go. */
	menu->count = kept;
	free(doomed);

	/* The compositor removes the item and its subtree too. */
	xdg_menu_v1_remove_item(menu->proxy, id);

	/* Succeeded: the item and its subtree are gone. */
	return 0;
}

/*
 * Sets an item's label.
 */
int
kl_menu_set_label(
	struct kl_menu *menu,
	uint32_t id,
	const char *label)
{
	size_t length;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* No label is an empty one. */
	if (label == NULL)
		label = "";

	/* A label has a bound. */
	length = strlen(label);
	if (length > MENU_TEXT_MAX)
		return E2BIG;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_label(menu->proxy, id, label);
	return 0;
}

/*
 * Sets the action an item's choice reports.
 */
int
kl_menu_set_action(
	struct kl_menu *menu,
	uint32_t id,
	uint32_t action)
{
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_action(menu->proxy, id, action);
	return 0;
}

/*
 * Sets whether an item can be chosen.
 */
int
kl_menu_set_enabled(
	struct kl_menu *menu,
	uint32_t id,
	int enabled)
{
	uint32_t value;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Any nonzero value is true on the wire's 0 or 1. */
	value = 0;
	if (enabled)
		value = 1;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_enabled(menu->proxy, id, value);
	return 0;
}

/*
 * Sets whether an item is shown.
 */
int
kl_menu_set_visible(
	struct kl_menu *menu,
	uint32_t id,
	int visible)
{
	uint32_t value;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Any nonzero value is true on the wire's 0 or 1. */
	value = 0;
	if (visible)
		value = 1;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_visible(menu->proxy, id, value);
	return 0;
}

/*
 * Sets whether a checkbox or radio item is checked.
 */
int
kl_menu_set_checked(
	struct kl_menu *menu,
	uint32_t id,
	int checked)
{
	uint32_t value;
	unsigned type;
	int found;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Only a checkbox or a radio item has a checked state. */
	found = menu_find(menu, id);
	type = menu->entries[found].type;
	if (type != KL_MENU_ITEM_CHECKBOX && type != KL_MENU_ITEM_RADIO)
		return EINVAL;

	/* Any nonzero value is true on the wire's 0 or 1. */
	value = 0;
	if (checked)
		value = 1;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_checked(menu->proxy, id, value);
	return 0;
}

/*
 * Sets an item's role.
 */
int
kl_menu_set_role(
	struct kl_menu *menu,
	uint32_t id,
	unsigned role)
{
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Only the roles the protocol names. */
	if (role > MENU_ROLE_LAST)
		return EINVAL;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_role(menu->proxy, id, role);
	return 0;
}

/*
 * Sets an item's icon name.
 */
int
kl_menu_set_icon_name(
	struct kl_menu *menu,
	uint32_t id,
	const char *icon_name)
{
	size_t length;
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* No name is an empty one. */
	if (icon_name == NULL)
		icon_name = "";

	/* A name has a bound. */
	length = strlen(icon_name);
	if (length > MENU_TEXT_MAX)
		return E2BIG;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_icon_name(menu->proxy, id, icon_name);
	return 0;
}

/*
 * Sets an item's shortcut.
 */
int
kl_menu_set_shortcut(
	struct kl_menu *menu,
	uint32_t id,
	unsigned modifiers,
	uint32_t keysym)
{
	int error;

	/* The item must be there, in a transaction. */
	error = menu_change(menu, id);
	if (error != 0)
		return error;

	/* Only the four modifiers the protocol names. */
	if ((modifiers & ~MENU_MODIFIERS_ALL) != 0U)
		return EINVAL;

	/* Succeeded: the request is sent. */
	xdg_menu_v1_set_shortcut(menu->proxy, id, modifiers, keysym);
	return 0;
}

/*
 * Makes the place on a window that shows a menu.
 */
struct kl_window_menu *
kl_window_menu_create(
	struct kl_menu_service *service,
	struct xdg_toplevel *toplevel,
	const struct kl_window_menu_listener *listener,
	void *data)
{
	struct kl_window_menu *window_menu;
	struct wl_event_queue *queue;
	int status;

	/* The record with the application's listener. */
	window_menu = calloc(1, sizeof(*window_menu));
	if (window_menu == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The application's callbacks and their argument. */
	window_menu->listener = listener;
	window_menu->data = data;

	/* The protocol object (the compositor allows one per window). */
	window_menu->proxy = xdg_menu_manager_v1_get_toplevel_menu(service->manager, toplevel);
	if (window_menu->proxy == NULL) {
		free(window_menu);
		errno = ENOMEM;
		return NULL;
	}

	/* Its events arrive with the window's. */
	queue = wl_proxy_get_queue((struct wl_proxy *)toplevel);
	wl_proxy_set_queue((struct wl_proxy *)window_menu->proxy, queue);

	/* The library's listener hands them on. */
	status = xdg_toplevel_menu_v1_add_listener(window_menu->proxy, &menu_place_listener, window_menu);
	if (status != 0) {
		xdg_toplevel_menu_v1_destroy(window_menu->proxy);
		free(window_menu);
		errno = EINVAL;
		return NULL;
	}

	/* Succeeded: the window can show a menu. */
	return window_menu;
}

/*
 * Opens a menu as a context menu at a point of a surface, in answer to a
 * press (its seat and serial).  Returns NULL with errno set: ENOTSUP for a
 * compositor without context menus, ENOMEM or EINVAL.
 */
struct kl_context_menu *
kl_menu_popup(
	struct kl_menu_service *service,
	struct kl_menu *menu,
	struct wl_surface *surface,
	int32_t x,
	int32_t y,
	struct wl_seat *seat,
	uint32_t serial,
	const struct kl_context_menu_listener *listener,
	void *data)
{
	struct kl_context_menu *context_menu;
	struct wl_event_queue *queue;
	int status;

	/* Context menus came with the protocol's version 2. */
	if (service->version < MENU_VERSION_CONTEXT) {
		errno = ENOTSUP;
		return NULL;
	}

	/* The record with the application's listener. */
	context_menu = calloc(1, sizeof(*context_menu));
	if (context_menu == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* The application's callbacks and their argument. */
	context_menu->listener = listener;
	context_menu->data = data;

	/* The protocol object, which opens the menu. */
	context_menu->proxy = xdg_menu_manager_v1_get_context_menu(service->manager, menu->proxy, surface, x, y, seat, serial);
	if (context_menu->proxy == NULL) {
		free(context_menu);
		errno = ENOMEM;
		return NULL;
	}

	/* Its events arrive with the surface's. */
	queue = wl_proxy_get_queue((struct wl_proxy *)surface);
	wl_proxy_set_queue((struct wl_proxy *)context_menu->proxy, queue);

	/* The library's listener hands them on. */
	status = xdg_context_menu_v1_add_listener(context_menu->proxy, &menu_context_listener, context_menu);
	if (status != 0) {
		xdg_context_menu_v1_destroy(context_menu->proxy);
		free(context_menu);
		errno = EINVAL;
		return NULL;
	}

	/* Succeeded: the menu opens; the choice and the end come to the listener. */
	return context_menu;
}

/*
 * Destroys a context menu; one still open closes without telling.
 */
void
kl_context_menu_destroy(
	struct kl_context_menu *context_menu)
{
	/* No context menu, nothing to destroy. */
	if (context_menu == NULL)
		return;

	/* The protocol object, then the record. */
	xdg_context_menu_v1_destroy(context_menu->proxy);
	free(context_menu);
}

/*
 * Shows a menu on the window (NULL shows none).
 */
int
kl_window_menu_set(
	struct kl_window_menu *window_menu,
	struct kl_menu *menu)
{
	struct xdg_menu_v1 *proxy;

	/* The menu's protocol object, or none. */
	proxy = NULL;
	if (menu != NULL)
		proxy = menu->proxy;

	/* Succeeded: the request is sent. */
	xdg_toplevel_menu_v1_set_menu(window_menu->proxy, proxy);
	return 0;
}

/*
 * Destroys a window's place for a menu.
 */
void
kl_window_menu_destroy(
	struct kl_window_menu *window_menu)
{
	/* No place, nothing to destroy. */
	if (window_menu == NULL)
		return;

	/* The protocol object, then the record. */
	xdg_toplevel_menu_v1_destroy(window_menu->proxy);
	free(window_menu);
}

/* Hands a choice on to the application's listener. */
static void
menu_activated(
	void *data,
	struct xdg_toplevel_menu_v1 *proxy,
	uint32_t item,
	uint32_t action,
	struct wl_seat *seat,
	uint32_t serial)
{
	struct kl_window_menu *window_menu;

	/* The window menu the event is for. */
	(void)proxy;
	window_menu = data;

	/* The application's callback, if it has one. */
	if (window_menu->listener == NULL || window_menu->listener->activated == NULL)
		return;
	window_menu->listener->activated(window_menu->data, window_menu, item, action, seat, serial);
}

/* Hands a submenu's opening on to the application's listener. */
static void
menu_opened(
	void *data,
	struct xdg_toplevel_menu_v1 *proxy,
	uint32_t item)
{
	struct kl_window_menu *window_menu;

	/* The window menu the event is for. */
	(void)proxy;
	window_menu = data;

	/* The application's callback, if it has one. */
	if (window_menu->listener == NULL || window_menu->listener->opened == NULL)
		return;
	window_menu->listener->opened(window_menu->data, window_menu, item);
}

/* Hands a submenu's closing on to the application's listener. */
static void
menu_closed(
	void *data,
	struct xdg_toplevel_menu_v1 *proxy,
	uint32_t item)
{
	struct kl_window_menu *window_menu;

	/* The window menu the event is for. */
	(void)proxy;
	window_menu = data;

	/* The application's callback, if it has one. */
	if (window_menu->listener == NULL || window_menu->listener->closed == NULL)
		return;
	window_menu->listener->closed(window_menu->data, window_menu, item);
}

/* Finds an entry of the mirror by its ID; -1 when there is none (and for the root). */
static int
menu_find(
	const struct kl_menu *menu,
	uint32_t id)
{
	unsigned index;

	/* The root is not an item. */
	if (id == KL_MENU_ROOT)
		return -1;

	/* A search of at most MENU_ITEMS_MAX entries. */
	for (index = 0; index < menu->count; index++) {
		/* The entry with this ID. */
		if (menu->entries[index].id == id)
			return (int)index;
	}

	/* None has it. */
	return -1;
}

/* Tells how deep an item is (the root 0, a top-level item 1). */
static unsigned
menu_depth(
	const struct kl_menu *menu,
	uint32_t id)
{
	unsigned depth;
	int found;

	/* Up the parents; the bound keeps a broken chain from looping. */
	depth = 0;
	while (id != KL_MENU_ROOT && depth <= MENU_DEPTH_MAX) {
		/* An ID the mirror does not have ends the chain. */
		found = menu_find(menu, id);
		if (found < 0)
			break;

		/* One level more, and up to its parent. */
		depth++;
		id = menu->entries[found].parent;
	}

	/* Succeeded: the depth. */
	return depth;
}

/* Checks a new item as the compositor would: 0, or why it would be refused. */
static int
menu_check_add(
	const struct kl_menu *menu,
	uint32_t id,
	uint32_t parent,
	uint32_t before,
	unsigned type,
	const char *label)
{
	unsigned depth;
	size_t length;
	int found;

	/* Only inside a transaction, with an ID (0 is none) of a known type. */
	if (!menu->updating ||
	    id == 0U ||
	    type > KL_MENU_ITEM_SUBMENU)
		return EINVAL;

	/* The ID must be new. */
	found = menu_find(menu, id);
	if (found >= 0)
		return EEXIST;

	/* The parent is the top level or an item that is there... */
	if (parent != KL_MENU_ROOT) {
		found = menu_find(menu, parent);
		if (found < 0)
			return ENOENT;

		/* ...and a submenu. */
		if (menu->entries[found].type != KL_MENU_ITEM_SUBMENU)
			return EINVAL;
	}

	/* The sibling it goes before is there... */
	if (before != 0U) {
		found = menu_find(menu, before);
		if (found < 0)
			return ENOENT;

		/* ...and the parent's child. */
		if (menu->entries[found].parent != parent)
			return EINVAL;
	}

	/* The menu stays within the compositor's bounds. */
	depth = menu_depth(menu, parent) + 1U;
	length = strlen(label);
	if (menu->count >= MENU_ITEMS_MAX ||
	    depth > MENU_DEPTH_MAX ||
	    length > MENU_TEXT_MAX)
		return E2BIG;

	/* Succeeded: the compositor will take it. */
	return 0;
}

/* Makes room for one more entry in the mirror. */
static int
menu_room(
	struct kl_menu *menu)
{
	struct menu_entry *grown;
	unsigned capacity;

	/* There is room already. */
	if (menu->count < menu->capacity)
		return 0;

	/* The array doubles (from 16 entries). */
	capacity = menu->capacity * 2U;
	if (capacity < 16U)
		capacity = 16U;

	/* The larger array, the entries kept. */
	grown = realloc(menu->entries, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;

	/* Succeeded: the mirror has room. */
	menu->entries = grown;
	menu->capacity = capacity;
	return 0;
}

/* Checks a change of an existing item: in a transaction, of an item that is there. */
static int
menu_change(
	const struct kl_menu *menu,
	uint32_t id)
{
	int found;

	/* Only inside a transaction. */
	if (!menu->updating)
		return EINVAL;

	/* The item must be there. */
	found = menu_find(menu, id);
	if (found < 0)
		return ENOENT;

	/* Succeeded: the change may be sent. */
	return 0;
}

/* Hands a context menu's choice on to the application. */
static void
menu_context_activated(
	void *data,
	struct xdg_context_menu_v1 *proxy,
	uint32_t item,
	uint32_t action,
	uint32_t serial)
{
	struct kl_context_menu *context_menu;

	/* The context menu the event is for. */
	(void)proxy;
	context_menu = data;

	/* The application's callback, when it has one. */
	if (context_menu->listener != NULL && context_menu->listener->activated != NULL)
		context_menu->listener->activated(context_menu->data, context_menu, item, action, serial);
}

/* Hands a context menu's end on to the application (which destroys it then). */
static void
menu_context_done(
	void *data,
	struct xdg_context_menu_v1 *proxy)
{
	struct kl_context_menu *context_menu;

	/* The context menu the event is for. */
	(void)proxy;
	context_menu = data;

	/* The application's callback, when it has one. */
	if (context_menu->listener != NULL && context_menu->listener->done != NULL)
		context_menu->listener->done(context_menu->data, context_menu);
}
