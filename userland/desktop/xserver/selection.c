/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Atoms, window properties and selections, and the bridge between X's
 * CLIPBOARD and the desktop's clipboard (ws035-p087).
 *
 * Atoms: the core predefined atoms the clients use keep their numbers;
 * any other name is given the next number.  Properties: any property a
 * client sets on a window is kept (WM_NAME and the icon path are also kept
 * as the window's strings, protocol.c).  Selections: a selection's owner is
 * a client's window, told SelectionClear when another takes it; a
 * ConvertSelection is passed to the owner as SelectionRequest, and the
 * owner answers the requestor through SendEvent (SelectionNotify), as the
 * ICCCM has it.
 *
 * The bridge: when another desktop client's text becomes the desktop's
 * selection, the server owns CLIPBOARD for it (on the root window) and
 * answers a ConvertSelection itself, with the text read from the desktop
 * (and PRIMARY, when no X client owns it).  While an X client owns
 * CLIPBOARD the server offers its text to the desktop; a desktop client
 * asking for it hands over a descriptor, and the server asks the owner
 * for UTF8_STRING into a property of the root window, then writes what the
 * owner put there into the descriptor.
 *
 * PRIMARY is bridged the same way with the desktop's primary selection
 * (ws035-p103); without an X owner and without the desktop's primary text,
 * PRIMARY still answers with the clipboard's text, as before.
 */

#include "userland/desktop/xserver/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The core predefined atoms kept by name. */
#define ATOM_PRIMARY		1U
#define ATOM_ATOM		4U
#define ATOM_STRING		31U

/* The events of selections. */
#define EVENT_SELECTION_CLEAR	29U
#define EVENT_SELECTION_REQUEST	30U
#define EVENT_SELECTION_NOTIFY	31U

/* The bit a SendEvent's event carries. */
#define EVENT_SENT		0x80U

/* ChangeProperty's modes, and GetProperty's "any type". */
#define PROPERTY_REPLACE	0U
#define PROPERTY_PREPEND	1U
#define PROPERTY_APPEND		2U
#define PROPERTY_ANY_TYPE	0U

/* The errors of these requests. */
#define BAD_VALUE		2U
#define BAD_ATOM		5U

/* The largest property kept, in bytes. */
#define PROPERTY_MAX		(1024U * 1024U)

/*
 * One predefined atom kept by name.
 */
struct selection_atom {
	uint32_t atom;
	const char *name;
};

/* The predefined atoms known by name (the others keep their numbers but have no name here). */
static const struct selection_atom selection_predefined[] = {
	{ 1U, "PRIMARY" },
	{ 2U, "SECONDARY" },
	{ 4U, "ATOM" },
	{ 6U, "CARDINAL" },
	{ 19U, "INTEGER" },
	{ 31U, "STRING" },
	{ 33U, "WINDOW" },
	{ 39U, "WM_NAME" },
	{ 67U, "WM_CLASS" }
};

static struct x11_property *selection_property(struct x11server *server, uint32_t window, uint32_t atom);
static int selection_property_set(struct x11server *server, uint32_t window, uint32_t atom, uint32_t type, uint8_t format, unsigned mode, const uint8_t *data, size_t length);
static void selection_property_delete(struct x11server *server, struct x11_property *property);
static struct x11_selection *selection_find(struct x11server *server, uint32_t atom, int create);
static void selection_event(struct x11server *server, unsigned owner, const uint8_t *event);
static void selection_notify(struct x11server *server, unsigned owner, uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property);
static void selection_clear(struct x11server *server, struct x11_selection *entry);
static uint32_t selection_bridge_convert(struct x11server *server, uint32_t requestor, uint32_t target, uint32_t property, int primary);
static void selection_desktop_owns(struct x11server *server, uint32_t atom, const char *name, int text);
static void selection_bridge_ask(struct x11server *server, uint32_t atom, int fd);
static void selection_bridge_done(struct x11server *server, uint32_t property);
static const char *selection_name(struct x11server *server, uint32_t atom);

/*
 * Makes the atoms the server itself uses (CLIPBOARD, UTF8_STRING,
 * TARGETS, TEXT and its bridge's property) before any client asks.
 */
void
x11_selection_init(
	struct x11server *server)
{
	/* Each name in turn gets its number. */
	server->atom_clipboard = x11_atom_intern(server, "CLIPBOARD", 0);
	server->atom_utf8 = x11_atom_intern(server, "UTF8_STRING", 0);
	server->atom_targets = x11_atom_intern(server, "TARGETS", 0);
	server->atom_text = x11_atom_intern(server, "TEXT", 0);
	server->atom_bridge = x11_atom_intern(server, "ZED_SELECTION", 0);
}

/*
 * Finds a name's atom; a new name gets the next number unless only an
 * existing one is asked for.  Returns the atom, or 0 (None) for an unknown
 * name asked only if it exists, or when the table is full.
 */
uint32_t
x11_atom_intern(
	struct x11server *server,
	const char *name,
	int only_if_exists)
{
	unsigned index;
	size_t length;
	int same;

	/* A predefined name keeps its number. */
	for (index = 0; index < sizeof(selection_predefined) / sizeof(selection_predefined[0]); index++) {
		same = strcmp(name, selection_predefined[index].name);
		if (same == 0)
			return selection_predefined[index].atom;
	}

	/* A name given a number before. */
	for (index = 0; index < server->atom_count; index++) {
		same = strcmp(name, server->atom_names[index]);
		if (same == 0)
			return X11_ATOM_FIRST_DYNAMIC + index;
	}

	/* An unknown one asked only if it exists, a full table, or too long a name: None. */
	length = strlen(name);
	if (only_if_exists || server->atom_count == X11_MAX_ATOMS || length >= X11_ATOM_NAME)
		return 0U;

	/* Succeeded: the next number. */
	(void)snprintf(server->atom_names[server->atom_count], X11_ATOM_NAME, "%s", name);
	server->atom_count++;
	return X11_ATOM_FIRST_DYNAMIC + server->atom_count - 1U;
}

/* InternAtom: a name's atom (only if it exists, when asked). */
unsigned
x11_request_intern_atom(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32];
	char name[X11_ATOM_NAME];
	size_t count;
	uint32_t atom;

	/* The name's length and bytes. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	count = x11_read16(request + 4, client->order);
	if (count > length - 8U)
		return X11_BAD_LENGTH;

	/* A name too long for the table has no atom. */
	atom = 0U;
	if (count < sizeof(name)) {
		memcpy(name, request + 8, count);
		name[count] = '\0';
		atom = x11_atom_intern(server, name, request[1] != 0U);
	}

	/* The reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, atom, client->order);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/* GetAtomName: an atom's name (BadAtom for one without a name here). */
unsigned
x11_request_get_atom_name(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint8_t reply[32 + X11_ATOM_NAME + 4];
	const char *name;
	size_t count;
	size_t padded;

	/* The atom's name. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	name = selection_name(server, x11_read32(request + 4, client->order));
	if (name == NULL)
		return BAD_ATOM;

	/* The reply with the name, padded. */
	memset(reply, 0, sizeof(reply));
	count = strlen(name);
	padded = (count + 3U) & ~(size_t)3U;
	x11_write16(reply + 8, (uint16_t)count, client->order);
	memcpy(reply + 32, name, count);
	x11_reply(server, client, reply, 32U + padded);

	/* Succeeded: answered. */
	return 0U;
}

/*
 * ChangeProperty of any other property than WM_NAME and the icon path
 * (protocol.c keeps those): kept with the window, replaced, prepended to
 * or appended to.
 */
unsigned
x11_request_change_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *found;
	struct x11_client *client;
	uint32_t window;
	uint32_t count;
	size_t bytes;
	uint8_t format;
	int error;

	/* The window, the property's type and format, and its data's length. */
	client = &server->clients[index];
	if (length < 24U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, client->order);
	found = x11_window_find(server, window);
	if (found == NULL)
		return X11_BAD_WINDOW;
	format = request[16];
	count = x11_read32(request + 20, client->order);

	/* Only 8, 16 or 32 bits a unit, and data within the request. */
	if (format != 8U && format != 16U && format != 32U)
		return BAD_VALUE;
	bytes = (size_t)count * (size_t)(format / 8U);
	if (bytes > length - 24U)
		return X11_BAD_LENGTH;

	/* Kept (a property too large is refused). */
	error = selection_property_set(server, window, x11_read32(request + 8, client->order), x11_read32(request + 12, client->order), format, request[1], request + 24, bytes);
	if (error != 0)
		return X11_BAD_ALLOC;

	/* Succeeded: changed. */
	return 0U;
}

