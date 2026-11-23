/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window manager of the rootless server's top-level windows.
 *
 * Each top-level window that is not override-redirect is shown on a
 * system window (rootless.c), and the system's window manager moves,
 * sizes, minimizes and closes it.  This file speaks for it to the X
 * clients as ICCCM and EWMH have a window manager do: it keeps each
 * window's WM_STATE (Normal while mapped, Iconic while minimized, when the
 * window manager unmaps the window and keeps its surface, Withdrawn once
 * the client unmaps it), starts a window minimized when its WM_HINTS ask,
 * asks a client to close a window (WM_DELETE_WINDOW) or to take the focus
 * (WM_TAKE_FOCUS) when it said it wants to be asked, gives the surface the
 * limits on the window's size (WM_NORMAL_HINTS), its title (_NET_WM_NAME
 * before WM_NAME), its owner (WM_TRANSIENT_FOR) and whether it is a menu
 * (_NET_WM_WINDOW_TYPE), keeps _NET_WM_STATE (maximized, full-screen,
 * hidden) and the root window's _NET_ACTIVE_WINDOW, and answers the
 * messages clients send the root window (WM_CHANGE_STATE, _NET_WM_STATE,
 * _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW, and the synthetic UnmapNotify of
 * a minimized window being withdrawn).  The root window names the hints
 * this window manager keeps (_NET_SUPPORTED) and its own window
 * (_NET_SUPPORTING_WM_CHECK).
 *
 * It also gives the surface the window's icon (_NET_WM_ICON), its shape
 * (SHAPE's bounding region, from shape-ext.c) and its request for the
 * user's attention (WM_HINTS's urgency, _NET_WM_STATE_DEMANDS_ATTENTION,
 * which ends when the window is activated); asks the system to bring a
 * window to the front when a client sends _NET_ACTIVE_WINDOW; says the
 * size of the system's frame (_NET_FRAME_EXTENTS, also when a client asks
 * before it maps a window); and resizes a window in step with its
 * client's drawing (_NET_WM_SYNC_REQUEST and the client's SYNC counter).
 *
 * A rooted server has no window manager of its own: an X client manages
 * the root window's children, and this file keeps nothing.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The events the window manager reads: UnmapNotify and ClientMessage. */
#define WM_EVENT_UNMAP_NOTIFY		X11_EVENT_UNMAP_NOTIFY
#define WM_EVENT_CLIENT_MESSAGE		33U

/* ChangeProperty's mode that replaces the data. */
#define WM_PROPERTY_REPLACE		0U

/* WM_HINTS: the flag that its initial state is set, and the field's index (in 32-bit units). */
#define WM_HINTS_STATE			(1U << 1)
#define WM_HINTS_STATE_FIELD		2U

/* WM_HINTS's flag that the window asks for the user's attention (ICCCM 4.1.2.4's UrgencyHint). */
#define WM_HINTS_URGENCY		(1U << 8)

/*
 * WM_NORMAL_HINTS (ICCCM 4.1.2.3): the flags of the fields set, the
 * fields' indexes (in 32-bit units), how many fields a client of the
 * first version of ICCCM writes (without the base size and gravity), and
 * the largest size a field may give.
 */
#define WM_SIZE_MIN			(1U << 4)
#define WM_SIZE_MAX			(1U << 5)
#define WM_SIZE_INC			(1U << 6)
#define WM_SIZE_ASPECT			(1U << 7)
#define WM_SIZE_BASE			(1U << 8)
#define WM_SIZE_FIELD_FLAGS		0U
#define WM_SIZE_FIELD_MIN_WIDTH		5U
#define WM_SIZE_FIELD_MIN_HEIGHT	6U
#define WM_SIZE_FIELD_MAX_WIDTH		7U
#define WM_SIZE_FIELD_MAX_HEIGHT	8U
#define WM_SIZE_FIELD_WIDTH_INC		9U
#define WM_SIZE_FIELD_HEIGHT_INC	10U
#define WM_SIZE_FIELD_MIN_ASPECT_X	11U
#define WM_SIZE_FIELD_MIN_ASPECT_Y	12U
#define WM_SIZE_FIELD_MAX_ASPECT_X	13U
#define WM_SIZE_FIELD_MAX_ASPECT_Y	14U
#define WM_SIZE_FIELD_BASE_WIDTH	15U
#define WM_SIZE_FIELD_BASE_HEIGHT	16U
#define WM_SIZE_FIELDS_OLD		15U
#define WM_SIZE_FIELDS			17U
#define WM_SIZE_LIMIT			32767U

/* _NET_WM_STATE's actions (EWMH): remove, add, toggle. */
#define WM_NET_STATE_REMOVE		0U
#define WM_NET_STATE_ADD		1U
#define WM_NET_STATE_TOGGLE		2U

/*
 * The window manager's own bits of a window's state, beside the surface's
 * (x11backend.h): maximized up and down, and across.  The surface is
 * maximized when both are set; one alone is kept for _NET_WM_STATE.
 */
#define WM_HALF_VERT			(1U << 8)
#define WM_HALF_HORZ			(1U << 9)
#define WM_MAXIMIZED			(WM_HALF_VERT | WM_HALF_HORZ)

/*
 * The window manager's bit of a window that asks for the user's
 * attention (_NET_WM_STATE_DEMANDS_ATTENTION), until it is activated.
 */
#define WM_ATTENTION			(1U << 10)

/* The most atoms _NET_WM_STATE names here. */
#define WM_NET_STATES_MAX		5U

/* The most atoms the root window's _NET_SUPPORTED names. */
#define WM_SUPPORTED_MAX		32U

/*
 * _NET_WM_ICON: the size of icon the system is given (the smallest icon
 * at least this wide, else the largest), and the largest side kept.
 */
#define WM_ICON_WANTED			32U
#define WM_ICON_SIDE_MAX		256U

/*
 * _NET_WM_SYNC_REQUEST: how long the window manager waits for a client's
 * counter before it gives the window the next size the system asks for
 * anyway (milliseconds).
 */
#define WM_SYNC_TIMEOUT_MS		100U

/* The name the window manager's own window gives (_NET_WM_NAME). */
#define WM_NAME				"rootless"

/*
 * One atom the window manager interns when the server starts: its name,
 * and where in struct x11_wm_atoms its number is kept.
 */
struct wm_atom_name {
	const char *name;
	size_t offset;
};

/*
 * The atoms the window manager interns, in the order _NET_SUPPORTED names
 * the EWMH ones (from net_supported on).  Read only, shared by every
 * server of the process.
 */
static const struct wm_atom_name wm_atom_names[] = {
	{ "WM_PROTOCOLS", offsetof(struct x11_wm_atoms, protocols) },
	{ "WM_DELETE_WINDOW", offsetof(struct x11_wm_atoms, delete_window) },
	{ "WM_TAKE_FOCUS", offsetof(struct x11_wm_atoms, take_focus) },
	{ "WM_STATE", offsetof(struct x11_wm_atoms, state) },
	{ "WM_CHANGE_STATE", offsetof(struct x11_wm_atoms, change_state) },
	{ "_NET_SUPPORTED", offsetof(struct x11_wm_atoms, net_supported) },
	{ "_NET_SUPPORTING_WM_CHECK", offsetof(struct x11_wm_atoms, net_supporting_wm_check) },
	{ "_NET_WM_NAME", offsetof(struct x11_wm_atoms, net_wm_name) },
	{ "_NET_WM_STATE", offsetof(struct x11_wm_atoms, net_wm_state) },
	{ "_NET_WM_STATE_FULLSCREEN", offsetof(struct x11_wm_atoms, net_wm_state_fullscreen) },
	{ "_NET_WM_STATE_MAXIMIZED_VERT", offsetof(struct x11_wm_atoms, net_wm_state_maximized_vert) },
	{ "_NET_WM_STATE_MAXIMIZED_HORZ", offsetof(struct x11_wm_atoms, net_wm_state_maximized_horz) },
	{ "_NET_WM_STATE_HIDDEN", offsetof(struct x11_wm_atoms, net_wm_state_hidden) },
	{ "_NET_WM_WINDOW_TYPE", offsetof(struct x11_wm_atoms, net_wm_window_type) },
	{ "_NET_WM_WINDOW_TYPE_MENU", offsetof(struct x11_wm_atoms, net_wm_window_type_menu) },
	{ "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", offsetof(struct x11_wm_atoms, net_wm_window_type_dropdown_menu) },
	{ "_NET_WM_WINDOW_TYPE_POPUP_MENU", offsetof(struct x11_wm_atoms, net_wm_window_type_popup_menu) },
	{ "_NET_WM_WINDOW_TYPE_TOOLTIP", offsetof(struct x11_wm_atoms, net_wm_window_type_tooltip) },
	{ "_NET_WM_WINDOW_TYPE_COMBO", offsetof(struct x11_wm_atoms, net_wm_window_type_combo) },
	{ "_NET_WM_WINDOW_TYPE_DND", offsetof(struct x11_wm_atoms, net_wm_window_type_dnd) },
	{ "_NET_WM_WINDOW_TYPE_NOTIFICATION", offsetof(struct x11_wm_atoms, net_wm_window_type_notification) },
	{ "_NET_ACTIVE_WINDOW", offsetof(struct x11_wm_atoms, net_active_window) },
	{ "_NET_CLOSE_WINDOW", offsetof(struct x11_wm_atoms, net_close_window) },
	{ "_NET_WM_STATE_DEMANDS_ATTENTION", offsetof(struct x11_wm_atoms, net_wm_state_demands_attention) },
	{ "_NET_WM_ICON", offsetof(struct x11_wm_atoms, net_wm_icon) },
	{ "_NET_FRAME_EXTENTS", offsetof(struct x11_wm_atoms, net_frame_extents) },
	{ "_NET_REQUEST_FRAME_EXTENTS", offsetof(struct x11_wm_atoms, net_request_frame_extents) },
	{ "_NET_WM_SYNC_REQUEST", offsetof(struct x11_wm_atoms, net_wm_sync_request) },
	{ "_NET_WM_SYNC_REQUEST_COUNTER", offsetof(struct x11_wm_atoms, net_wm_sync_request_counter) }
};

