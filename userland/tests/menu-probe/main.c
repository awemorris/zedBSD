/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests the compositor's System Menu protocol (WS070) and libkeiland's checks.
 *
 * Each server case opens its own connection, sends a few requests of
 * xdg_menu_manager_v1 and xdg_menu_v1 through the private protocol header,
 * and checks the protocol error the compositor answers with (its interface and
 * code), or that there is none.  The library case checks that libkeiland
 * refuses the same mistakes itself and sends nothing that would end the
 * connection.  Every case prints MENUPROBE case=NAME ok or FAIL, and the
 * run ends with MENUPROBE DONE failures=N.
 */

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <keiland/keiland.h>

#include "userland/desktop/libwayland/xdg-toplevel-menu-v1-client-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The case expects no protocol error. */
#define PROBE_NO_ERROR		0xffffffffU

/*
 * One connection of a case: the display, the globals it bound (by their
 * registry names while the registry announces them), a menu, and a window
 * for the cases that need a toplevel.
 */
struct probe_connection {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *shell;
	struct xdg_menu_manager_v1 *manager;
	struct xdg_menu_v1 *menu;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
};

/*
 * One server case: its name, the requests it sends, and the error it
 * expects (the interface's name and the code, or PROBE_NO_ERROR).
 */
struct probe_case {
	const char *name;
	void (*send)(struct probe_connection *connection);
	const char *interface;
	uint32_t code;
};

static void probe_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void probe_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static int probe_server_case(const struct probe_case *test);
static int probe_connect(struct probe_connection *connection);
static void probe_disconnect(struct probe_connection *connection);
static int probe_library(void);
static void send_outside(struct probe_connection *connection);
static void send_zero_id(struct probe_connection *connection);
static void send_duplicate(struct probe_connection *connection);
static void send_parent(struct probe_connection *connection);
static void send_checked(struct probe_connection *connection);
static void send_serial(struct probe_connection *connection);
static void send_nested(struct probe_connection *connection);
static void send_role(struct probe_connection *connection);
static void send_exists(struct probe_connection *connection);
static void send_good(struct probe_connection *connection);
static void send_removed(struct probe_connection *connection);

/* The registry's callbacks while a connection binds its globals. */
static const struct wl_registry_listener probe_registry_listener = {
	probe_global, probe_global_remove
};

/* The server cases, each on its own connection. */
static const struct probe_case probe_cases[] = {
	{ "outside", send_outside, "xdg_menu_v1", 4U },
	{ "zero-id", send_zero_id, "xdg_menu_v1", 0U },
	{ "duplicate", send_duplicate, "xdg_menu_v1", 0U },
	{ "parent", send_parent, "xdg_menu_v1", 1U },
	{ "checked", send_checked, "xdg_menu_v1", 2U },
	{ "serial", send_serial, "xdg_menu_v1", 6U },
	{ "nested", send_nested, "xdg_menu_v1", 5U },
	{ "role", send_role, "xdg_menu_v1", 3U },
	{ "exists", send_exists, "xdg_menu_manager_v1", 0U },
	{ "good", send_good, NULL, PROBE_NO_ERROR },
	{ "removed", send_removed, "xdg_menu_v1", 1U }
};

/*
 * Runs every case and reports how many failed.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned index;
	unsigned failures;
	int failed;

	/* No options. */
	(void)argc;
	(void)argv;

	/* Each server case. */
	failures = 0;
	for (index = 0; index < sizeof(probe_cases) / sizeof(probe_cases[0]); index++) {
		failed = probe_server_case(&probe_cases[index]);
		if (failed)
			failures++;
	}

	/* The library's own checks. */
	failed = probe_library();
	if (failed)
		failures++;

	/* One line for the whole run. */
	printf("MENUPROBE DONE failures=%u\n", failures);
	fflush(stdout);

	/* Reports a case that failed. */
	if (failures != 0U)
		return 1;

	/* Succeeded: every case passed. */
	return 0;
}

/* Binds the compositor, the shell and the menu manager as the registry announces them. */
static void
probe_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct probe_connection *connection;
	int same;

	/* The compositor, for a window. */
	(void)version;
	connection = data;
	same = strcmp(interface, "wl_compositor");
	if (same == 0) {
		connection->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4U);
		return;
	}

	/* The shell, for a toplevel. */
	same = strcmp(interface, "xdg_wm_base");
	if (same == 0) {
		connection->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1U);
		return;
	}

	/* The System Menu. */
	same = strcmp(interface, "xdg_menu_manager_v1");
	if (same == 0)
		connection->manager = wl_registry_bind(registry, name, &xdg_menu_manager_v1_interface, 1U);
}