/*
 * GetProperty of a kept property: its type and format, the part asked
 * (from long-offset, at most long-length units of four bytes) and what is
 * left after it; deleted when asked and all was read.  A property that is
 * not kept answers None.  Returns 0 when it answered, or an error.
 */
unsigned
x11_request_get_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_property *property;
	struct x11_window *found;
	struct x11_client *client;
	uint8_t *reply;
	uint32_t window;
	uint32_t type;
	size_t offset;
	size_t count;
	size_t most;
	size_t after;
	size_t padded;
	int msb;

	/* The window must exist. */
	client = &server->clients[index];
	msb = client->order;
	if (length < 24U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, msb);
	found = x11_window_find(server, window);
	if (found == NULL)
		return X11_BAD_WINDOW;

	/* The property; none is a reply of type None. */
	property = selection_property(server, window, x11_read32(request + 8, msb));
	type = x11_read32(request + 12, msb);
	if (property == NULL) {
		reply = calloc(1U, 32U);
		if (reply == NULL)
			return X11_BAD_ALLOC;
		x11_reply(server, client, reply, 32U);
		free(reply);
		return 0U;
	}

	/* Another type than asked: its type and format and its whole length, no data. */
	offset = (size_t)x11_read32(request + 16, msb) * 4U;
	if (type != PROPERTY_ANY_TYPE && type != property->type) {
		reply = calloc(1U, 32U);
		if (reply == NULL)
			return X11_BAD_ALLOC;
		reply[1] = property->format;
		x11_write32(reply + 8, property->type, msb);
		x11_write32(reply + 12, (uint32_t)property->length, msb);
		x11_reply(server, client, reply, 32U);
		free(reply);
		return 0U;
	}

	/* The part asked for, and what is left after it. */
	if (offset > property->length)
		return BAD_VALUE;
	count = property->length - offset;
	most = (size_t)x11_read32(request + 20, msb) * 4U;
	if (count > most)
		count = most;
	after = property->length - offset - count;
	padded = (count + 3U) & ~(size_t)3U;

	/* The reply: the type, the format, what is after, the units given, and the data. */
	reply = calloc(1U, 32U + padded);
	if (reply == NULL)
		return X11_BAD_ALLOC;
	reply[1] = property->format;
	x11_write32(reply + 8, property->type, msb);
	x11_write32(reply + 12, (uint32_t)after, msb);
	x11_write32(reply + 16, (uint32_t)(count / (size_t)(property->format / 8U)), msb);
	memcpy(reply + 32, property->data + offset, count);
	x11_reply(server, client, reply, 32U + padded);
	free(reply);

	/* Deleted when asked and all was read. */
	if (request[1] != 0U && after == 0U)
		selection_property_delete(server, property);

	/* Succeeded: answered. */
	return 0U;
}

