/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Who hears an event, and sending it.
 *
 * Every client keeps its own event mask on a window; an event goes to each
 * client whose mask on the window has the event's bit, as the core
 * protocol has it.  The events are built least significant byte first and
 * turned to each client's own order as they are sent.  Structure events go
 * to the window's StructureNotify clients and its parent's
 * SubstructureNotify clients; keys, buttons and motion start at the window
 * under the pointer (or the focus window) and go up the tree to the first
 * window a client selected them on, unless a window on the way keeps them
 * from propagating.  Which device event goes where under the focus and
 * the grabs is input.c's to decide; it builds them and sends them up the
 * tree with the functions here.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The events whose layout the core speaks of here. */
#define EVENT_KEYMAP_NOTIFY		X11_EVENT_KEYMAP_NOTIFY
#define EVENT_SELECTION_NOTIFY		31U

/* SendEvent's destinations that are not windows: the window under the pointer and the focus window. */
#define SEND_POINTER_WINDOW		0U
#define SEND_INPUT_FOCUS		1U

/* The first number of selections a window keeps room for. */
#define EVENT_SELECTS_FIRST		2U

static struct x11_event_select *event_select_find(const struct x11_window *window, unsigned client);
static void event_swap16(uint8_t *bytes);
static void event_swap32(uint8_t *bytes);

/*
 * Sets the events a client asks for on a window (0: none).  Returns 0, or
 * the error: BadValue for an unknown bit, BadAccess for a bit only one
 * client may select that another client has, BadAlloc.
 */