/* The index in wm_atom_names of the first EWMH atom, the first _NET_SUPPORTED names. */
#define WM_FIRST_NET_ATOM		5U

static int wm_managed(struct x11server *server, const struct x11_window *window);
static uint32_t wm_atom_at(const struct x11server *server, unsigned index);
static int wm_write_state(struct x11server *server, struct x11_window *window);
static void wm_withdraw(struct x11server *server, struct x11_window *window);
static void wm_minimize(struct x11server *server, struct x11_window *window, int tell_surface);
static unsigned wm_initial_state(struct x11_window *window);
static unsigned wm_net_state_read(struct x11server *server, struct x11_window *window);
static unsigned wm_net_state_bit(struct x11server *server, uint32_t atom);
static void wm_net_state_message(struct x11server *server, struct x11_window *window, uint32_t action, uint32_t first, uint32_t second);
static unsigned wm_surface_flags(struct x11_window *window);
static int wm_popup_type(struct x11server *server, struct x11_window *window);
static struct x11_surface *wm_owner(struct x11server *server, struct x11_window *window);
static void wm_hints_read(struct x11_window *window, struct x11_size_hints *hints);
static uint32_t wm_field(const struct x11_property *property, unsigned field);
static unsigned wm_size_field(const struct x11_property *property, unsigned field);
static void wm_client_message(struct x11server *server, struct x11_window *window, uint32_t type, uint32_t first, uint32_t second);
static void wm_queue_close(struct x11server *server, struct x11_window *window);
static void wm_touch(struct x11server *server, struct x11_window *window, unsigned changed);
static size_t wm_text(const struct x11_property *property, uint32_t utf8, char *title, size_t size);
static size_t wm_utf8_length(const uint8_t *bytes, size_t left);
static unsigned wm_steps(unsigned size, unsigned base, unsigned step);
static int wm_sync_counter_of(struct x11server *server, struct x11_window *window, uint32_t *counter);
static void wm_sync_request(struct x11server *server, struct x11_window *window);
static void wm_sync_release(struct x11server *server, struct x11_window *window);
static void wm_write_frame(struct x11server *server, struct x11_window *window, const uint32_t *extents);
static int wm_icon_pick(const struct x11_property *icons, size_t *offset, unsigned *width, unsigned *height);

/*
 * Starts the window manager: interns its atoms, makes its own window and
 * names it and the hints it keeps on the root window (a rooted server's
 * window manager is a client's, which names its own).  Returns 0, or an
 * errno value.
 */
int
x11_wm_init(
	struct x11server *server)
{
	uint32_t supported[WM_SUPPORTED_MAX];
	uint8_t data[WM_SUPPORTED_MAX * 4U];
	struct x11_window *check;
	uint32_t *slot;
	uint32_t atom;
	unsigned count;
	unsigned index;
	unsigned code;
	int error;

	/* Each atom the window manager speaks of. */
	for (index = 0U; index < sizeof(wm_atom_names) / sizeof(wm_atom_names[0]); index++) {
		atom = x11_atom_intern(server, wm_atom_names[index].name, strlen(wm_atom_names[index].name), 0);
		if (atom == 0U)
			return ENOMEM;
		slot = (uint32_t *)((char *)&server->wm + wm_atom_names[index].offset);
		*slot = atom;
	}

	/* A rooted server leaves the window manager's window and hints to its window manager. */
	server->wm_active = 0U;
	if (server->mode == X11SERVER_ROOTED)
		return 0;

	/* The window manager's own window. */
	error = x11_window_server_child(server, X11_WM_CHECK_XID);
	if (error != 0)
		return error;
	check = x11_window_find(server, X11_WM_CHECK_XID);
	if (check == NULL)
		return ENOMEM;

	/* It names itself, and gives its name, as EWMH has the window manager's window do. */
	x11_write32(data, X11_WM_CHECK_XID, 0);
	code = x11_property_set(server, check, server->wm.net_supporting_wm_check, X11_ATOM_WINDOW, 32U, WM_PROPERTY_REPLACE, data, 4U, X11_NO_CLIENT);
	if (code != 0U)
		return ENOMEM;
	code = x11_property_set(server, check, server->wm.net_wm_name, server->atom_utf8, 8U, WM_PROPERTY_REPLACE, (const uint8_t *)WM_NAME, sizeof(WM_NAME) - 1U, X11_NO_CLIENT);
	if (code != 0U)
		return ENOMEM;

	/* The root window names it too. */
	code = x11_property_set(server, server->root, server->wm.net_supporting_wm_check, X11_ATOM_WINDOW, 32U, WM_PROPERTY_REPLACE, data, 4U, X11_NO_CLIENT);
	if (code != 0U)
		return ENOMEM;

	/* The EWMH hints the window manager keeps. */
	count = 0U;
	for (index = WM_FIRST_NET_ATOM; index < sizeof(wm_atom_names) / sizeof(wm_atom_names[0]); index++) {
		supported[count] = wm_atom_at(server, index);
		count++;
	}
	for (index = 0U; index < count; index++)
		x11_write32(data + index * 4U, supported[index], 0);
	code = x11_property_set(server, server->root, server->wm.net_supported, X11_ATOM_ATOM, 32U, WM_PROPERTY_REPLACE, data, (size_t)count * 4U, X11_NO_CLIENT);
	if (code != 0U)
		return ENOMEM;

	/* No window is active yet. */
	x11_write32(data, 0U, 0);
	code = x11_property_set(server, server->root, server->wm.net_active_window, X11_ATOM_WINDOW, 32U, WM_PROPERTY_REPLACE, data, 4U, X11_NO_CLIENT);
	if (code != 0U)
		return ENOMEM;

	/* Succeeded: the window manager is there. */
	server->wm_active = 0U;
	return 0;
}

/*
 * Ends the window manager: its own window goes (unless a client destroyed
 * it already).
 */
void
x11_wm_fini(
	struct x11server *server)
{
	struct x11_window *check;

	/* Nothing was made. */
	if (server->root == NULL)
		return;

	/* The window, while it is still the server's. */
	check = x11_window_find(server, X11_WM_CHECK_XID);
	if (check == NULL)
		return;
	if (check->owner != X11_NO_CLIENT)
		return;

	/* Succeeded: destroyed. */
	x11_window_destroy(server, check);
}

/*
 * Decides how a top-level window leaves the Withdrawn state when its
 * client maps it (ICCCM 4.1.4): Normal, or Iconic when its WM_HINTS ask
 * to start so, which keeps it unmapped and its surface minimized.  A
 * minimized window mapped again is Normal again.  Returns 1 when the
 * window stays unmapped, 0 when it is mapped.
 */
int
x11_wm_map(
	struct x11server *server,
	struct x11_window *window)
{
	unsigned initial;
	int managed;

	/* Not a window the window manager keeps. */
	managed = wm_managed(server, window);
	if (!managed)
		return 0;