/* DeleteProperty: the property goes (one that is not there is fine). */
unsigned
x11_request_delete_property(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_property *property;
	struct x11_window *found;
	struct x11_client *client;
	uint32_t window;

	/* The window and the property. */
	client = &server->clients[index];
	if (length < 12U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, client->order);
	found = x11_window_find(server, window);
	if (found == NULL)
		return X11_BAD_WINDOW;
	property = selection_property(server, window, x11_read32(request + 8, client->order));

	/* Succeeded: it is gone. */
	if (property != NULL)
		selection_property_delete(server, property);
	return 0U;
}

/*
 * SetSelectionOwner: a window (or None) owns a selection; the owner
 * before, when another, is told SelectionClear.  CLIPBOARD owned by an X
 * client is offered to the desktop; given up, it is taken back.
 */
unsigned
x11_request_set_selection_owner(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_selection *entry;
	struct x11_window *found;
	struct x11_client *client;
	const char *name;
	uint32_t window;
	uint32_t atom;

	/* The owner window (None, or a window) and the selection. */
	client = &server->clients[index];
	if (length < 16U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, client->order);
	atom = x11_read32(request + 8, client->order);
	found = NULL;
	if (window != 0U)
		found = x11_window_find(server, window);
	if (window != 0U && found == NULL)
		return X11_BAD_WINDOW;
	entry = selection_find(server, atom, 1);
	if (entry == NULL)
		return X11_BAD_ALLOC;

	/* The owner before, when another window, is told. */
	if (entry->window != 0U && entry->window != window)
		selection_clear(server, entry);

	/* The new owner. */
	entry->window = window;
	entry->owner = index;
	entry->time = x11_read32(request + 12, client->order);
	name = selection_name(server, atom);
	if (name == NULL)
		name = "?";
	printf("X11 SELECTION owner selection=%s window=0x%x client=%u\n", name, window, index);
	fflush(stdout);

	/* CLIPBOARD owned by an X client is the desktop's selection; given up, the desktop's is emptied. */
	if (atom == server->atom_clipboard && server->wayland != NULL) {
		if (window != 0U) {
			(void)x11_wayland_selection_own(server->wayland);
		} else {
			x11_wayland_selection_drop(server->wayland);
		}
	}

	/* The same for PRIMARY and the desktop's primary selection. */
	if (atom == ATOM_PRIMARY && server->wayland != NULL) {
		if (window != 0U) {
			(void)x11_wayland_primary_own(server->wayland);
		} else {
			x11_wayland_primary_drop(server->wayland);
		}
	}

	/* Succeeded: owned. */
	return 0U;
}