/* A global going away does not matter to a short case. */
static void
probe_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to do. */
	(void)data;
	(void)registry;
	(void)name;
}

/* Runs one server case and reports whether it failed (1) or passed (0). */
static int
probe_server_case(
	const struct probe_case *test)
{
	struct probe_connection connection;
	const struct wl_interface *interface;
	uint32_t code;
	uint32_t id;
	int status;
	int same;

	/* A connection of its own, with a menu. */
	status = probe_connect(&connection);
	if (status != 0) {
		printf("MENUPROBE case=%s FAIL connect errno=%d\n", test->name, errno);
		probe_disconnect(&connection);
		return 1;
	}

	/* The requests, then a round trip that meets the answer. */
	test->send(&connection);
	status = wl_display_roundtrip(connection.display);

	/* A case that expects no error must see the round trip succeed. */
	if (test->code == PROBE_NO_ERROR) {
		code = wl_display_get_protocol_error(connection.display, NULL, NULL);
		probe_disconnect(&connection);
		if (status < 0) {
			printf("MENUPROBE case=%s FAIL code=%u\n", test->name, code);
			return 1;
		}

		/* Succeeded: the model was taken without an error. */
		printf("MENUPROBE case=%s ok\n", test->name);
		return 0;
	}

	/* Otherwise the connection ends with the expected interface's error code. */
	interface = NULL;
	id = 0;
	code = wl_display_get_protocol_error(connection.display, &interface, &id);

	/* The interface the error names, compared with the case's. */
	same = 1;
	if (interface != NULL)
		same = strcmp(interface->name, test->interface);

	/* A round trip that succeeded, no error, or another interface's or another code is a failure. */
	if (status >= 0 ||
	    interface == NULL ||
	    same != 0 ||
	    code != test->code) {
		printf("MENUPROBE case=%s FAIL status=%d code=%u want=%u\n", test->name, status, code, test->code);
		probe_disconnect(&connection);
		return 1;
	}

	/* Succeeded: the case's error came back. */
	printf("MENUPROBE case=%s ok interface=%s object=%u code=%u\n", test->name, interface->name, id, code);
	probe_disconnect(&connection);
	return 0;
}

/* Connects, binds the globals and makes a menu; returns 0 or -1 with errno set. */
static int
probe_connect(
	struct probe_connection *connection)
{
	int status;

	/* The connection and its registry. */
	memset(connection, 0, sizeof(*connection));
	connection->display = wl_display_connect(NULL);
	if (connection->display == NULL)
		return -1;
	connection->registry = wl_display_get_registry(connection->display);
	if (connection->registry == NULL)
		return -1;

	/* The globals, announced by one round trip. */
	status = wl_registry_add_listener(connection->registry, &probe_registry_listener, connection);
	if (status != 0)
		return -1;
	status = wl_display_roundtrip(connection->display);
	if (status < 0)
		return -1;

	/* The compositor must have the System Menu. */
	if (connection->manager == NULL ||
	    connection->compositor == NULL ||
	    connection->shell == NULL) {
		errno = ENOTSUP;
		return -1;
	}

	/* A menu for the case. */
	connection->menu = xdg_menu_manager_v1_create_menu(connection->manager);
	if (connection->menu == NULL)
		return -1;

	/* Succeeded: the case can send its requests. */
	return 0;
}

/* Disconnects (the objects go with the connection). */
static void
probe_disconnect(
	struct probe_connection *connection)
{
	/* The connection takes every object with it. */
	if (connection->display != NULL)
		wl_display_disconnect(connection->display);
	memset(connection, 0, sizeof(*connection));
}

/* A change outside a transaction. */
static void
send_outside(
	struct probe_connection *connection)
{
	/* No begin_update before it. */
	xdg_menu_v1_append_item(connection->menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "One", 1U);
}