	/* A minimized window mapped again (by its client, or by the system's restore) is Normal. */
	if (window->wm_state == X11_WM_ICONIC) {
		window->wm_state = X11_WM_NORMAL;
		window->wm_flags &= ~X11_SURFACE_ICONIC;
		window->wm_changed |= X11_SURFACE_CHANGED_FLAGS;
		(void)wm_write_state(server, window);
		return 0;
	}

	/* A Normal window is mapped already. */
	if (window->wm_state != X11_WM_WITHDRAWN)
		return 0;

	/* Out of the Withdrawn state: maximized or full-screen as its _NET_WM_STATE asks. */
	window->wm_flags = wm_net_state_read(server, window);
	window->wm_changed = 0U;

	/* Minimized from the start when its WM_HINTS ask: not mapped, its surface opened minimized. */
	initial = wm_initial_state(window);
	if (initial == X11_WM_ICONIC) {
		window->wm_state = X11_WM_ICONIC;
		window->wm_flags |= X11_SURFACE_ICONIC;
		(void)wm_write_state(server, window);
		wm_touch(server, window, 0U);
		return 1;
	}

	/* Succeeded: Normal, and mapped. */
	window->wm_state = X11_WM_NORMAL;
	(void)wm_write_state(server, window);
	return 0;
}

/*
 * Hears that a window was unmapped: a Normal top-level window its client
 * unmapped is Withdrawn.  The window manager's own unmapping of a window
 * it minimizes leaves it Iconic.
 */
void
x11_wm_unmapped(
	struct x11server *server,
	struct x11_window *window)
{
	int managed;

	/* Not a window the window manager keeps. */
	managed = wm_managed(server, window);
	if (!managed)
		return;

	/* Minimized, or Withdrawn already. */
	if (window->wm_state != X11_WM_NORMAL)
		return;

	/* Succeeded: Withdrawn. */
	wm_withdraw(server, window);
}

/*
 * Closes a top-level window the system asked to close (its close button,
 * Alt+F4, the desktop's close): a client that names WM_DELETE_WINDOW in
 * its WM_PROTOCOLS is asked to close it (ICCCM 4.2.8.1); any other client
 * is ended, with all its windows.
 */
void
x11_wm_close(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_client *client;
	int asks;

	/* A window of no client (the server's own). */
	client = x11_client_of(server, window->owner);
	if (client == NULL)
		return;

	/* A client that wants to be asked is. */
	asks = x11_wm_protocol(server, window, server->wm.delete_window);
	if (asks) {
		wm_client_message(server, window, server->wm.protocols, server->wm.delete_window, server->now);
		x11_log(server, "X11SERVER WM delete window=0x%x", (unsigned)window->id);
		return;
	}

	/* Succeeded: the client goes, with all its windows. */
	x11_log(server, "X11SERVER WM close client of window=0x%x", (unsigned)window->id);
	x11_client_close(server, window->owner);
}

/*
 * Follows the state the system or the user gave a window's surface
 * (X11_SURFACE_ICONIC, MAXIMIZED, FULLSCREEN): minimized, the window is
 * unmapped and Iconic; brought back, it is mapped and Normal; maximized or
 * full-screen, _NET_WM_STATE says so.  The surface is not told back what
 * it reported.
 */
void
x11_wm_backend_state(
	struct x11server *server,
	struct x11_window *window,
	unsigned state)
{
	unsigned flags;
	int managed;

	/* Not a window the window manager keeps, or one without a surface. */
	managed = wm_managed(server, window);
	if (!managed)
		return;
	if (window->wm_state == X11_WM_WITHDRAWN)
		return;

	/* Maximized and full-screen as the system has them now (a request for attention stays). */
	flags = window->wm_flags & (X11_SURFACE_ICONIC | WM_ATTENTION);
	if ((state & X11_SURFACE_MAXIMIZED) != 0U)
		flags |= WM_MAXIMIZED;
	if ((state & X11_SURFACE_FULLSCREEN) != 0U)
		flags |= X11_SURFACE_FULLSCREEN;

	/* Minimized: unmapped, Iconic. */
	if ((state & X11_SURFACE_ICONIC) != 0U && window->wm_state == X11_WM_NORMAL) {
		window->wm_flags = flags;
		wm_minimize(server, window, 0);
		return;
	}

	/* Brought back: mapped, Normal (x11_wm_map writes the state). */
	if ((state & X11_SURFACE_ICONIC) == 0U && window->wm_state == X11_WM_ICONIC) {
		window->wm_flags = flags;
		x11_window_map(server, window);
		return;
	}

	/* Nothing else changed. */
	if (flags == window->wm_flags)
		return;

	/* Succeeded: maximized or full-screen, said in _NET_WM_STATE. */
	window->wm_flags = flags;
	(void)wm_write_state(server, window);
}

/*
 * Answers an event a client sent the root window (SendEvent, in the
 * core's byte order): ICCCM's WM_CHANGE_STATE (minimize), the synthetic
 * UnmapNotify that withdraws a minimized window (ICCCM 4.1.4), and EWMH's
 * _NET_WM_STATE, _NET_ACTIVE_WINDOW and _NET_CLOSE_WINDOW.
 */
void
x11_wm_root_event(
	struct x11server *server,
	const uint8_t *event)
{
	struct x11_window *window;
	uint32_t type;
	uint32_t first;
	uint32_t second;
	uint32_t third;
	uint32_t time;
	unsigned code;
	int managed;

	/* A synthetic UnmapNotify withdraws a minimized window. */
	code = event[0] & 0x7fU;
	if (code == WM_EVENT_UNMAP_NOTIFY) {
		window = x11_window_find(server, x11_read32(event + 8, 0));
		managed = wm_managed(server, window);
		if (managed && window->wm_state == X11_WM_ICONIC)
			wm_withdraw(server, window);
		return;
	}

	/* Otherwise only a ClientMessage of 32-bit data, about a window the window manager keeps. */
	if (code != WM_EVENT_CLIENT_MESSAGE || event[1] != 32U)
		return;
	window = x11_window_find(server, x11_read32(event + 4, 0));
	managed = wm_managed(server, window);
	if (!managed)
		return;
	type = x11_read32(event + 8, 0);
	first = x11_read32(event + 12, 0);
	second = x11_read32(event + 16, 0);
	third = x11_read32(event + 20, 0);

	/* _NET_REQUEST_FRAME_EXTENTS: the frame the window will have, before it is mapped (EWMH). */
	if (type == server->wm.net_request_frame_extents) {
		wm_write_frame(server, window, server->wm_frame);
		return;
	}

	/* WM_CHANGE_STATE to IconicState: a Normal window is minimized, its surface with it. */
	if (type == server->wm.change_state) {
		if (first == X11_WM_ICONIC && window->wm_state == X11_WM_NORMAL)
			wm_minimize(server, window, 1);
		return;
	}

	/* _NET_WM_STATE: maximized or full-screen added, removed or toggled. */
	if (type == server->wm.net_wm_state) {
		wm_net_state_message(server, window, first, second, third);
		return;
	}

	/* _NET_ACTIVE_WINDOW: a mapped window comes to the top and takes the focus (at the time given, else now). */
	if (type == server->wm.net_active_window) {
		if (!window->mapped)
			return;
		time = second;
		if (time == 0U)
			time = server->now;
		x11_window_raise(server, window);
		x11_input_focus_top_level(server, window, time);

		/* The system is asked to bring the surface to the front (it may only mark it). */
		wm_touch(server, window, X11_SURFACE_CHANGED_ACTIVATE);
		return;
	}

	/* Succeeded: _NET_CLOSE_WINDOW closes it as the system's close button does. */
	if (type == server->wm.net_close_window)
		wm_queue_close(server, window);
}

/*
 * Hears that a window's property changed or went: a top-level window's
 * surface is told its new title (WM_NAME, _NET_WM_NAME), owner
 * (WM_TRANSIENT_FOR) and limits on its size (WM_NORMAL_HINTS).
 */
void
x11_wm_property(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom)
{
	int managed;

	/* Not a window the window manager keeps, or before its atoms are known. */
	managed = wm_managed(server, window);
	if (!managed)
		return;
	if (server->wm.net_wm_name == 0U)
		return;

	/* The title, compared whole when the surface is brought in step. */
	if (atom == X11_ATOM_WM_NAME || atom == server->wm.net_wm_name) {
		wm_touch(server, window, X11_SURFACE_CHANGED_TITLE);
		return;
	}

	/* The owner. */
	if (atom == X11_ATOM_WM_TRANSIENT_FOR) {
		wm_touch(server, window, X11_SURFACE_CHANGED_OWNER);
		return;
	}

	/* The icon. */
	if (atom == server->wm.net_wm_icon) {
		wm_touch(server, window, X11_SURFACE_CHANGED_ICON);
		return;
	}

	/* The hints, whose urgency the surface shows. */
	if (atom == X11_ATOM_WM_HINTS) {
		wm_touch(server, window, X11_SURFACE_CHANGED_FLAGS);
		return;
	}

	/* Succeeded: the limits on its size. */
	if (atom == X11_ATOM_WM_NORMAL_HINTS)
		wm_touch(server, window, X11_SURFACE_CHANGED_HINTS);
}