/* GetSelectionOwner: the owner window (the root window while the desktop's text is CLIPBOARD's), or None. */
unsigned
x11_request_get_selection_owner(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_selection *entry;
	struct x11_client *client;
	uint8_t reply[32];
	uint32_t window;

	/* The selection's owner. */
	client = &server->clients[index];
	if (length < 8U)
		return X11_BAD_LENGTH;
	entry = selection_find(server, x11_read32(request + 4, client->order), 0);
	window = 0U;
	if (entry != NULL)
		window = entry->window;

	/* The reply. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, window, client->order);
	x11_reply(server, client, reply, sizeof(reply));

	/* Succeeded: answered. */
	return 0U;
}

/*
 * ConvertSelection: the owner is asked (SelectionRequest) to put the
 * selection, as the target, in the requestor's property; the server
 * answers itself for the desktop's text (CLIPBOARD owned for it, or
 * PRIMARY without an X owner); without an owner the requestor is told
 * None at once.
 */
unsigned
x11_request_convert_selection(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_selection *entry;
	struct x11_window *found;
	struct x11_client *client;
	struct x11_client *owner;
	uint8_t event[32];
	uint32_t requestor;
	uint32_t selection;
	uint32_t target;
	uint32_t property;
	uint32_t answered;
	int primary;
	int bridge;
	int text;

	/* The requestor, the selection, the target and the property (None: the target's name). */
	client = &server->clients[index];
	if (length < 24U)
		return X11_BAD_LENGTH;
	requestor = x11_read32(request + 4, client->order);
	selection = x11_read32(request + 8, client->order);
	target = x11_read32(request + 12, client->order);
	property = x11_read32(request + 16, client->order);
	found = x11_window_find(server, requestor);
	if (found == NULL)
		return X11_BAD_WINDOW;
	if (property == 0U)
		property = target;

	/*
	 * The desktop's text answers: a selection owned for it, or a PRIMARY
	 * or CLIPBOARD without an X owner while there is some (PRIMARY's own,
	 * else the clipboard's).
	 */
	entry = selection_find(server, selection, 0);
	bridge = 0;
	if (entry != NULL && entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT)
		bridge = 1;
	text = 0;
	primary = 0;
	if (server->wayland != NULL && selection == ATOM_PRIMARY)
		primary = x11_wayland_primary_has_text(server->wayland);
	if (server->wayland != NULL)
		text = x11_wayland_selection_has_text(server->wayland);
	if ((entry == NULL || entry->window == 0U) && (text || primary) &&
	    (selection == ATOM_PRIMARY || selection == server->atom_clipboard))
		bridge = 1;
	if (bridge) {
		answered = selection_bridge_convert(server, requestor, target, property, primary);
		selection_notify(server, index, requestor, selection, target, answered);
		return 0U;
	}

	/* No owner: None at once. */
	owner = NULL;
	if (entry != NULL && entry->window != 0U)
		owner = x11_client_of(server, entry->owner);
	if (owner == NULL) {
		selection_notify(server, index, requestor, selection, target, 0U);
		return 0U;
	}

	/* The owner is asked. */
	memset(event, 0, sizeof(event));
	event[0] = EVENT_SELECTION_REQUEST;
	x11_write32(event + 4, x11_read32(request + 20, client->order), owner->order);
	x11_write32(event + 8, entry->window, owner->order);
	x11_write32(event + 12, requestor, owner->order);
	x11_write32(event + 16, selection, owner->order);
	x11_write32(event + 20, target, owner->order);
	x11_write32(event + 24, property, owner->order);
	selection_event(server, entry->owner, event);

	/* Succeeded: the owner answers the requestor. */
	return 0U;
}

