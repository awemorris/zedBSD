/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The root window's mode, and what a window manager that is an X client
 * needs of the server: save-sets and the end of other clients.
 *
 * A rooted server keeps the root window's pixels (draw.c) from its start,
 * painted with the root window's background, and shows them on one
 * surface of the backend (X11_SURFACE_ROOT, opened by rootless.c) while a
 * client is connected: the surface comes with the first client and goes
 * with the last.  The server manages no window itself; a client that
 * selects SubstructureRedirect on the root window (a window manager) is
 * asked to map, configure and restack its children, and frames them with
 * ReparentWindow.  Closing the surface ends every client, as closing a
 * nested X server's window does.
 *
 * ChangeSaveSet's save-sets are kept here for both modes: when a client
 * ends, each window of its save-set that is inside one of its windows is
 * reparented to the nearest ancestor that is not, keeping its place on
 * the screen, and every one of them is mapped (so a window manager's end
 * leaves the windows it framed).  KillClient ends the client that made a
 * resource; SetCloseDownMode is taken and every client's resources are
 * destroyed when it ends (DestroyAll), so nothing is retained.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* SetCloseDownMode's last mode (DestroyAll, RetainPermanent, RetainTemporary). */
#define ROOTED_CLOSE_DOWN_MAX		2U

/* KillClient's AllTemporary: the resources of every client ended with RetainTemporary. */
#define ROOTED_ALL_TEMPORARY		0U

/* The first room of the save-sets' list, which doubles when full. */
#define ROOTED_SAVES_FIRST		16U

static unsigned rooted_clients(const struct x11server *server);
static int rooted_save_find(const struct x11server *server, unsigned client, uint32_t window);
static void rooted_save_remove(struct x11server *server, unsigned at);
static void rooted_save_restore_one(struct x11server *server, unsigned client, struct x11_window *window);

/*
 * Starts a rooted server's root window: its pixels, painted with its
 * background.  A rootless server's root window has none.
 */
void
x11_rooted_start(
	struct x11server *server)
{
	/* A rootless server's root window shows nothing. */
	if (server->mode != X11SERVER_ROOTED)
		return;

	/* Succeeded: the root window's tree changes from nothing to all of it, which paints it. */
	x11_draw_begin(server, server->root, NULL);
	x11_draw_end(server);
	x11_log(server, "X11SERVER ROOTED %ux%u", server->width, server->height);
}

/*
 * Returns the window a rooted server shows on its one surface: the root
 * window while a client is connected (all of it to be shown when its
 * surface is still to be opened), or NULL while none is (its surface
 * closed).
 */
struct x11_window *
x11_rooted_shown(
	struct x11server *server)
{
	struct x11_window *root;
	unsigned count;

	/* No client: the surface goes. */
	root = server->root;
	count = rooted_clients(server);
	if (count == 0U) {
		x11_rootless_forget(server, root);
		return NULL;
	}

	/* A surface to be opened shows all of the root window. */
	if (root->surface == NULL)
		x11_mark_dirty(server, 0, 0, (int)server->width, (int)server->height);

	/* Succeeded: the root window. */
	return root;
}

/*
 * Ends every client of a rooted server whose surface the system asked to
 * close; the surface goes with the last of them.
 */
void
x11_rooted_close(
	struct x11server *server)
{
	unsigned index;

	/* Each connected client, as the end of a nested server would. */
	x11_log(server, "X11SERVER ROOTED close: every client ends");
	for (index = 0U; index < server->clients_max; index++) {
		if (server->clients[index].id == 0U)
			continue;
		x11_client_close(server, index);
	}
}

/*
 * Tells whether the server itself manages a window as its window manager:
 * a top-level window, not override-redirect, of a rootless server.
 */
int
x11_rooted_server_manages(
	const struct x11server *server,
	const struct x11_window *window)
{
	/* A rooted server leaves every window to its clients. */
	if (server->mode == X11SERVER_ROOTED)
		return 0;

	/* Not a top-level window, or a menu or tooltip, which no window manager decorates. */
	if (window->parent != server->root)
		return 0;
	if (window->override_redirect)
		return 0;

	/* Succeeded: the server manages it. */
	return 1;
}

/*
 * Puts a window into a client's save-set, or takes it out (remove
 * nonzero), the entry charged to the client.  Returns 0, or BadAlloc.
 */
unsigned
x11_save_set_change(
	struct x11server *server,
	unsigned client,
	struct x11_window *window,
	int remove)
{
	struct x11_save *grown;
	unsigned capacity;
	int at;
	int error;

	/* The window's entry in the client's save-set, if any. */
	at = rooted_save_find(server, client, window->id);

	/* Taken out, when it is there. */
	if (remove) {
		if (at >= 0)
			rooted_save_remove(server, (unsigned)at);
		return 0U;
	}

	/* Already in the save-set. */
	if (at >= 0)
		return 0U;

	/* The client holds the entry. */
	error = x11_memory_charge(server, client, sizeof(struct x11_save));
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Room for one more, doubling the list when it is full. */
	if (server->save_count == server->save_capacity) {
		capacity = ROOTED_SAVES_FIRST;
		if (server->save_capacity != 0U)
			capacity = server->save_capacity * 2U;
		grown = realloc(server->saves, (size_t)capacity * sizeof(*grown));
		if (grown == NULL) {
			x11_memory_release(server, client, sizeof(struct x11_save));
			return X11_BAD_ALLOC;
		}
		server->saves = grown;
		server->save_capacity = capacity;
	}

	/* Succeeded: the entry. */
	server->saves[server->save_count].client = client;
	server->saves[server->save_count].window = window->id;
	server->save_count++;
	return 0U;
}