/*
 * Names the top-level window the keyboard's focus is in on the root
 * window (_NET_ACTIVE_WINDOW; None while the focus is in none), when it
 * changed.
 */
void
x11_wm_focus_changed(
	struct x11server *server)
{
	struct x11_window *focus;
	struct x11_window *top;
	uint8_t data[4];
	uint32_t active;
	int managed;

	/* Before the window manager is there, or once the root window is gone. */
	if (server->wm.net_active_window == 0U || server->root == NULL)
		return;

	/* The top-level window the focus is in, when the window manager keeps it. */
	active = 0U;
	focus = x11_window_find(server, server->focus);
	if (focus != NULL) {
		top = x11_window_top_level(focus);
		managed = wm_managed(server, top);
		if (managed)
			active = top->id;
	}

	/* The same window. */
	if (active == server->wm_active)
		return;

	/* A window activated no longer asks for the user's attention (EWMH). */
	if (active != 0U) {
		top = x11_window_find(server, active);
		if (top != NULL && (top->wm_flags & WM_ATTENTION) != 0U) {
			top->wm_flags &= ~WM_ATTENTION;
			(void)wm_write_state(server, top);
			wm_touch(server, top, X11_SURFACE_CHANGED_FLAGS);
		}
	}

	/* Succeeded: named. */
	server->wm_active = active;
	x11_write32(data, active, 0);
	(void)x11_property_set(server, server->root, server->wm.net_active_window, X11_ATOM_WINDOW, 32U, WM_PROPERTY_REPLACE, data, 4U, X11_NO_CLIENT);
}

/*
 * Tells whether a window names a protocol (WM_DELETE_WINDOW,
 * WM_TAKE_FOCUS) in its WM_PROTOCOLS.
 */
int
x11_wm_protocol(
	struct x11server *server,
	struct x11_window *window,
	uint32_t protocol)
{
	struct x11_property *protocols;
	size_t offset;
	uint32_t atom;

	/* Before the window manager's atoms are known. */
	if (protocol == 0U || server->wm.protocols == 0U)
		return 0;

	/* WM_PROTOCOLS: a list of atoms. */
	protocols = x11_property_find(window, server->wm.protocols);
	if (protocols == NULL || protocols->format != 32U)
		return 0;

	/* Each atom named. */
	for (offset = 0U; offset + 4U <= protocols->length; offset += 4U) {
		atom = x11_read32(protocols->data + offset, 0);
		if (atom == protocol)
			return 1;
	}

	/* Not named. */
	return 0;
}

/*
 * Asks a window's client to take the focus (ICCCM 4.1.7's WM_TAKE_FOCUS,
 * with the time of the event that gave it).
 */
void
x11_wm_take_focus(
	struct x11server *server,
	struct x11_window *window,
	uint32_t time)
{
	/* Succeeded: asked. */
	wm_client_message(server, window, server->wm.protocols, server->wm.take_focus, time);
}

/*
 * Tells whether the window manager keeps an unmapped window's surface:
 * the window is minimized (Iconic).
 */
int
x11_wm_keeps_surface(
	const struct x11_window *window)
{
	/* Only a minimized window. */
	if (window->wm_state != X11_WM_ICONIC)
		return 0;

	/* Succeeded: its surface stays, minimized. */
	return 1;
}

/*
 * Fills the window manager's part of what a top-level window's surface
 * shows: a menu or tooltip (override-redirect, or a menu's
 * _NET_WM_WINDOW_TYPE) is a popup, else a decorated window with its state,
 * its owner's surface and the limits on its size.
 */
void
x11_wm_info(
	struct x11server *server,
	struct x11_window *window,
	struct x11_surface_info *info)
{
	int popup;

	/* An override-redirect window is a menu or a tooltip, which no window manager decorates. */
	info->kind = X11_SURFACE_TOPLEVEL;
	if (window->override_redirect) {
		info->kind = X11_SURFACE_POPUP;
		return;
	}

	/* So is a menu that asked for the window manager's place. */
	popup = wm_popup_type(server, window);
	if (popup)
		info->kind = X11_SURFACE_POPUP;

	/* Succeeded: its state, owner and limits. */
	info->flags = wm_surface_flags(window);
	info->owner = wm_owner(server, window);
	wm_hints_read(window, &info->hints);
}

/*
 * Gives a top-level window's title in UTF-8: its _NET_WM_NAME, else its
 * WM_NAME (Latin-1, or UTF-8 when its type says so), cut at a whole
 * character to the room there is; control characters are spaces.  An
 * empty string when it has neither.
 */
void
x11_wm_title(
	struct x11server *server,
	struct x11_window *window,
	char *title,
	size_t size)
{
	struct x11_property *name;
	size_t length;

	/* Nothing fits. */
	if (size == 0U)
		return;
	title[0] = '\0';

	/* _NET_WM_NAME, when it has text. */
	name = NULL;
	if (server->wm.net_wm_name != 0U)
		name = x11_property_find(window, server->wm.net_wm_name);
	if (name != NULL && name->format == 8U && name->type == server->atom_utf8) {
		length = wm_text(name, server->atom_utf8, title, size);
		if (length != 0U)
			return;
	}

	/* Succeeded: WM_NAME, or nothing. */
	name = x11_property_find(window, X11_ATOM_WM_NAME);
	if (name != NULL && name->format == 8U)
		(void)wm_text(name, server->atom_utf8, title, size);
}

/*
 * Brings a size the system gave a top-level window within the limits its
 * client put on it (WM_NORMAL_HINTS).
 */
void
x11_wm_constrain(
	struct x11server *server,
	struct x11_window *window,
	int *width,
	int *height)
{
	struct x11_size_hints hints;
	unsigned new_width;
	unsigned new_height;
	int managed;

	/* Not a window the window manager keeps. */
	managed = wm_managed(server, window);
	if (!managed)
		return;

	/* The limits, applied. */
	wm_hints_read(window, &hints);
	new_width = (unsigned)*width;
	new_height = (unsigned)*height;
	x11server_size_hints_apply(&hints, &new_width, &new_height);

	/* Succeeded: the size within them. */
	*width = (int)new_width;
	*height = (int)new_height;
}

/*
 * Gives the icon a top-level window's _NET_WM_ICON names for its surface:
 * the smallest icon at least WM_ICON_WANTED pixels wide (else the
 * largest), as ARGB pixels in a malloc'd array the caller frees.  Returns
 * 0, ENOENT when the window has no icon that can be read, or ENOMEM.
 */