/*
 * SendEvent: the event goes to the destination window's owner, marked as
 * sent.  A SelectionNotify to the root window answers the server's own
 * request for the X owner's text (the bridge).
 */
unsigned
x11_request_send_event(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_window *window;
	struct x11_client *client;
	uint32_t destination;
	uint8_t event[32];

	/* The destination and the event. */
	client = &server->clients[index];
	if (length < 44U)
		return X11_BAD_LENGTH;
	destination = x11_read32(request + 4, client->order);
	memcpy(event, request + 12, sizeof(event));

	/* A SelectionNotify to the root window ends a request of the bridge (its property names the answer, None for none). */
	if (destination == X11_ROOT_XID && (event[0] & 0x7fU) == EVENT_SELECTION_NOTIFY) {
		selection_bridge_done(server, x11_read32(event + 20, client->order));
		return 0U;
	}

	/* Another destination must be a window of a client. */
	window = x11_window_find(server, destination);
	if (window == NULL)
		return X11_BAD_WINDOW;

	/* Succeeded: sent to the window's owner (the event was built in the sender's byte order, which is also its owner's here). */
	event[0] |= EVENT_SENT;
	selection_event(server, window->owner, event);
	return 0U;
}

/*
 * Forgets what a window held: its properties, and the selections it owned
 * (a selection owned by it has no owner any more; CLIPBOARD's is taken
 * back from the desktop).
 */
void
x11_selection_forget_window(
	struct x11server *server,
	uint32_t window)
{
	unsigned index;

	/* Its properties. */
	for (index = X11_MAX_PROPERTIES; index > 0U; index--) {
		/* Only this window's. */
		if (server->properties[index - 1U].window == window && server->properties[index - 1U].data != NULL)
			selection_property_delete(server, &server->properties[index - 1U]);
	}

	/* The selections it owned. */
	for (index = 0; index < server->selection_count; index++) {
		/* Only this window's. */
		if (server->selections[index].window != window)
			continue;

		/* No owner now; the desktop's CLIPBOARD or primary selection is taken back. */
		server->selections[index].window = 0U;
		if (server->selections[index].atom == server->atom_clipboard && server->wayland != NULL)
			x11_wayland_selection_drop(server->wayland);
		if (server->selections[index].atom == ATOM_PRIMARY && server->wayland != NULL)
			x11_wayland_primary_drop(server->wayland);
	}
}

/*
 * The desktop's selection changed (wayland.c): another client's text makes
 * the server CLIPBOARD's owner for it (the X owner before is told
 * SelectionClear); none, while the server owned it, leaves it without an
 * owner.
 */
void
x11_selection_wayland(
	void *context,
	int text)
{
	struct x11server *server;

	/* CLIPBOARD's owner. */
	server = context;
	selection_desktop_owns(server, server->atom_clipboard, "CLIPBOARD", text);
}

/* The desktop's primary selection changed (wayland.c, ws035-p103): PRIMARY as CLIPBOARD above. */
void
x11_selection_primary(
	void *context,
	int text)
{
	struct x11server *server;

	/* PRIMARY's owner. */
	server = context;
	selection_desktop_owns(server, ATOM_PRIMARY, "PRIMARY", text);
}

/*
 * A desktop client asks for the X owner's text (wayland.c): its descriptor
 * waits, and the owner is asked for UTF8_STRING in the root window's
 * bridge property.  Without an owner, or with too many waiting, the
 * descriptor is closed at once (the reader sees no text).
 */
void
x11_selection_send(
	void *context,
	int fd)
{
	struct x11server *server;

	/* CLIPBOARD's owner is asked. */
	server = context;
	selection_bridge_ask(server, server->atom_clipboard, fd);
}

/* A desktop client asks for PRIMARY's X text (wayland.c, ws035-p103): as x11_selection_send. */
void
x11_selection_primary_send(
	void *context,
	int fd)
{
	struct x11server *server;

	/* PRIMARY's owner is asked. */
	server = context;
	selection_bridge_ask(server, ATOM_PRIMARY, fd);
}

/*
 * The desktop's text became (or stopped being) a selection's: with text
 * the server owns it on the root window (the X owner before is told
 * SelectionClear); without, the server's ownership ends.
 */