unsigned
x11_event_select(
	struct x11server *server,
	struct x11_window *window,
	unsigned client,
	uint32_t mask)
{
	struct x11_event_select *entry;
	struct x11_event_select *grown;
	unsigned capacity;
	unsigned index;

	(void)server;

	/* Only the core's bits. */
	if ((mask & ~X11_MASK_ALL) != 0U)
		return X11_BAD_VALUE;

	/* A bit only one client may hold is refused while another client holds it. */
	for (index = 0U; index < window->select_count; index++) {
		if (window->selects[index].client == client)
			continue;
		if ((window->selects[index].mask & mask & X11_MASK_EXCLUSIVE) != 0U)
			return X11_BAD_ACCESS;
	}

	/* The client's entry, changed or taken away. */
	entry = event_select_find(window, client);
	if (entry != NULL) {
		if (mask != 0U) {
			entry->mask = mask;
			return 0U;
		}
		index = (unsigned)(entry - window->selects);
		window->selects[index] = window->selects[window->select_count - 1U];
		window->select_count--;
		return 0U;
	}

	/* No events asked for and none held: nothing to keep. */
	if (mask == 0U)
		return 0U;

	/* Room for one more. */
	if (window->select_count == window->select_capacity) {
		capacity = EVENT_SELECTS_FIRST;
		if (window->select_capacity != 0U)
			capacity = window->select_capacity * 2U;
		grown = realloc(window->selects, (size_t)capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		window->selects = grown;
		window->select_capacity = capacity;
	}

	/* Succeeded: the client's events on the window. */
	window->selects[window->select_count].client = client;
	window->selects[window->select_count].mask = mask;
	window->select_count++;
	return 0U;
}

/*
 * Returns the events a client asked for on a window.
 */
uint32_t
x11_event_mask_of(
	const struct x11_window *window,
	unsigned client)
{
	struct x11_event_select *entry;

	/* The client's entry, if it has one. */
	entry = event_select_find(window, client);
	if (entry == NULL)
		return 0U;

	/* Succeeded: its mask. */
	return entry->mask;
}

/*
 * Returns every event any client asked for on a window.
 */
uint32_t
x11_event_masks(
	const struct x11_window *window)
{
	uint32_t mask;
	unsigned index;

	/* Each client's mask, together. */
	mask = 0U;
	for (index = 0U; index < window->select_count; index++)
		mask |= window->selects[index].mask;

	/* Succeeded: the union. */
	return mask;
}

/*
 * Returns the client that asked for any of the events of a mask on a
 * window (the one of an exclusive bit), or X11_NO_CLIENT.
 */
unsigned
x11_event_selector(
	const struct x11_window *window,
	uint32_t mask)
{
	unsigned index;

	/* The first client with one of the bits. */
	for (index = 0U; index < window->select_count; index++) {
		if ((window->selects[index].mask & mask) != 0U)
			return window->selects[index].client;
	}

	/* Nobody asked. */
	return X11_NO_CLIENT;
}

/*
 * Forgets what a client asked for on a window (the client is going).
 */
void
x11_event_forget_client(
	struct x11_window *window,
	unsigned client)
{
	struct x11_event_select *entry;
	unsigned index;

	/* The client's entry goes; the last one takes its place. */
	entry = event_select_find(window, client);
	if (entry == NULL)
		return;
	index = (unsigned)(entry - window->selects);
	window->selects[index] = window->selects[window->select_count - 1U];
	window->select_count--;
}

/*
 * Sends one event (32 bytes, least significant byte first) to a client:
 * turned to its byte order, with its last sequence number.
 */
void
x11_event_send(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *event)
{
	uint8_t copy[32];

	/* The event in the client's order. */
	memcpy(copy, event, sizeof(copy));
	if (client->order)
		x11_event_swap(copy);

	/* The client's sequence number, in every event but KeymapNotify. */
	if ((copy[0] & 0x7fU) != EVENT_KEYMAP_NOTIFY)
		x11_write16(copy + 2, client->sequence, client->order);

	/* Queued. */
	x11_client_send(server, client, copy, sizeof(copy));
}

/*
 * Sends an event (least significant byte first) to every client that
 * asked for any of a mask's events on a window.  Returns how many clients
 * it went to.
 */
unsigned
x11_event_deliver(
	struct x11server *server,
	const struct x11_window *window,
	uint32_t mask,
	const uint8_t *event)
{
	struct x11_client *client;
	unsigned delivered;
	unsigned index;

	/* Each client that asked. */
	delivered = 0U;
	for (index = 0U; index < window->select_count; index++) {
		if ((window->selects[index].mask & mask) == 0U)
			continue;
		client = x11_client_of(server, window->selects[index].client);
		if (client == NULL)
			continue;
		x11_event_send(server, client, event);
		delivered++;
	}

	/* Succeeded: the count. */
	return delivered;
}

/*
 * Sends a structure event (least significant byte first; its event window
 * is filled in here) to the window's StructureNotify clients, then to its
 * parent's SubstructureNotify clients.
 */
void
x11_event_structure(
	struct x11server *server,
	const struct x11_window *window,
	uint8_t *event)
{
	/* About itself, to the window's clients. */
	x11_write32(event + 4, window->id, 0);
	(void)x11_event_deliver(server, window, X11_MASK_STRUCTURE_NOTIFY, event);

	/* About its child, to the parent's. */
	if (window->parent == NULL)
		return;
	x11_write32(event + 4, window->parent->id, 0);
	(void)x11_event_deliver(server, window->parent, X11_MASK_SUBSTRUCTURE_NOTIFY, event);
}

/*
 * Tells the clients that asked for exposures on a window that all of it
 * must be drawn again.
 */
void
x11_event_expose(
	struct x11server *server,
	const struct x11_window *window)
{
	uint8_t event[32];

	/* An InputOnly window shows nothing. */
	if (window->window_class == X11_INPUT_ONLY)
		return;

	/* The whole window, and no more exposures follow. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_EXPOSE;
	x11_write32(event + 4, window->id, 0);
	x11_write16(event + 12, window->width, 0);
	x11_write16(event + 14, window->height, 0);
	(void)x11_event_deliver(server, window, X11_MASK_EXPOSURE, event);
}

/*
 * Tells the clients that asked for property changes on a window that a
 * property changed (or was deleted), and the selections, which may be
 * waiting for it.
 */
void
x11_event_property(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom,
	int deleted)
{
	uint8_t event[32];

	/* The window, the property, the time and the state (NewValue or Deleted). */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_PROPERTY_NOTIFY;
	x11_write32(event + 4, window->id, 0);
	x11_write32(event + 8, atom, 0);
	x11_write32(event + 12, server->now, 0);
	event[16] = 0U;
	if (deleted)
		event[16] = 1U;
	(void)x11_event_deliver(server, window, X11_MASK_PROPERTY_CHANGE, event);

	/* The selections and the window manager hear of it too. */
	x11_selection_property_event(server, window, atom, deleted);
	x11_wm_property(server, window, atom);
}

/*
 * Turns an event (32 bytes) from one byte order to the other, field by
 * field as the core protocol lays out each event.  An event the core does
 * not define is left as it is.
 */
void
x11_event_swap(
	uint8_t *event)
{
	unsigned offset;
	unsigned format;

	/* The sequence number, in every event but KeymapNotify. */
	if ((event[0] & 0x7fU) != EVENT_KEYMAP_NOTIFY)
		event_swap16(event + 2);

	/* The fields of each kind. */
	switch (event[0] & 0x7fU) {
	case 2U:
	case 3U:
	case 4U:
	case 5U:
	case 6U:
	case 7U:
	case 8U:
		/* Key, button, motion, crossing: time, root, event, child, four places, state. */
		for (offset = 4U; offset < 20U; offset += 4U)
			event_swap32(event + offset);
		for (offset = 20U; offset < 30U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 9U:
	case 10U:
	case 15U:
		/* Focus, visibility: the window. */
		event_swap32(event + 4);
		break;
	case 12U:
		/* Expose: the window, a rectangle and a count. */
		event_swap32(event + 4);
		for (offset = 8U; offset < 18U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 13U:
		/* GraphicsExpose: the drawable, a rectangle, the minor opcode and a count. */
		event_swap32(event + 4);
		for (offset = 8U; offset < 20U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 14U:
		/* NoExpose: the drawable and the minor opcode. */
		event_swap32(event + 4);
		event_swap16(event + 8);
		break;
	case 16U:
		/* CreateNotify: parent, window, a rectangle and a border. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		for (offset = 12U; offset < 22U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 17U:
	case 18U:
	case 19U:
	case 20U:
		/* DestroyNotify, UnmapNotify, MapNotify, MapRequest: event (or parent) and window. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		break;
	case 21U:
		/* ReparentNotify: event, window, parent and a place. */
		for (offset = 4U; offset < 16U; offset += 4U)
			event_swap32(event + offset);
		event_swap16(event + 16);
		event_swap16(event + 18);
		break;
	case 22U:
		/* ConfigureNotify: event, window, above, a rectangle and a border. */
		for (offset = 4U; offset < 16U; offset += 4U)
			event_swap32(event + offset);
		for (offset = 16U; offset < 26U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 23U:
		/* ConfigureRequest: parent, window, sibling, a rectangle, a border and the mask. */
		for (offset = 4U; offset < 16U; offset += 4U)
			event_swap32(event + offset);
		for (offset = 16U; offset < 28U; offset += 2U)
			event_swap16(event + offset);
		break;
	case 24U:
		/* GravityNotify: event, window and a place. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		event_swap16(event + 12);
		event_swap16(event + 14);
		break;
	case 25U:
		/* ResizeRequest: the window and a size. */
		event_swap32(event + 4);
		event_swap16(event + 8);
		event_swap16(event + 10);
		break;
	case 26U:
	case 27U:
		/* Circulate: event (or parent) and window. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		break;
	case 28U:
	case 29U:
		/* PropertyNotify (window, atom, time), SelectionClear (time, owner, selection). */
		for (offset = 4U; offset < 16U; offset += 4U)
			event_swap32(event + offset);
		break;
	case 30U:
		/* SelectionRequest: time, owner, requestor, selection, target, property. */
		for (offset = 4U; offset < 28U; offset += 4U)
			event_swap32(event + offset);
		break;
	case 31U:
		/* SelectionNotify: time, requestor, selection, target, property. */
		for (offset = 4U; offset < 24U; offset += 4U)
			event_swap32(event + offset);
		break;
	case 32U:
		/* ColormapNotify: the window and the colormap. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		break;
	case 33U:
		/* ClientMessage: the window, the type, and the data in units of its format. */
		event_swap32(event + 4);
		event_swap32(event + 8);
		format = event[1];
		if (format == 16U) {
			for (offset = 12U; offset < 32U; offset += 2U)
				event_swap16(event + offset);
		} else if (format == 32U) {
			for (offset = 12U; offset < 32U; offset += 4U)
				event_swap32(event + offset);
		}
		break;
	default:
		/* KeymapNotify, MappingNotify (bytes only) and the events of extensions. */
		break;
	}
}

/*
 * SendEvent: a client's event, marked as sent, to the clients that asked
 * for the mask's events on the destination (the window under the pointer,
 * the focus window, or a window), or up the tree to the first window where
 * one did when the client asks it to propagate; with no mask, to the
 * destination's creator.  A SelectionNotify to the root window answers the
 * server's own request for an X owner's text (selection.c).
 */
unsigned
x11_request_send_event(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *destination;
	struct x11_window *focus;
	struct x11_window *pointer;
	struct x11_window *window;
	struct x11_client *client;
	struct x11_client *creator;
	uint32_t target;
	uint32_t mask;
	uint8_t event[32];
	unsigned delivered;
	unsigned code;
	int inside;

	/* The request: propagate, the destination, the mask and the event. */
	client = &server->clients[index];
	if (length != 44U)
		return X11_BAD_LENGTH;
	if (request[1] > 1U)
		return X11_BAD_VALUE;
	target = x11_read32(request + 4, client->order);
	mask = x11_read32(request + 8, client->order);
	if ((mask & ~X11_MASK_ALL) != 0U)
		return X11_BAD_VALUE;

	/* Only a core event (no extension of this server defines one). */
	code = request[12] & 0x7fU;
	if (code < X11_EVENT_KEY_PRESS || code > X11_EVENT_LAST)
		return X11_BAD_VALUE;

	/* The event in the core's order, marked as sent. */
	memcpy(event, request + 12, sizeof(event));
	if (client->order)
		x11_event_swap(event);
	event[0] |= X11_EVENT_SENT;

	/* The focus window (PointerRoot: the root window; None: none) and the window under the pointer. */
	focus = NULL;
	if (server->focus != X11_FOCUS_NONE) {
		focus = x11_window_find(server, server->focus);
		if (focus == NULL)
			focus = server->root;
	}
	pointer = x11_input_window_under(server);

	/* The destination. */
	destination = NULL;
	if (target == SEND_POINTER_WINDOW) {
		destination = pointer;
	} else if (target == SEND_INPUT_FOCUS) {
		/* The window under the pointer when it is in the focus window, else the focus window (none: nothing is sent). */
		if (focus == NULL)
			return 0U;
		destination = focus;
		inside = x11_window_inferior(pointer, focus);
		if (inside)
			destination = pointer;
	} else {
		destination = x11_window_find(server, target);
		if (destination == NULL)
			return X11_BAD_WINDOW;
	}

	/* A SelectionNotify to the root window ends a request of the clipboard's bridge (its property names the answer). */
	if (destination == server->root && code == EVENT_SELECTION_NOTIFY) {
		x11_selection_bridge_done(server, index, event);
		return 0U;
	}

	/* The window manager hears what a client asks of it on the root window (ICCCM's and EWMH's messages). */
	if (destination == server->root)
		x11_wm_root_event(server, event);

	/* No mask: to the client that created the destination. */
	if (mask == 0U) {
		creator = x11_client_of(server, destination->owner);
		if (creator != NULL)
			x11_event_send(server, creator, event);
		return 0U;
	}

	/* To the clients that asked on the destination, else up the tree when asked to propagate. */
	window = destination;
	while (window != NULL) {
		delivered = x11_event_deliver(server, window, mask, event);
		if (delivered > 0U || request[1] == 0U)
			break;

		/* The events kept from going on, and the focus window that ends the way for InputFocus. */
		mask &= ~window->do_not_propagate;
		if (mask == 0U)
			break;
		if (target == SEND_INPUT_FOCUS && window == focus)
			break;
		window = window->parent;
	}

	/* Succeeded: sent where it could go. */
	return 0U;
}

/*
 * Fills a key, button, motion or crossing event (least significant byte
 * first) as seen from a window: the pointer on the root window and in the
 * window, the state, on this screen.
 */
void
x11_event_device(
	uint8_t *event,
	struct x11server *server,
	uint8_t type,
	uint8_t detail,
	uint32_t time,
	const struct x11_window *window,
	uint32_t child,
	uint16_t state)
{
	/* The type, detail, time, root, event window and child. */
	memset(event, 0, 32U);
	event[0] = type;
	event[1] = detail;
	x11_write32(event + 4, time, 0);
	x11_write32(event + 8, X11_ROOT_XID, 0);
	x11_write32(event + 12, window->id, 0);
	x11_write32(event + 16, child, 0);

	/* The pointer on the root window and in the window, the state, on this screen. */
	x11_write16(event + 20, (uint16_t)server->pointer_x, 0);
	x11_write16(event + 22, (uint16_t)server->pointer_y, 0);
	x11_write16(event + 24, (uint16_t)(server->pointer_x - window->x), 0);
	x11_write16(event + 26, (uint16_t)(server->pointer_y - window->y), 0);
	x11_write16(event + 28, state, 0);
	event[30] = 1U;
}

/*
 * Sends a device event from a window up the tree, to the clients of the
 * first window on the way where any asked for it, unless a window keeps it
 * from propagating or the way ends at the stop window.  With a client
 * named (not X11_NO_CLIENT), only that client hears it, and only when it
 * is among those of that first window (the event as it would reach the
 * client without a grab).  Returns the window it went to, or NULL.
 */
struct x11_window *
x11_event_propagate(
	struct x11server *server,
	struct x11_window *source,
	const struct x11_window *stop,
	uint32_t mask,
	uint8_t type,
	uint8_t detail,
	uint32_t time,
	uint16_t state,
	unsigned only)
{
	struct x11_window *window;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t child;
	uint32_t selected;
	unsigned delivered;

	/* From the source up, the child on the way named in each event. */
	window = source;
	child = 0U;
	while (window != NULL) {
		x11_event_device(event, server, type, detail, time, window, child, state);

		/* Every client that asked hears it. */
		if (only == X11_NO_CLIENT) {
			delivered = x11_event_deliver(server, window, mask, event);
			if (delivered > 0U)
				return window;
		}

		/* The first window anyone asked on ends the way; the named client hears it there if it asked. */
		selected = x11_event_masks(window);
		if (only != X11_NO_CLIENT && (selected & mask) != 0U) {
			selected = x11_event_mask_of(window, only);
			client = x11_client_of(server, only);
			if ((selected & mask) == 0U || client == NULL)
				return NULL;
			x11_event_send(server, client, event);
			return window;
		}

		/* Kept from going on, or at the end of the way. */
		if ((window->do_not_propagate & mask) != 0U)
			return NULL;
		if (window == stop)
			return NULL;
		child = window->id;
		window = window->parent;
	}

	/* Nobody asked. */
	return NULL;
}

/*
 * Returns the child of a window that an inferior is in (the inferior
 * itself when it is a child), or None (the inferior is the window, or not
 * in it).
 */
uint32_t
x11_event_child_toward(
	const struct x11_window *window,
	const struct x11_window *inferior)
{
	/* Up from the inferior to the window's child. */
	while (inferior != NULL && inferior->parent != window)
		inferior = inferior->parent;

	/* Not an inferior of the window. */
	if (inferior == NULL)
		return 0U;

	/* Succeeded: the child. */
	return inferior->id;
}

/* Returns a client's entry on a window, or NULL. */
static struct x11_event_select *
event_select_find(
	const struct x11_window *window,
	unsigned client)
{
	unsigned index;

	/* The entry of the client. */
	for (index = 0U; index < window->select_count; index++) {
		if (window->selects[index].client == client)
			return &window->selects[index];
	}

	/* None. */
	return NULL;
}

/* Reverses the two bytes of a 16-bit field. */
static void
event_swap16(
	uint8_t *bytes)
{
	uint8_t first;

	/* The two ends change places. */
	first = bytes[0];
	bytes[0] = bytes[1];
	bytes[1] = first;
}

/* Reverses the four bytes of a 32-bit field. */
static void
event_swap32(
	uint8_t *bytes)
{
	uint8_t byte;

	/* The outer pair, then the inner pair. */
	byte = bytes[0];
	bytes[0] = bytes[3];
	bytes[3] = byte;
	byte = bytes[1];
	bytes[1] = bytes[2];
	bytes[2] = byte;
}