int
x11_wm_icon(
	struct x11server *server,
	struct x11_window *window,
	uint32_t **pixels,
	unsigned *width,
	unsigned *height)
{
	struct x11_property *icons;
	uint32_t *copy;
	size_t offset;
	size_t count;
	size_t index;
	int found;

	/* _NET_WM_ICON: 32-bit units, each icon its width, its height and its pixels. */
	*pixels = NULL;
	*width = 0U;
	*height = 0U;
	if (server->wm.net_wm_icon == 0U)
		return ENOENT;
	icons = x11_property_find(window, server->wm.net_wm_icon);
	if (icons == NULL || icons->format != 32U)
		return ENOENT;

	/* The icon of the size wanted. */
	found = wm_icon_pick(icons, &offset, width, height);
	if (!found)
		return ENOENT;

	/* Its pixels, copied out of the property (kept least significant byte first). */
	count = (size_t)*width * (size_t)*height;
	copy = malloc(count * sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;
	for (index = 0U; index < count; index++)
		copy[index] = x11_read32(icons->data + offset + index * 4U, 0);

	/* Succeeded: the icon. */
	*pixels = copy;
	return 0;
}

/*
 * Takes the size of the system's frame around a top-level window (left,
 * right, top, bottom, in X's pixels): the window's _NET_FRAME_EXTENTS
 * says it, and a window that asks before it is mapped
 * (_NET_REQUEST_FRAME_EXTENTS) is told the last frame given.
 */
void
x11_wm_frame(
	struct x11server *server,
	struct x11_window *window,
	const uint32_t *extents)
{
	unsigned index;

	/* The frame the next window will likely have. */
	for (index = 0U; index < 4U; index++)
		server->wm_frame[index] = extents[index];

	/* Succeeded: said on the window. */
	wm_write_frame(server, window, extents);
}

/*
 * Notes that a top-level window's shape (SHAPE's bounding region) changed,
 * so that its surface is told.
 */
void
x11_wm_shape_changed(
	struct x11server *server,
	struct x11_window *window)
{
	/* Only a top-level window has a surface. */
	if (window->parent != server->root)
		return;

	/* Succeeded: the surface is told at the next showing. */
	wm_touch(server, window, X11_SURFACE_CHANGED_SHAPE);
}

/*
 * Gives a top-level window a size the system asked for in step with its
 * client's drawing (EWMH's _NET_WM_SYNC_REQUEST): a window whose client
 * named _NET_WM_SYNC_REQUEST in its WM_PROTOCOLS and keeps a counter
 * (_NET_WM_SYNC_REQUEST_COUNTER) is asked to set the counter to a new
 * value once it has drawn the size, before it hears of the size; while
 * the window manager waits for that value, a further size is held, and
 * given when the counter reaches the value or the wait times out.
 * Returns 1 when the size is held (the caller gives it nothing now), 0
 * when the caller gives it at once.
 */
int
x11_wm_sync_configure(
	struct x11server *server,
	struct x11_window *window,
	int32_t x,
	int32_t y,
	int width,
	int height)
{
	uint32_t counter;
	int managed;
	int asks;
	int has;

	/* Not a window the window manager keeps. */
	managed = wm_managed(server, window);
	if (!managed)
		return 0;

	/* A client that does not draw in step with the window manager. */
	asks = x11_wm_protocol(server, window, server->wm.net_wm_sync_request);
	if (!asks)
		return 0;
	has = wm_sync_counter_of(server, window, &counter);
	if (!has)
		return 0;

	/* While the window manager waits for the client's last size, the newest size waits. */
	if (window->sync_waiting) {
		window->sync_held = 1;
		window->sync_x = x;
		window->sync_y = y;
		window->sync_width = width;
		window->sync_height = height;
		return 1;
	}

	/* A move alone needs no drawing: given at once. */
	if (width == (int)window->width && height == (int)window->height)
		return 0;

	/* Succeeded: the client is asked, then given the size. */
	wm_sync_request(server, window);
	return 0;
}

/*
 * Hears that a counter took a value (SYNC): a window whose sync request
 * counter it is, and which waited for the value, is given the size held
 * meanwhile.
 */
void
x11_wm_sync_counter(
	struct x11server *server,
	uint32_t counter,
	int64_t value)
{
	struct x11_window *window;
	struct x11_window *next;
	uint32_t window_counter;
	int has;

	/* Before the root window is there. */
	if (server->root == NULL)
		return;

	/* Each top-level window waiting for this counter to reach its value. */
	for (window = server->root->first_child; window != NULL; window = next) {
		next = window->above;
		if (!window->sync_waiting)
			continue;
		has = wm_sync_counter_of(server, window, &window_counter);
		if (!has || window_counter != counter)
			continue;
		if (value < window->sync_value)
			continue;

		/* The wait is over: the size held, if any. */
		window->sync_waiting = 0;
		wm_sync_release(server, window);
	}
}

/*
 * Ends the waits for counters that took too long, giving each window the
 * size held meanwhile.  Returns nonzero while a window still waits.
 */
int
x11_wm_sync_tick(
	struct x11server *server)
{
	struct x11_window *window;
	struct x11_window *next;
	int waiting;

	/* Before the root window is there. */
	waiting = 0;
	if (server->root == NULL)
		return 0;

	/* Each top-level window waiting. */
	for (window = server->root->first_child; window != NULL; window = next) {
		next = window->above;
		if (!window->sync_waiting)
			continue;

		/* Not yet too long. */
		if ((int32_t)(server->now - window->sync_deadline) < 0) {
			waiting = 1;
			continue;
		}

		/* Too long: the size held is given anyway. */
		window->sync_waiting = 0;
		wm_sync_release(server, window);
		if (window->sync_waiting)
			waiting = 1;
	}

	/* Succeeded: whether a window still waits. */
	return waiting;
}

/*
 * Brings a size within a surface's limits as ICCCM 4.1.2.3 has a window
 * manager do: no smaller than the smallest and no larger than the largest
 * size, the ratio of width to height (beyond the base size) within its
 * range, and the base size plus whole steps.  The smallest size wins over
 * the rest.
 */
void
x11server_size_hints_apply(
	const struct x11_size_hints *hints,
	unsigned *width,
	unsigned *height)
{
	uint64_t over_width;
	uint64_t over_height;
	unsigned new_width;
	unsigned new_height;

	/* The largest size. */
	new_width = *width;
	new_height = *height;
	if (hints->max_width != 0U && new_width > hints->max_width)
		new_width = hints->max_width;
	if (hints->max_height != 0U && new_height > hints->max_height)
		new_height = hints->max_height;

	/* The size beyond the base, whose ratio is kept. */
	over_width = 0U;
	if (new_width > hints->base_width)
		over_width = new_width - hints->base_width;
	over_height = 0U;
	if (new_height > hints->base_height)
		over_height = new_height - hints->base_height;

	/* Too tall for the smallest ratio: shorter. */
	if (hints->min_aspect_x != 0U && hints->min_aspect_y != 0U) {
		if (over_width * hints->min_aspect_y < over_height * hints->min_aspect_x) {
			over_height = over_width * hints->min_aspect_y / hints->min_aspect_x;
			new_height = hints->base_height + (unsigned)over_height;
		}
	}

	/* Too wide for the largest ratio: narrower. */
	if (hints->max_aspect_x != 0U && hints->max_aspect_y != 0U) {
		if (over_width * hints->max_aspect_y > over_height * hints->max_aspect_x) {
			over_width = over_height * hints->max_aspect_x / hints->max_aspect_y;
			new_width = hints->base_width + (unsigned)over_width;
		}
	}

	/* Whole steps from the base size, down. */
	new_width = wm_steps(new_width, hints->base_width, hints->width_inc);
	new_height = wm_steps(new_height, hints->base_height, hints->height_inc);

	/* The smallest size, by whole steps up when they are kept. */
	if (new_width < hints->min_width) {
		new_width = hints->min_width;
		if (hints->width_inc > 1U && new_width > hints->base_width)
			new_width = hints->base_width + (new_width - hints->base_width + hints->width_inc - 1U) / hints->width_inc * hints->width_inc;
	}
	if (new_height < hints->min_height) {
		new_height = hints->min_height;
		if (hints->height_inc > 1U && new_height > hints->base_height)
			new_height = hints->base_height + (new_height - hints->base_height + hints->height_inc - 1U) / hints->height_inc * hints->height_inc;
	}

	/* A window is never empty. */
	if (new_width == 0U)
		new_width = 1U;
	if (new_height == 0U)
		new_height = 1U;

	/* Succeeded: the size within the limits. */
	*width = new_width;
	*height = new_height;
}

/*
 * Tells whether the window manager keeps a window: a top-level window
 * (the root window's child) of a rootless server that is not
 * override-redirect and not the window manager's own.
 */
static int
wm_managed(
	struct x11server *server,
	const struct x11_window *window)
{
	/* No window, or not a top-level one, or one a client of a rooted server manages. */
	if (window == NULL || server->root == NULL)
		return 0;
	if (window->parent != server->root)
		return 0;
	if (server->mode == X11SERVER_ROOTED)
		return 0;

	/* A menu or a tooltip, or the window manager's own window. */
	if (window->override_redirect)
		return 0;
	if (window->owner == X11_NO_CLIENT)
		return 0;

	/* Succeeded: kept. */
	return 1;
}

/* Returns the number of the atom at an index of wm_atom_names. */
static uint32_t
wm_atom_at(
	const struct x11server *server,
	unsigned index)
{
	const uint32_t *slot;

	/* Succeeded: kept where the table says. */
	slot = (const uint32_t *)((const char *)&server->wm + wm_atom_names[index].offset);
	return *slot;
}

/*
 * Writes a top-level window's WM_STATE (its ICCCM state, no icon window)
 * and _NET_WM_STATE (maximized, full-screen, hidden while minimized).
 * Returns 0, or nonzero when a property could not be kept.
 */
static int
wm_write_state(
	struct x11server *server,
	struct x11_window *window)
{
	uint32_t states[WM_NET_STATES_MAX];
	uint8_t data[WM_NET_STATES_MAX * 4U];
	unsigned count;
	unsigned index;
	unsigned code;

	/* WM_STATE: the state, and no icon window. */
	x11_write32(data, window->wm_state, 0);
	x11_write32(data + 4, 0U, 0);
	code = x11_property_set(server, window, server->wm.state, server->wm.state, 32U, WM_PROPERTY_REPLACE, data, 8U, X11_NO_CLIENT);
	if (code != 0U)
		return -1;

	/* _NET_WM_STATE: the states the window manager keeps. */
	count = 0U;
	if ((window->wm_flags & X11_SURFACE_FULLSCREEN) != 0U) {
		states[count] = server->wm.net_wm_state_fullscreen;
		count++;
	}
	if ((window->wm_flags & WM_HALF_VERT) != 0U) {
		states[count] = server->wm.net_wm_state_maximized_vert;
		count++;
	}
	if ((window->wm_flags & WM_HALF_HORZ) != 0U) {
		states[count] = server->wm.net_wm_state_maximized_horz;
		count++;
	}
	if (window->wm_state == X11_WM_ICONIC) {
		states[count] = server->wm.net_wm_state_hidden;
		count++;
	}
	if ((window->wm_flags & WM_ATTENTION) != 0U) {
		states[count] = server->wm.net_wm_state_demands_attention;
		count++;
	}
	for (index = 0U; index < count; index++)
		x11_write32(data + index * 4U, states[index], 0);
	code = x11_property_set(server, window, server->wm.net_wm_state, X11_ATOM_ATOM, 32U, WM_PROPERTY_REPLACE, data, (size_t)count * 4U, X11_NO_CLIENT);
	if (code != 0U)
		return -1;

	/* Succeeded: both written. */
	return 0;
}

/*
 * Withdraws a top-level window (its client unmapped it, or withdrew it
 * while minimized): WM_STATE and _NET_WM_STATE go (ICCCM 4.1.4, EWMH),
 * and its surface closes.
 */
static void
wm_withdraw(
	struct x11server *server,
	struct x11_window *window)
{
	/* Withdrawn, with no state to show. */
	window->wm_state = X11_WM_WITHDRAWN;
	window->wm_flags = 0U;
	window->wm_changed = 0U;

	/* Its window manager's properties. */
	x11_property_delete(server, window, server->wm.state);
	x11_property_delete(server, window, server->wm.net_wm_state);

	/* Succeeded: its surface closes at the next showing. */
	wm_touch(server, window, 0U);
}

/*
 * Minimizes a Normal top-level window: it is Iconic and unmapped (its
 * client hears UnmapNotify), and its surface stays, minimized.  The
 * surface is told when the client asked (WM_CHANGE_STATE), not when the
 * system minimized it.
 */
static void
wm_minimize(
	struct x11server *server,
	struct x11_window *window,
	int tell_surface)
{
	/* Iconic, said before the unmap so that the unmap does not withdraw it. */
	window->wm_state = X11_WM_ICONIC;
	window->wm_flags |= X11_SURFACE_ICONIC;
	if (tell_surface)
		window->wm_changed |= X11_SURFACE_CHANGED_FLAGS;
	(void)wm_write_state(server, window);

	/* Succeeded: unmapped, the surface kept. */
	x11_window_unmap(server, window);
	wm_touch(server, window, 0U);
}

/*
 * Returns the state a window's WM_HINTS ask it to start in
 * (X11_WM_NORMAL unless they ask for X11_WM_ICONIC).
 */
static unsigned
wm_initial_state(
	struct x11_window *window)
{
	struct x11_property *hints;
	uint32_t flags;
	uint32_t state;

	/* WM_HINTS with its initial state set. */
	hints = x11_property_find(window, X11_ATOM_WM_HINTS);
	if (hints == NULL || hints->format != 32U)
		return X11_WM_NORMAL;
	flags = wm_field(hints, 0U);
	if ((flags & WM_HINTS_STATE) == 0U)
		return X11_WM_NORMAL;

	/* Succeeded: Iconic, else Normal. */
	state = wm_field(hints, WM_HINTS_STATE_FIELD);
	if (state == X11_WM_ICONIC)
		return X11_WM_ICONIC;
	return X11_WM_NORMAL;
}

/*
 * Reads the states a client put in a window's _NET_WM_STATE before mapping
 * it that the window manager keeps (maximized, full-screen).
 */
static unsigned
wm_net_state_read(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_property *states;
	unsigned flags;
	size_t offset;

	/* A list of atoms. */
	states = x11_property_find(window, server->wm.net_wm_state);
	if (states == NULL || states->format != 32U)
		return 0U;

	/* Succeeded: each state kept. */
	flags = 0U;
	for (offset = 0U; offset + 4U <= states->length; offset += 4U)
		flags |= wm_net_state_bit(server, x11_read32(states->data + offset, 0));
	return flags;
}

/* Returns the window manager's bit of a _NET_WM_STATE atom (0: one it does not keep). */
static unsigned
wm_net_state_bit(
	struct x11server *server,
	uint32_t atom)
{
	/* The states kept; hidden is the window manager's to say. */
	if (atom == 0U)
		return 0U;
	if (atom == server->wm.net_wm_state_fullscreen)
		return X11_SURFACE_FULLSCREEN;
	if (atom == server->wm.net_wm_state_maximized_vert)
		return WM_HALF_VERT;
	if (atom == server->wm.net_wm_state_maximized_horz)
		return WM_HALF_HORZ;
	if (atom == server->wm.net_wm_state_demands_attention)
		return WM_ATTENTION;

	/* Not kept. */
	return 0U;
}

/*
 * Answers a client's _NET_WM_STATE message: the two states named are
 * removed, added or toggled; a mapped window's surface follows, and the
 * property is written again.  A window not yet mapped keeps its own
 * property (EWMH: the client sets it before the map).
 */
static void
wm_net_state_message(
	struct x11server *server,
	struct x11_window *window,
	uint32_t action,
	uint32_t first,
	uint32_t second)
{
	unsigned named;
	unsigned flags;

	/* A window not out of the Withdrawn state. */
	if (window->wm_state == X11_WM_WITHDRAWN)
		return;

	/* The states named, and the action on them. */
	named = wm_net_state_bit(server, first) | wm_net_state_bit(server, second);
	flags = window->wm_flags;
	if (action == WM_NET_STATE_REMOVE) {
		flags &= ~named;
	} else if (action == WM_NET_STATE_ADD) {
		flags |= named;
	} else if (action == WM_NET_STATE_TOGGLE) {
		flags ^= named;
	}

	/* Nothing changed. */
	if (flags == window->wm_flags)
		return;

	/* Succeeded: the surface told, the property written. */
	window->wm_flags = flags;
	(void)wm_write_state(server, window);
	wm_touch(server, window, X11_SURFACE_CHANGED_FLAGS);
}

/* Returns the state a top-level window's surface is to have. */
static unsigned
wm_surface_flags(
	struct x11_window *window)
{
	struct x11_property *hints;
	unsigned flags;
	uint32_t hint_flags;

	/* Minimized and full-screen as kept. */
	flags = window->wm_flags & (X11_SURFACE_ICONIC | X11_SURFACE_FULLSCREEN);

	/* Maximized only when both ways are. */
	if ((window->wm_flags & WM_MAXIMIZED) == WM_MAXIMIZED)
		flags |= X11_SURFACE_MAXIMIZED;

	/* Asking for the user's attention by _NET_WM_STATE. */
	if ((window->wm_flags & WM_ATTENTION) != 0U)
		flags |= X11_SURFACE_URGENT;

	/* Or by WM_HINTS's urgency. */
	hints = x11_property_find(window, X11_ATOM_WM_HINTS);
	if (hints != NULL && hints->format == 32U) {
		hint_flags = wm_field(hints, 0U);
		if ((hint_flags & WM_HINTS_URGENCY) != 0U)
			flags |= X11_SURFACE_URGENT;
	}

	/* Succeeded: the state. */
	return flags;
}

/*
 * Tells whether a window's _NET_WM_WINDOW_TYPE makes it a menu, a tooltip
 * or another window the window manager does not decorate or activate.
 * The first type the window manager knows decides.
 */
static int
wm_popup_type(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_property *types;
	size_t offset;
	uint32_t atom;

	/* A list of atoms. */
	types = x11_property_find(window, server->wm.net_wm_window_type);
	if (types == NULL || types->format != 32U)
		return 0;

	/* The first popup kind named. */
	for (offset = 0U; offset + 4U <= types->length; offset += 4U) {
		atom = x11_read32(types->data + offset, 0);
		if (atom == 0U)
			continue;
		if (atom == server->wm.net_wm_window_type_menu ||
		    atom == server->wm.net_wm_window_type_dropdown_menu ||
		    atom == server->wm.net_wm_window_type_popup_menu ||
		    atom == server->wm.net_wm_window_type_tooltip ||
		    atom == server->wm.net_wm_window_type_combo ||
		    atom == server->wm.net_wm_window_type_dnd ||
		    atom == server->wm.net_wm_window_type_notification)
			return 1;
	}

	/* A decorated window. */
	return 0;
}

/*
 * Returns the surface of the top-level window a window is transient for
 * (WM_TRANSIENT_FOR), or NULL.
 */
static struct x11_surface *
wm_owner(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_property *transient;
	struct x11_window *owner;
	struct x11_window *top;

	/* WM_TRANSIENT_FOR: one window. */
	transient = x11_property_find(window, X11_ATOM_WM_TRANSIENT_FOR);
	if (transient == NULL || transient->format != 32U || transient->length < 4U)
		return NULL;

	/* The window's top-level window, which is neither the root window (a group) nor the window itself. */
	owner = x11_window_find(server, x11_read32(transient->data, 0));
	if (owner == NULL || owner == server->root)
		return NULL;
	top = x11_window_top_level(owner);
	if (top == window)
		return NULL;

	/* Succeeded: its surface (none while it has none). */
	return top->surface;
}

/*
 * Reads a window's WM_NORMAL_HINTS into the limits on its size, with
 * ICCCM's defaults: a base size left out is the smallest size and the
 * other way round; a largest size below the smallest is no limit.
 */
static void
wm_hints_read(
	struct x11_window *window,
	struct x11_size_hints *hints)
{
	struct x11_property *normal;
	uint32_t flags;

	/* No limits without the hints (at least the fields of the first version). */
	memset(hints, 0, sizeof(*hints));
	normal = x11_property_find(window, X11_ATOM_WM_NORMAL_HINTS);
	if (normal == NULL || normal->format != 32U || normal->length < WM_SIZE_FIELDS_OLD * 4U)
		return;
	flags = wm_field(normal, WM_SIZE_FIELD_FLAGS);

	/* The smallest size. */
	if ((flags & WM_SIZE_MIN) != 0U) {
		hints->min_width = wm_size_field(normal, WM_SIZE_FIELD_MIN_WIDTH);
		hints->min_height = wm_size_field(normal, WM_SIZE_FIELD_MIN_HEIGHT);
	}

	/* The base size, when the client wrote it. */
	if ((flags & WM_SIZE_BASE) != 0U && normal->length >= WM_SIZE_FIELDS * 4U) {
		hints->base_width = wm_size_field(normal, WM_SIZE_FIELD_BASE_WIDTH);
		hints->base_height = wm_size_field(normal, WM_SIZE_FIELD_BASE_HEIGHT);
	} else if ((flags & WM_SIZE_MIN) != 0U) {
		hints->base_width = hints->min_width;
		hints->base_height = hints->min_height;
	}

	/* A smallest size left out is the base size. */
	if ((flags & WM_SIZE_MIN) == 0U) {
		hints->min_width = hints->base_width;
		hints->min_height = hints->base_height;
	}

	/* The largest size, when it is not below the smallest. */
	if ((flags & WM_SIZE_MAX) != 0U) {
		hints->max_width = wm_size_field(normal, WM_SIZE_FIELD_MAX_WIDTH);
		hints->max_height = wm_size_field(normal, WM_SIZE_FIELD_MAX_HEIGHT);
		if (hints->max_width != 0U && hints->max_width < hints->min_width)
			hints->max_width = 0U;
		if (hints->max_height != 0U && hints->max_height < hints->min_height)
			hints->max_height = 0U;
	}

	/* The steps. */
	if ((flags & WM_SIZE_INC) != 0U) {
		hints->width_inc = wm_size_field(normal, WM_SIZE_FIELD_WIDTH_INC);
		hints->height_inc = wm_size_field(normal, WM_SIZE_FIELD_HEIGHT_INC);
	}

	/* Succeeded: the range of ratios, when both ends are whole. */
	if ((flags & WM_SIZE_ASPECT) != 0U) {
		hints->min_aspect_x = wm_size_field(normal, WM_SIZE_FIELD_MIN_ASPECT_X);
		hints->min_aspect_y = wm_size_field(normal, WM_SIZE_FIELD_MIN_ASPECT_Y);
		hints->max_aspect_x = wm_size_field(normal, WM_SIZE_FIELD_MAX_ASPECT_X);
		hints->max_aspect_y = wm_size_field(normal, WM_SIZE_FIELD_MAX_ASPECT_Y);
		if (hints->min_aspect_x == 0U || hints->min_aspect_y == 0U) {
			hints->min_aspect_x = 0U;
			hints->min_aspect_y = 0U;
		}
		if (hints->max_aspect_x == 0U || hints->max_aspect_y == 0U) {
			hints->max_aspect_x = 0U;
			hints->max_aspect_y = 0U;
		}
	}
}

/* Returns a 32-bit field of a property (0 past its end). */
static uint32_t
wm_field(
	const struct x11_property *property,
	unsigned field)
{
	/* Past the end. */
	if (property->length < ((size_t)field + 1U) * 4U)
		return 0U;

	/* Succeeded: kept least significant byte first. */
	return x11_read32(property->data + (size_t)field * 4U, 0);
}

/* Returns a size field of WM_NORMAL_HINTS: a negative one is 0, a larger one than a window can have is that size. */
static unsigned
wm_size_field(
	const struct x11_property *property,
	unsigned field)
{
	int32_t value;

	/* Negative. */
	value = (int32_t)wm_field(property, field);
	if (value < 0)
		return 0U;

	/* Too large. */
	if ((uint32_t)value > WM_SIZE_LIMIT)
		return WM_SIZE_LIMIT;

	/* Succeeded: the size. */
	return (unsigned)value;
}

/*
 * Sends a ClientMessage of 32-bit data (a type and two values) to the
 * client that made a window, as a window manager's SendEvent with no mask
 * does.
 */
static void
wm_client_message(
	struct x11server *server,
	struct x11_window *window,
	uint32_t type,
	uint32_t first,
	uint32_t second)
{
	struct x11_client *client;
	uint8_t event[32];

	/* The window's client, when it is still there. */
	client = x11_client_of(server, window->owner);
	if (client == NULL)
		return;

	/* The event, marked as sent, least significant byte first. */
	memset(event, 0, sizeof(event));
	event[0] = WM_EVENT_CLIENT_MESSAGE | X11_EVENT_SENT;
	event[1] = 32U;
	x11_write32(event + 4, window->id, 0);
	x11_write32(event + 8, type, 0);
	x11_write32(event + 12, first, 0);
	x11_write32(event + 16, second, 0);

	/* Succeeded: sent. */
	x11_event_send(server, client, event);
}

/*
 * Asks for a window to be closed at the next tick, as the system's close
 * button does (not while a client's request is being answered).
 */
static void
wm_queue_close(
	struct x11server *server,
	struct x11_window *window)
{
	/* Remembered, when there is room. */
	if (server->closing_count >= X11_CLOSING_MAX)
		return;

	/* Succeeded: closed at the next tick. */
	server->closing[server->closing_count] = window->id;
	server->closing_count++;
}

/*
 * Notes what a top-level window's surface is yet to be told, and has the
 * next showing bring the surface in step (open it, close it, update it).
 */
static void
wm_touch(
	struct x11server *server,
	struct x11_window *window,
	unsigned changed)
{
	/* Succeeded: noted, and the next tick looks at the window. */
	window->wm_changed |= changed;
	x11_mark_dirty(server, window->x, window->y, 1, 1);
}

/*
 * Copies the text of an 8-bit property into a title as UTF-8: UTF-8 when
 * its type is UTF8_STRING (a byte that does not start a whole, valid
 * character is a question mark), else Latin-1 made UTF-8.  It ends at a
 * NUL or where the next character does not fit, and control characters
 * are spaces.  Returns how many bytes were copied (without the NUL
 * written after them).
 */
static size_t
wm_text(
	const struct x11_property *property,
	uint32_t utf8,
	char *title,
	size_t size)
{
	const uint8_t *data;
	size_t used;
	size_t index;
	size_t length;
	uint8_t byte;
	uint8_t encoded[2];
	int is_utf8;

	/* UTF-8 or Latin-1. */
	is_utf8 = 0;
	if (property->type == utf8)
		is_utf8 = 1;

	/* Each character, while there is room for it and the NUL. */
	data = property->data;
	used = 0U;
	index = 0U;
	while (index < property->length) {
		byte = data[index];
		if (byte == 0U)
			break;

		/* A control character is a space. */
		if (byte < 0x20U || byte == 0x7fU) {
			if (used + 1U >= size)
				break;
			title[used] = ' ';
			used++;
			index++;
			continue;
		}

		/* ASCII is itself. */
		if (byte < 0x80U) {
			if (used + 1U >= size)
				break;
			title[used] = (char)byte;
			used++;
			index++;
			continue;
		}

		/* A Latin-1 character beyond ASCII is two bytes of UTF-8. */
		if (!is_utf8) {
			if (used + 2U >= size)
				break;
			encoded[0] = (uint8_t)(0xc0U | (byte >> 6));
			encoded[1] = (uint8_t)(0x80U | (byte & 0x3fU));
			title[used] = (char)encoded[0];
			title[used + 1U] = (char)encoded[1];
			used += 2U;
			index++;
			continue;
		}

		/* A UTF-8 character that is not whole and valid is a question mark. */
		length = wm_utf8_length(data + index, property->length - index);
		if (length == 0U) {
			if (used + 1U >= size)
				break;
			title[used] = '?';
			used++;
			index++;
			continue;
		}

		/* A whole character, copied. */
		if (used + length >= size)
			break;
		memcpy(title + used, data + index, length);
		used += length;
		index += length;
	}

	/* Succeeded: the text, ended. */
	title[used] = '\0';
	return used;
}

/*
 * Returns how many bytes the UTF-8 character at the start of some bytes
 * takes (2 to 4), or 0 when they do not start a whole, valid character
 * beyond ASCII (RFC 3629: no overlong form, no surrogate, nothing past
 * U+10FFFF).
 */
static size_t
wm_utf8_length(
	const uint8_t *bytes,
	size_t left)
{
	size_t length;
	size_t index;
	uint8_t low;
	uint8_t high;

	/* The lead byte: the character's length, and the range of its second byte. */
	low = 0x80U;
	high = 0xbfU;
	if (bytes[0] >= 0xc2U && bytes[0] <= 0xdfU) {
		length = 2U;
	} else if (bytes[0] >= 0xe0U && bytes[0] <= 0xefU) {
		length = 3U;
		if (bytes[0] == 0xe0U)
			low = 0xa0U;
		if (bytes[0] == 0xedU)
			high = 0x9fU;
	} else if (bytes[0] >= 0xf0U && bytes[0] <= 0xf4U) {
		length = 4U;
		if (bytes[0] == 0xf0U)
			low = 0x90U;
		if (bytes[0] == 0xf4U)
			high = 0x8fU;
	} else {
		return 0U;
	}

	/* The bytes it needs are there. */
	if (left < length)
		return 0U;

	/* The second byte in its range, the rest continuation bytes. */
	if (bytes[1] < low || bytes[1] > high)
		return 0U;
	for (index = 2U; index < length; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U)
			return 0U;
	}

	/* Succeeded: a whole character. */
	return length;
}