static void
selection_desktop_owns(
	struct x11server *server,
	uint32_t atom,
	const char *name,
	int text)
{
	struct x11_selection *entry;

	/* The selection's entry. */
	entry = selection_find(server, atom, 1);
	if (entry == NULL)
		return;

	/* Text: the server owns it on the root window (the X owner before is told). */
	if (text) {
		if (entry->window != 0U && entry->window != X11_ROOT_XID)
			selection_clear(server, entry);
		entry->window = X11_ROOT_XID;
		entry->owner = X11_NO_CLIENT;
		printf("X11 SELECTION owner selection=%s window=root client=desktop\n", name);
		fflush(stdout);
		return;
	}

	/* No text: the server's ownership ends. */
	if (entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT)
		entry->window = 0U;
}

/*
 * Asks a selection's X owner for its text for a desktop client: the
 * descriptor waits, and the owner is asked for UTF8_STRING in the root
 * window's bridge property.  Without an owner, or with too many waiting,
 * the descriptor is closed at once (the reader sees no text).
 */
static void
selection_bridge_ask(
	struct x11server *server,
	uint32_t atom,
	int fd)
{
	struct x11_selection *entry;
	struct x11_client *owner;
	uint8_t event[32];

	/* The selection's X owner. */
	entry = selection_find(server, atom, 0);
	owner = NULL;
	if (entry != NULL && entry->window != 0U && entry->window != X11_ROOT_XID)
		owner = x11_client_of(server, entry->owner);

	/* Nobody to ask, or too many waiting: no text. */
	if (owner == NULL || server->pending_count == X11_PENDING_SENDS) {
		close(fd);
		return;
	}

	/* The descriptor waits for the answer. */
	server->pending_sends[server->pending_count] = fd;
	server->pending_count++;

	/* Succeeded: the owner is asked, the root window as the requestor. */
	memset(event, 0, sizeof(event));
	event[0] = EVENT_SELECTION_REQUEST;
	x11_write32(event + 8, entry->window, owner->order);
	x11_write32(event + 12, X11_ROOT_XID, owner->order);
	x11_write32(event + 16, atom, owner->order);
	x11_write32(event + 20, server->atom_utf8, owner->order);
	x11_write32(event + 24, server->atom_bridge, owner->order);
	selection_event(server, entry->owner, event);
}

/*
 * Gives the class of a window from its WM_CLASS (two strings, the instance
 * and the class; the instance when there is no class).  Returns 0, or
 * ENOENT when the window has none.
 */
int
x11_window_class(
	struct x11server *server,
	uint32_t window,
	char *name,
	size_t size)
{
	struct x11_property *property;
	const char *data;
	size_t instance;
	size_t used;

	/* An 8-bit WM_CLASS with some text. */
	property = selection_property(server, window, X11_ATOM_WM_CLASS);
	if (property == NULL || property->format != 8U || property->length == 0U || size == 0U)
		return ENOENT;
	data = (const char *)property->data;

	/* The class after the instance's NUL, else the instance. */
	instance = strnlen(data, property->length);
	if (instance + 1U < property->length && data[instance + 1U] != '\0') {
		data += instance + 1U;
		instance = strnlen(data, property->length - (size_t)(data - (const char *)property->data));
	}

	/* Succeeded: as much as fits. */
	used = instance;
	if (used > size - 1U)
		used = size - 1U;
	memcpy(name, data, used);
	name[used] = '\0';
	if (used == 0U)
		return ENOENT;
	return 0;
}

/* Finds a window's kept property; NULL when it has none of the name. */
static struct x11_property *
selection_property(
	struct x11server *server,
	uint32_t window,
	uint32_t atom)
{
	unsigned index;

	/* Each kept property. */
	for (index = 0; index < X11_MAX_PROPERTIES; index++) {
		/* The window's, of the name. */
		if (server->properties[index].data != NULL && server->properties[index].window == window && server->properties[index].atom == atom)
			return &server->properties[index];
	}

	/* None. */
	return NULL;
}

/*
 * Keeps a property's data: replaced (the type and format with it), or
 * added before or after what is there.  Returns 0, or an errno value
 * (ENOMEM, E2BIG, EINVAL for a mode or a type that does not match).
 */