/* An item ID of zero. */
static void
send_zero_id(
	struct probe_connection *connection)
{
	/* Zero is not an item's ID. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_append_item(connection->menu, 0U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Zero", 1U);
}

/* The same ID twice. */
static void
send_duplicate(
	struct probe_connection *connection)
{
	/* The second append reuses the first's ID. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_append_item(connection->menu, 5U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Five", 1U);
	xdg_menu_v1_append_item(connection->menu, 5U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Five again", 2U);
}

/* A child of an item that is not a submenu. */
static void
send_parent(
	struct probe_connection *connection)
{
	/* Item 1 is a normal item. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_append_item(connection->menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "One", 1U);
	xdg_menu_v1_append_item(connection->menu, 2U, 1U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Two", 2U);
}

/* A checked state on an item that cannot be checked. */
static void
send_checked(
	struct probe_connection *connection)
{
	/* Item 1 is a normal item. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_append_item(connection->menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "One", 1U);
	xdg_menu_v1_set_checked(connection->menu, 1U, 1U);
}

/* A commit that names another serial. */
static void
send_serial(
	struct probe_connection *connection)
{
	/* Begun as 7, committed as 8. */
	xdg_menu_v1_begin_update(connection->menu, 7U);
	xdg_menu_v1_commit(connection->menu, 8U);
}

/* A transaction inside a transaction. */
static void
send_nested(
	struct probe_connection *connection)
{
	/* Two begins. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_begin_update(connection->menu, 2U);
}

/* A role the protocol does not name. */
static void
send_role(
	struct probe_connection *connection)
{
	/* Role 99. */
	xdg_menu_v1_begin_update(connection->menu, 1U);
	xdg_menu_v1_append_item(connection->menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "One", 1U);
	xdg_menu_v1_set_role(connection->menu, 1U, 99U);
}

/* A second place for a menu on one window. */
static void
send_exists(
	struct probe_connection *connection)
{
	/* A toplevel, never committed, and two places on it. */
	connection->surface = wl_compositor_create_surface(connection->compositor);
	connection->role = xdg_wm_base_get_xdg_surface(connection->shell, connection->surface);
	connection->toplevel = xdg_surface_get_toplevel(connection->role);
	(void)xdg_menu_manager_v1_get_toplevel_menu(connection->manager, connection->toplevel);
	(void)xdg_menu_manager_v1_get_toplevel_menu(connection->manager, connection->toplevel);
}

/*
 * A correct model: a submenu with children, an insert before a sibling,
 * every attribute set, a subtree removed, and the menu shown on a window.
 */
static void
send_good(
	struct probe_connection *connection)
{
	struct xdg_toplevel_menu_v1 *place;
	struct xdg_menu_v1 *menu;

	/* The tree. */
	menu = connection->menu;
	xdg_menu_v1_begin_update(menu, 1U);
	xdg_menu_v1_append_item(menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_SUBMENU, "File", 0U);
	xdg_menu_v1_append_item(menu, 2U, 1U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Open", 10U);
	xdg_menu_v1_insert_item(menu, 3U, 1U, 2U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "New", 11U);
	xdg_menu_v1_append_item(menu, 4U, 1U, XDG_MENU_V1_ITEM_TYPE_SEPARATOR, "", 0U);
	xdg_menu_v1_append_item(menu, 5U, 1U, XDG_MENU_V1_ITEM_TYPE_CHECKBOX, "Autosave", 12U);
	xdg_menu_v1_append_item(menu, 6U, 0U, XDG_MENU_V1_ITEM_TYPE_SUBMENU, "Theme", 0U);
	xdg_menu_v1_append_item(menu, 7U, 6U, XDG_MENU_V1_ITEM_TYPE_RADIO, "Light", 13U);
	xdg_menu_v1_append_item(menu, 8U, 6U, XDG_MENU_V1_ITEM_TYPE_RADIO, "Dark", 14U);

	/* Every attribute. */
	xdg_menu_v1_set_label(menu, 2U, "Open...");
	xdg_menu_v1_set_action(menu, 2U, 20U);
	xdg_menu_v1_set_enabled(menu, 3U, 0U);
	xdg_menu_v1_set_visible(menu, 4U, 1U);
	xdg_menu_v1_set_checked(menu, 5U, 1U);
	xdg_menu_v1_set_checked(menu, 8U, 1U);
	xdg_menu_v1_set_role(menu, 3U, XDG_MENU_V1_ROLE_NEW);
	xdg_menu_v1_set_icon_name(menu, 2U, "document-open-symbolic");
	xdg_menu_v1_set_shortcut(menu, 2U, XDG_MENU_V1_MODIFIER_CTRL, 'o');

	/* The Theme submenu and its children go; the model is shown. */
	xdg_menu_v1_remove_item(menu, 6U);
	xdg_menu_v1_commit(menu, 1U);

	/* The menu on a window. */
	connection->surface = wl_compositor_create_surface(connection->compositor);
	connection->role = xdg_wm_base_get_xdg_surface(connection->shell, connection->surface);
	connection->toplevel = xdg_surface_get_toplevel(connection->role);
	place = xdg_menu_manager_v1_get_toplevel_menu(connection->manager, connection->toplevel);
	xdg_toplevel_menu_v1_set_menu(place, menu);
}

/* A child of a submenu removed with its parent's subtree. */
static void
send_removed(
	struct probe_connection *connection)
{
	struct xdg_menu_v1 *menu;