/* Returns a size brought down to the base size plus whole steps (none below the base). */
static unsigned
wm_steps(
	unsigned size,
	unsigned base,
	unsigned step)
{
	/* No steps, or not beyond the base. */
	if (step <= 1U || size <= base)
		return size;

	/* Succeeded: whole steps. */
	return base + (size - base) / step * step;
}

/*
 * Reads the counter a window's client keeps for the window manager's
 * sync requests (the first of _NET_WM_SYNC_REQUEST_COUNTER).  Returns 1
 * with it, 0 when the window has none.
 */
static int
wm_sync_counter_of(
	struct x11server *server,
	struct x11_window *window,
	uint32_t *counter)
{
	struct x11_property *property;

	/* One or two counters; the first is the one the request sets. */
	property = x11_property_find(window, server->wm.net_wm_sync_request_counter);
	if (property == NULL || property->format != 32U || property->length < 4U)
		return 0;
	*counter = x11_read32(property->data, 0);
	if (*counter == 0U)
		return 0;

	/* Succeeded: the counter. */
	return 1;
}

/*
 * Asks a window's client to set its counter to the next value once it
 * has drawn the size it is about to hear of (a ClientMessage of
 * WM_PROTOCOLS: _NET_WM_SYNC_REQUEST, the time, the value's low and high
 * 32 bits), and waits for it.
 */