static int
selection_property_set(
	struct x11server *server,
	uint32_t window,
	uint32_t atom,
	uint32_t type,
	uint8_t format,
	unsigned mode,
	const uint8_t *data,
	size_t length)
{
	struct x11_property *property;
	uint8_t *joined;
	unsigned index;
	size_t kept;

	/* The property there, or a free slot for a new one. */
	property = selection_property(server, window, atom);
	if (property == NULL) {
		for (index = 0; index < X11_MAX_PROPERTIES && property == NULL; index++) {
			/* A free slot. */
			if (server->properties[index].data == NULL)
				property = &server->properties[index];
		}

		/* A full table keeps no more; a new property is always replaced. */
		if (property == NULL)
			return ENOMEM;
		mode = PROPERTY_REPLACE;
	}

	/* A known mode; added data must be of the same type and format. */
	if (mode > PROPERTY_APPEND)
		return EINVAL;
	if (mode != PROPERTY_REPLACE && (property->type != type || property->format != format))
		return EINVAL;

	/* The new data: what is added, and what was there on its side. */
	if (mode != PROPERTY_REPLACE && property->length + length > PROPERTY_MAX)
		return E2BIG;
	if (mode == PROPERTY_REPLACE && length > PROPERTY_MAX)
		return E2BIG;
	kept = 0;
	if (mode != PROPERTY_REPLACE)
		kept = property->length;
	joined = malloc(length + kept + 1U);
	if (joined == NULL)
		return ENOMEM;
	if (mode == PROPERTY_PREPEND) {
		memcpy(joined, data, length);
		memcpy(joined + length, property->data, property->length);
		length += property->length;
	} else if (mode == PROPERTY_APPEND) {
		memcpy(joined, property->data, property->length);
		memcpy(joined + property->length, data, length);
		length += property->length;
	} else {
		memcpy(joined, data, length);
	}

	/* Succeeded: the property holds it. */
	free(property->data);
	property->window = window;
	property->atom = atom;
	property->type = type;
	property->format = format;
	property->data = joined;
	property->length = length;
	return 0;
}

/* Lets a property go. */
static void
selection_property_delete(
	struct x11server *server,
	struct x11_property *property)
{
	/* The data, and the slot is free. */
	(void)server;
	free(property->data);
	memset(property, 0, sizeof(*property));
}

/* Finds a selection's entry, made when asked and there is room; NULL otherwise. */
static struct x11_selection *
selection_find(
	struct x11server *server,
	uint32_t atom,
	int create)
{
	unsigned index;

	/* The selection's entry. */
	for (index = 0; index < server->selection_count; index++) {
		/* The same selection. */
		if (server->selections[index].atom == atom)
			return &server->selections[index];
	}

	/* A new one when asked and there is room. */
	if (!create || server->selection_count == X11_MAX_SELECTIONS)
		return NULL;

	/* Succeeded: an entry without an owner. */
	memset(&server->selections[server->selection_count], 0, sizeof(server->selections[0]));
	server->selections[server->selection_count].atom = atom;
	server->selection_count++;
	return &server->selections[server->selection_count - 1U];
}

/* Queues a 32-byte event for a client (its sequence number set). */
static void
selection_event(
	struct x11server *server,
	unsigned owner,
	const uint8_t *event)
{
	struct x11_client *client;
	uint8_t copy[32];

	/* A client that is gone hears nothing. */
	client = x11_client_of(server, owner);
	if (client == NULL)
		return;

	/* The event with the client's last sequence number. */
	memcpy(copy, event, sizeof(copy));
	x11_write16(copy + 2, client->sequence, client->order);

	/* Succeeded: queued. */
	x11_client_send(server, client, copy, sizeof(copy));
}

/* Tells a requestor's client that a conversion is done (property None: it failed). */
static void
selection_notify(
	struct x11server *server,
	unsigned owner,
	uint32_t requestor,
	uint32_t selection,
	uint32_t target,
	uint32_t property)
{
	struct x11_client *client;
	uint8_t event[32];

	/* A client that is gone hears nothing. */
	client = x11_client_of(server, owner);
	if (client == NULL)
		return;

	/* The event. */
	memset(event, 0, sizeof(event));
	event[0] = EVENT_SELECTION_NOTIFY;
	x11_write32(event + 8, requestor, client->order);
	x11_write32(event + 12, selection, client->order);
	x11_write32(event + 16, target, client->order);
	x11_write32(event + 20, property, client->order);

	/* Succeeded: queued. */
	selection_event(server, owner, event);
}

/* Tells a selection's X owner that it is not the owner any more. */
static void
selection_clear(
	struct x11server *server,
	struct x11_selection *entry)
{
	struct x11_client *client;
	uint8_t event[32];

	/* Only an X client's window is told. */
	if (entry->owner == X11_NO_CLIENT)
		return;
	client = x11_client_of(server, entry->owner);
	if (client == NULL)
		return;