/*
 * Restores the windows of an ending client's save-set (while the client
 * is still connected, so that its own redirection does not stop the
 * maps): each is reparented out of the client's windows, and mapped.  The
 * client's entries go.
 */
void
x11_save_set_restore(
	struct x11server *server,
	unsigned client)
{
	struct x11_window *window;
	uint32_t id;
	unsigned at;

	/* Each entry of the client's, from the first; restoring changes no entry, and each goes once done. */
	at = 0U;
	while (at < server->save_count) {
		if (server->saves[at].client != client) {
			at++;
			continue;
		}
		id = server->saves[at].window;
		rooted_save_remove(server, at);

		/* The window, when it is still there. */
		window = x11_window_find(server, id);
		if (window != NULL)
			rooted_save_restore_one(server, client, window);
	}
}

/*
 * Takes a window that is being destroyed out of every save-set.
 */
void
x11_save_set_forget_window(
	struct x11server *server,
	uint32_t window)
{
	unsigned at;

	/* Each entry naming it goes. */
	at = 0U;
	while (at < server->save_count) {
		if (server->saves[at].window == window) {
			rooted_save_remove(server, at);
			continue;
		}
		at++;
	}
}

/*
 * Frees the save-sets' list at the server's end.
 */
void
x11_save_set_fini(
	struct x11server *server)
{
	/* Succeeded: the list goes. */
	free(server->saves);
	server->saves = NULL;
	server->save_count = 0U;
	server->save_capacity = 0U;
}

/*
 * SetCloseDownMode: DestroyAll, RetainPermanent or RetainTemporary, taken;
 * the server destroys every client's resources when it ends.
 */
unsigned
x11_request_set_close_down_mode(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	/* The request has no more than its mode. */
	(void)server;
	(void)index;
	if (length != 4U)
		return X11_BAD_LENGTH;

	/* One of the three modes. */
	if (request[1] > ROOTED_CLOSE_DOWN_MAX)
		return X11_BAD_VALUE;

	/* Succeeded: taken (nothing is retained). */
	return 0U;
}

/*
 * KillClient: the client that made a resource ends (the asking client
 * itself too); AllTemporary has nothing to destroy, since no client's
 * resources are retained.  BadValue for an id that names no resource of a
 * connected client.
 */
unsigned
x11_request_kill_client(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	uint32_t id;
	unsigned type;
	unsigned slot;

	/* The resource. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	id = x11_read32(request + 4, server->clients[index].order);

	/* AllTemporary: no resource was retained. */
	if (id == ROOTED_ALL_TEMPORARY)
		return 0U;

	/* A resource there is, made by a client (the server's own ids are below the first client's). */
	type = x11_resource_type(server, id);
	if (type == 0U || type == X11_RESOURCE_GONE)
		return X11_BAD_VALUE;
	slot = id >> X11_ID_SHIFT;
	if (slot == 0U)
		return X11_BAD_VALUE;
	slot--;
	if (slot >= server->clients_max)
		return X11_BAD_VALUE;
	if (server->clients[slot].id == 0U)
		return X11_BAD_VALUE;

	/* Succeeded: the client ends, with its resources. */
	x11_log(server, "X11SERVER KillClient 0x%x by client %u", (unsigned)id, index);
	x11_client_close(server, slot);
	return 0U;
}

/* Counts the clients whose setup was accepted. */
static unsigned
rooted_clients(
	const struct x11server *server)
{
	unsigned count;
	unsigned index;

	/* Each slot of a connected client past its setup. */
	count = 0U;
	for (index = 0U; index < server->clients_max; index++) {
		if (server->clients[index].id == 0U)
			continue;
		if (!server->clients[index].setup || server->clients[index].ending)
			continue;
		count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Finds a window's entry in a client's save-set: its index, or -1. */
static int
rooted_save_find(
	const struct x11server *server,
	unsigned client,
	uint32_t window)
{
	unsigned at;

	/* Each entry. */
	for (at = 0U; at < server->save_count; at++) {
		if (server->saves[at].client == client && server->saves[at].window == window)
			return (int)at;
	}

	/* None. */
	return -1;
}

/* Takes one entry out of the save-sets' list, its charge given back. */
static void
rooted_save_remove(
	struct x11server *server,
	unsigned at)
{
	/* The charge, then the entry, the last one moved into its place. */
	x11_memory_release(server, server->saves[at].client, sizeof(struct x11_save));
	server->save_count--;
	server->saves[at] = server->saves[server->save_count];
}

/*
 * Restores one window of an ending client's save-set: inside one of the
 * client's windows, it goes to the nearest ancestor that is not one, at
 * the same place on the root window; then it is mapped as the client's
 * MapWindow would.
 */
static void
rooted_save_restore_one(
	struct x11server *server,
	unsigned client,
	struct x11_window *window)
{
	struct x11_window *parent;
	unsigned code;
	int x;
	int y;

	/* The nearest ancestor the client did not make. */
	parent = window->parent;
	while (parent != NULL && parent->owner == client)
		parent = parent->parent;
	if (parent == NULL)
		return;

	/* Out of the client's windows, its border's corner where it was. */
	if (parent != window->parent) {
		x = window->x - (int)window->border - parent->x;
		y = window->y - (int)window->border - parent->y;
		code = x11_window_reparent(server, client, window, parent, x, y);
		if (code != 0U)
			return;
	}

	/* Succeeded: mapped. */
	x11_window_map_by(server, client, window);
}