static void
wm_sync_request(
	struct x11server *server,
	struct x11_window *window)
{
	struct x11_client *client;
	uint8_t event[32];
	uint64_t value;

	/* The window's client, when it is still there. */
	client = x11_client_of(server, window->owner);
	if (client == NULL)
		return;

	/* The next value, which only grows. */
	window->sync_value++;
	value = (uint64_t)window->sync_value;

	/* The event, marked as sent, least significant byte first. */
	memset(event, 0, sizeof(event));
	event[0] = WM_EVENT_CLIENT_MESSAGE | X11_EVENT_SENT;
	event[1] = 32U;
	x11_write32(event + 4, window->id, 0);
	x11_write32(event + 8, server->wm.protocols, 0);
	x11_write32(event + 12, server->wm.net_wm_sync_request, 0);
	x11_write32(event + 16, server->now, 0);
	x11_write32(event + 20, (uint32_t)(value & 0xffffffffU), 0);
	x11_write32(event + 24, (uint32_t)(value >> 32), 0);
	x11_event_send(server, client, event);

	/* Succeeded: the window manager waits for the value, not for ever. */
	window->sync_waiting = 1;
	window->sync_deadline = server->now + WM_SYNC_TIMEOUT_MS;
}

/*
 * Gives a window the size the system asked for while the window manager
 * waited for its counter, if any (asking the client again first).
 */