	/* A submenu with a child, removed, then a new child of it. */
	menu = connection->menu;
	xdg_menu_v1_begin_update(menu, 1U);
	xdg_menu_v1_append_item(menu, 1U, 0U, XDG_MENU_V1_ITEM_TYPE_SUBMENU, "Edit", 0U);
	xdg_menu_v1_append_item(menu, 2U, 1U, XDG_MENU_V1_ITEM_TYPE_SUBMENU, "Find", 0U);
	xdg_menu_v1_append_item(menu, 3U, 2U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Next", 1U);
	xdg_menu_v1_remove_item(menu, 1U);
	xdg_menu_v1_append_item(menu, 4U, 2U, XDG_MENU_V1_ITEM_TYPE_NORMAL, "Previous", 2U);
}

/*
 * Checks that libkeiland refuses what the compositor would, and sends
 * nothing of it: the connection survives a round trip.  Returns 1 when a
 * check failed.
 */
static int
probe_library(void)
{
	struct kl_menu_service *service;
	struct kl_menu *menu;
	struct wl_display *display;
	int results[12];
	int wanted[12];
	unsigned index;
	int status;
	int failed;
	int error;

	/* The connection and the service. */
	display = wl_display_connect(NULL);
	if (display == NULL) {
		printf("MENUPROBE case=library FAIL connect errno=%d\n", errno);
		return 1;
	}

	/* The service, found on its own queue. */
	service = kl_menu_service_open(display);
	if (service == NULL) {
		printf("MENUPROBE case=library FAIL service errno=%d\n", errno);
		wl_display_disconnect(display);
		return 1;
	}

	/* An empty menu. */
	menu = kl_menu_create(service);
	if (menu == NULL) {
		printf("MENUPROBE case=library FAIL menu errno=%d\n", errno);
		kl_menu_service_close(service);
		wl_display_disconnect(display);
		return 1;
	}

	/* Each call and what it must answer. */
	results[0] = kl_menu_append(menu, 1U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Outside", 1U);
	wanted[0] = EINVAL;
	results[1] = kl_menu_begin(menu);
	wanted[1] = 0;
	results[2] = kl_menu_begin(menu);
	wanted[2] = EBUSY;
	results[3] = kl_menu_append(menu, 0U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Zero", 1U);
	wanted[3] = EINVAL;
	results[4] = kl_menu_append(menu, 1U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "One", 1U);
	wanted[4] = 0;
	results[5] = kl_menu_append(menu, 1U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "One again", 1U);
	wanted[5] = EEXIST;
	results[6] = kl_menu_append(menu, 2U, 1U, KL_MENU_ITEM_NORMAL, "Under a normal item", 2U);
	wanted[6] = EINVAL;
	results[7] = kl_menu_append(menu, 3U, 99U, KL_MENU_ITEM_NORMAL, "Under nothing", 3U);
	wanted[7] = ENOENT;
	results[8] = kl_menu_set_checked(menu, 1U, 1);
	wanted[8] = EINVAL;
	results[9] = kl_menu_set_role(menu, 1U, 99U);
	wanted[9] = EINVAL;
	results[10] = kl_menu_commit(menu);
	wanted[10] = 0;
	results[11] = kl_menu_commit(menu);
	wanted[11] = EINVAL;

	/* The answers. */
	failed = 0;
	for (index = 0; index < sizeof(results) / sizeof(results[0]); index++) {
		/* A wrong answer names the call. */
		if (results[index] != wanted[index]) {
			printf("MENUPROBE case=library FAIL call=%u result=%d want=%d\n", index, results[index], wanted[index]);
			failed = 1;
		}
	}

	/* Nothing refused was sent: the connection is still well. */
	status = wl_display_roundtrip(display);
	if (status < 0) {
		error = wl_display_get_error(display);
		printf("MENUPROBE case=library FAIL roundtrip error=%d\n", error);
		failed = 1;
	}

	/* The objects and the connection go. */
	kl_menu_destroy(menu);
	kl_menu_service_close(service);
	wl_display_disconnect(display);

	/* Reports a check that failed. */
	if (failed)
		return 1;

	/* Succeeded: every check answered as the compositor would. */
	printf("MENUPROBE case=library ok\n");
	return 0;
}