	/* The event. */
	memset(event, 0, sizeof(event));
	event[0] = EVENT_SELECTION_CLEAR;
	x11_write32(event + 4, entry->time, client->order);
	x11_write32(event + 8, entry->window, client->order);
	x11_write32(event + 12, entry->atom, client->order);

	/* Succeeded: queued. */
	selection_event(server, entry->owner, event);
}

/*
 * Answers a conversion of the desktop's text: TARGETS lists the text
 * types, a text type puts the text in the requestor's property (as
 * UTF8_STRING, or STRING when asked).  Returns the property, or 0 (None)
 * when there is nothing to give.
 */
static uint32_t
selection_bridge_convert(
	struct x11server *server,
	uint32_t requestor,
	uint32_t target,
	uint32_t property,
	int primary)
{
	uint8_t targets[16];
	uint32_t type;
	size_t length;
	char *text;
	int error;

	/* TARGETS: the types given, as atoms. */
	if (target == server->atom_targets) {
		memcpy(targets, &server->atom_targets, 4U);
		memcpy(targets + 4, &server->atom_utf8, 4U);
		type = ATOM_STRING;
		memcpy(targets + 8, &type, 4U);
		memcpy(targets + 12, &server->atom_text, 4U);
		error = selection_property_set(server, requestor, property, ATOM_ATOM, 32U, PROPERTY_REPLACE, targets, sizeof(targets));
		if (error != 0)
			return 0U;
		return property;
	}

	/* Only the text types. */
	if (target != server->atom_utf8 && target != ATOM_STRING && target != server->atom_text)
		return 0U;

	/* The desktop's text: its primary selection's for PRIMARY when it has one, else its clipboard's. */
	if (primary) {
		error = x11_wayland_primary_read(server->wayland, &text, &length);
	} else {
		error = x11_wayland_selection_read(server->wayland, &text, &length);
	}

	/* Nothing read, nothing given. */
	if (error != 0)
		return 0U;

	/* In the requestor's property, of the type asked (TEXT is given as UTF8_STRING). */
	type = server->atom_utf8;
	if (target == ATOM_STRING)
		type = ATOM_STRING;
	error = selection_property_set(server, requestor, property, type, 8U, PROPERTY_REPLACE, (const uint8_t *)text, length);
	free(text);
	if (error != 0)
		return 0U;

	/* Succeeded: the property holds it. */
	printf("X11 SELECTION convert requestor=0x%x bytes=%lu\n", requestor, (unsigned long)length);
	fflush(stdout);
	return property;
}

/*
 * Ends the oldest request of the bridge: the X owner put its text in the
 * root window's property (or answered None); the text goes to the waiting
 * descriptor, which is closed.
 */
static void
selection_bridge_done(
	struct x11server *server,
	uint32_t property)
{
	struct x11_property *kept;
	size_t done;
	ssize_t wrote;
	int fd;

	/* The oldest waiting descriptor. */
	if (server->pending_count == 0U)
		return;
	fd = server->pending_sends[0];
	server->pending_count--;
	memmove(&server->pending_sends[0], &server->pending_sends[1], server->pending_count * sizeof(server->pending_sends[0]));

	/* The owner's text, when it gave some. */
	kept = NULL;
	if (property != 0U)
		kept = selection_property(server, X11_ROOT_XID, property);

	/* All of it (a short text; a failure leaves the reader what was written). */
	done = 0;
	while (kept != NULL && done < kept->length) {
		wrote = write(fd, kept->data + done, kept->length - done);
		if (wrote <= 0)
			break;
		done += (size_t)wrote;
	}

	/* The reader sees the end, and the property goes. */
	close(fd);
	if (kept != NULL)
		selection_property_delete(server, kept);
	printf("X11 SELECTION sent bytes=%lu\n", (unsigned long)done);
	fflush(stdout);
}

/* Returns an atom's name (the predefined ones kept by name, and those given numbers); NULL for another. */
static const char *
selection_name(
	struct x11server *server,
	uint32_t atom)
{
	unsigned index;

	/* A predefined one. */
	for (index = 0; index < sizeof(selection_predefined) / sizeof(selection_predefined[0]); index++) {
		/* The same number. */
		if (selection_predefined[index].atom == atom)
			return selection_predefined[index].name;
	}

	/* One given a number. */
	if (atom >= X11_ATOM_FIRST_DYNAMIC && atom - X11_ATOM_FIRST_DYNAMIC < server->atom_count)
		return server->atom_names[atom - X11_ATOM_FIRST_DYNAMIC];

	/* None here. */
	return NULL;
}