static void
wm_sync_release(
	struct x11server *server,
	struct x11_window *window)
{
	int32_t x;
	int32_t y;
	int width;
	int height;
	int held;

	/* Nothing held. */
	if (!window->sync_held)
		return;
	window->sync_held = 0;
	x = window->sync_x;
	y = window->sync_y;
	width = window->sync_width;
	height = window->sync_height;

	/* Asked again, when the client still draws in step. */
	held = x11_wm_sync_configure(server, window, x, y, width, height);
	if (held)
		return;

	/* Succeeded: the window takes the size, and its clients are told. */
	x11_window_backend_configure(server, window, (int)x, (int)y, (uint16_t)width, (uint16_t)height);
}

/* Writes a window's _NET_FRAME_EXTENTS: left, right, top and bottom. */
static void
wm_write_frame(
	struct x11server *server,
	struct x11_window *window,
	const uint32_t *extents)
{
	uint8_t data[16];
	unsigned index;

	/* Before the window manager's atoms are known. */
	if (server->wm.net_frame_extents == 0U)
		return;

	/* Succeeded: the four sizes as CARDINALs. */
	for (index = 0U; index < 4U; index++)
		x11_write32(data + index * 4U, extents[index], 0);
	(void)x11_property_set(server, window, server->wm.net_frame_extents, X11_ATOM_CARDINAL, 32U, WM_PROPERTY_REPLACE, data, sizeof(data), X11_NO_CLIENT);
}

/*
 * Finds the icon of _NET_WM_ICON to give the system: the smallest at
 * least WM_ICON_WANTED wide, else the largest, no side over
 * WM_ICON_SIDE_MAX.  Returns 1 with the offset of its pixels in the
 * property and its size, 0 when there is none that can be read.
 */
static int
wm_icon_pick(
	const struct x11_property *icons,
	size_t *offset,
	unsigned *width,
	unsigned *height)
{
	size_t position;
	size_t pixels;
	uint32_t icon_width;
	uint32_t icon_height;
	int found;
	int better;

	/* Each icon in turn, while its pixels are all there. */
	found = 0;
	position = 0U;
	while (position + 8U <= icons->length) {
		icon_width = x11_read32(icons->data + position, 0);
		icon_height = x11_read32(icons->data + position + 4U, 0);
		if (icon_width == 0U || icon_height == 0U)
			break;
		if (icon_width > WM_ICON_SIDE_MAX || icon_height > WM_ICON_SIDE_MAX)
			break;
		pixels = (size_t)icon_width * (size_t)icon_height * 4U;
		if (pixels > icons->length - position - 8U)
			break;

		/* Better than the one found so far: large enough and smaller, or larger while none is large enough. */
		better = 0;
		if (!found) {
			better = 1;
		} else if (icon_width >= WM_ICON_WANTED && (*width < WM_ICON_WANTED || icon_width < *width)) {
			better = 1;
		} else if (*width < WM_ICON_WANTED && icon_width > *width) {
			better = 1;
		}
		if (better) {
			found = 1;
			*offset = position + 8U;
			*width = icon_width;
			*height = icon_height;
		}
		position += 8U + pixels;
	}

	/* Succeeded: whether one was found. */
	return found;
}
