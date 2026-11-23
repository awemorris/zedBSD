/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selections, and the bridge between X's CLIPBOARD and the clipboard of
 * the system the backend shows the server on.
 *
 * Selections: a selection's owner is a client's window, told
 * SelectionClear when another takes it; a ConvertSelection is passed to
 * the owner as SelectionRequest, and the owner answers the requestor
 * through SendEvent (SelectionNotify), as the ICCCM has it.
 *
 * The bridge: when another program's text becomes the system's
 * selection, the server owns CLIPBOARD for it (on the root window) and
 * answers a ConvertSelection itself, with the text the backend reads (and
 * PRIMARY, when no X client owns it).  While an X client owns CLIPBOARD
 * the backend offers its text to the system; a program asking for it
 * becomes a request of the backend's, and the server asks the owner for
 * UTF8_STRING into a property of the root window, then gives what the
 * owner put there to the backend to answer the request with.
 *
 * PRIMARY is bridged the same way with the system's primary selection;
 * without an X owner and without the system's primary text, PRIMARY still
 * answers with the clipboard's text.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The events of selections. */
#define EVENT_SELECTION_CLEAR	29U
#define EVENT_SELECTION_REQUEST	30U
#define EVENT_SELECTION_NOTIFY	31U

/* ChangeProperty's replace mode. */
#define PROPERTY_REPLACE	0U

/* The longest selection name a log line shows. */
#define SELECTION_NAME_LOG	64

static struct x11_selection *selection_find(struct x11server *server, uint32_t atom, int create);
static void selection_event(struct x11server *server, unsigned owner, const uint8_t *event);
static void selection_notify(struct x11server *server, unsigned owner, uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property);
static void selection_clear(struct x11server *server, struct x11_selection *entry);
static uint32_t selection_bridge_convert(struct x11server *server, uint32_t requestor, uint32_t target, uint32_t property, int primary);
static void selection_desktop_owns(struct x11server *server, uint32_t atom, const char *name, int text);
static void selection_bridge_ask(struct x11server *server, uint32_t atom, uint32_t request);
static void selection_backend_own(struct x11server *server, uint32_t atom, int own);
static void selection_log_owner(struct x11server *server, uint32_t atom, uint32_t window, unsigned client);

/*
 * Makes the atoms the server itself uses (CLIPBOARD, UTF8_STRING,
 * TARGETS, TEXT and its bridge's property) before any client asks.
 */
void
x11_selection_init(
	struct x11server *server)
{
	/* Each name in turn gets its number. */
	server->atom_clipboard = x11_atom_intern(server, "CLIPBOARD", 9U, 0);
	server->atom_utf8 = x11_atom_intern(server, "UTF8_STRING", 11U, 0);
	server->atom_targets = x11_atom_intern(server, "TARGETS", 7U, 0);
	server->atom_text = x11_atom_intern(server, "TEXT", 4U, 0);
	server->atom_bridge = x11_atom_intern(server, "ZED_SELECTION", 13U, 0);
}

/*
 * SetSelectionOwner: a window (or None) owns a selection; the owner
 * before, when another, is told SelectionClear.  CLIPBOARD owned by an X
 * client is offered to the system; given up, it is taken back.
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
	uint32_t window;
	uint32_t atom;
	int valid;

	/* The owner window (None, or a window) and the selection. */
	client = &server->clients[index];
	if (length != 16U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, client->order);
	atom = x11_read32(request + 8, client->order);
	found = NULL;
	if (window != 0U)
		found = x11_window_find(server, window);
	if (window != 0U && found == NULL)
		return X11_BAD_WINDOW;
	valid = x11_atom_valid(server, atom);
	if (!valid)
		return X11_BAD_ATOM;
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
	selection_log_owner(server, atom, window, index);

	/* CLIPBOARD or PRIMARY owned by an X client is the system's selection; given up, the system's is emptied. */
	if (window != 0U) {
		selection_backend_own(server, atom, 1);
	} else {
		selection_backend_own(server, atom, 0);
	}

	/* Succeeded: owned. */
	return 0U;
}

/* GetSelectionOwner: the owner window (the root window while the system's text is CLIPBOARD's), or None. */
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
	uint32_t atom;
	int valid;

	/* The selection's owner. */
	client = &server->clients[index];
	if (length != 8U)
		return X11_BAD_LENGTH;
	atom = x11_read32(request + 4, client->order);
	valid = x11_atom_valid(server, atom);
	if (!valid)
		return X11_BAD_ATOM;
	entry = selection_find(server, atom, 0);
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
 * answers itself for the system's text (CLIPBOARD owned for it, or
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
	int valid;

	/* The requestor, the selection, the target and the property (None: the target's name). */
	client = &server->clients[index];
	if (length != 24U)
		return X11_BAD_LENGTH;
	requestor = x11_read32(request + 4, client->order);
	selection = x11_read32(request + 8, client->order);
	target = x11_read32(request + 12, client->order);
	property = x11_read32(request + 16, client->order);
	found = x11_window_find(server, requestor);
	if (found == NULL)
		return X11_BAD_WINDOW;
	valid = x11_atom_valid(server, selection);
	if (!valid)
		return X11_BAD_ATOM;
	valid = x11_atom_valid(server, target);
	if (!valid)
		return X11_BAD_ATOM;
	if (property != 0U) {
		valid = x11_atom_valid(server, property);
		if (!valid)
			return X11_BAD_ATOM;
	}
	if (property == 0U)
		property = target;

	/*
	 * The system's text answers: a selection owned for it, or a PRIMARY
	 * or CLIPBOARD without an X owner while there is some (PRIMARY's own,
	 * else the clipboard's).
	 */
	entry = selection_find(server, selection, 0);
	bridge = 0;
	if (entry != NULL && entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT)
		bridge = 1;
	primary = 0;
	if (selection == X11_ATOM_PRIMARY)
		primary = server->ops->clipboard_has_text(server->backend, X11_SELECTION_PRIMARY);
	text = server->ops->clipboard_has_text(server->backend, X11_SELECTION_CLIPBOARD);
	if ((entry == NULL || entry->window == 0U) && (text || primary) &&
	    (selection == X11_ATOM_PRIMARY || selection == server->atom_clipboard))
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
 * Ends the oldest request of the bridge: the X owner (the client of the
 * slot) answered with a SelectionNotify to the root window (the event,
 * least significant byte first), whose property holds its text (None:
 * none); the backend answers the system's request with it.
 */
void
x11_selection_bridge_done(
	struct x11server *server,
	unsigned slot,
	const uint8_t *event)
{
	struct x11_property *kept;
	uint32_t property;
	uint32_t request;
	size_t done;

	/* The property the answer names (the sender is not checked yet). */
	(void)slot;
	property = x11_read32(event + 20, 0);

	/* The oldest waiting request. */
	if (server->pending_count == 0U)
		return;
	request = server->pending_sends[0];
	server->pending_count--;
	memmove(&server->pending_sends[0], &server->pending_sends[1], server->pending_count * sizeof(server->pending_sends[0]));

	/* The owner's text, when it gave some. */
	kept = NULL;
	if (property != 0U)
		kept = x11_property_find(server->root, property);

	/* The request is answered with all of it, or with none. */
	if (kept != NULL) {
		done = server->ops->clipboard_answer(server->backend, request, kept->data, kept->length);
	} else {
		done = server->ops->clipboard_answer(server->backend, request, NULL, 0U);
	}

	/* The property goes. */
	if (kept != NULL)
		x11_property_delete(server, server->root, property);
	x11_log(server, "X11 SELECTION sent bytes=%lu", (unsigned long)done);
}

/*
 * Forgets the selections a window owned: such a selection has no owner
 * any more (CLIPBOARD's and PRIMARY's are taken back from the system).
 */
void
x11_selection_forget_window(
	struct x11server *server,
	uint32_t window)
{
	unsigned index;

	/* The selections it owned. */
	for (index = 0; index < server->selection_count; index++) {
		/* Only this window's. */
		if (server->selections[index].window != window)
			continue;

		/* No owner now; the system's clipboard or primary selection is taken back. */
		server->selections[index].window = 0U;
		selection_backend_own(server, server->selections[index].atom, 0);
	}
}

/*
 * Forgets the selections a closed client owned (through a window of
 * another client): such a selection has no owner any more.
 */
void
x11_selection_forget_client(
	struct x11server *server,
	unsigned client)
{
	unsigned index;

	/* The selections it owned. */
	for (index = 0; index < server->selection_count; index++) {
		/* Only this client's. */
		if (server->selections[index].window == 0U || server->selections[index].owner != client)
			continue;

		/* No owner now; the system's clipboard or primary selection is taken back. */
		server->selections[index].window = 0U;
		selection_backend_own(server, server->selections[index].atom, 0);
	}
}

/*
 * Hears that a window's property changed or was deleted.  The selections
 * wait for no property yet.
 */
void
x11_selection_property_event(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom,
	int deleted)
{
	/* Nothing waits for it. */
	(void)server;
	(void)window;
	(void)atom;
	(void)deleted;
}

/*
 * Hears that the system's selection changed (the backend): another
 * program's text makes the server the owner of CLIPBOARD or PRIMARY for
 * it (the X owner before is told SelectionClear); none, while the server
 * owned it, leaves it without an owner.
 */
void
x11server_backend_clipboard(
	struct x11server *server,
	unsigned selection,
	int text)
{
	/* PRIMARY's owner. */
	if (selection == X11_SELECTION_PRIMARY) {
		selection_desktop_owns(server, X11_ATOM_PRIMARY, "PRIMARY", text);
		return;
	}

	/* CLIPBOARD's owner. */
	selection_desktop_owns(server, server->atom_clipboard, "CLIPBOARD", text);
}

/*
 * Takes a request of the system for the text of an X owner of CLIPBOARD
 * or PRIMARY (the backend's number for it): the request waits, and the
 * owner is asked for UTF8_STRING in the root window's bridge property.
 * Without an owner, or with too many waiting, the request is answered at
 * once with no text.
 */
void
x11server_backend_clipboard_request(
	struct x11server *server,
	unsigned selection,
	uint32_t request)
{
	/* PRIMARY's owner is asked. */
	if (selection == X11_SELECTION_PRIMARY) {
		selection_bridge_ask(server, X11_ATOM_PRIMARY, request);
		return;
	}

	/* CLIPBOARD's owner is asked. */
	selection_bridge_ask(server, server->atom_clipboard, request);
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
 * Answers a conversion of the system's text: TARGETS lists the text
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
	struct x11_window *window;
	uint8_t targets[16];
	uint32_t type;
	size_t length;
	uint8_t *text;
	unsigned code;
	int error;

	/* The requestor's window. */
	window = x11_window_find(server, requestor);
	if (window == NULL)
		return 0U;

	/* TARGETS: the types given, as atoms (least significant byte first, as properties are kept). */
	if (target == server->atom_targets) {
		x11_write32(targets, server->atom_targets, 0);
		x11_write32(targets + 4, server->atom_utf8, 0);
		x11_write32(targets + 8, X11_ATOM_STRING, 0);
		x11_write32(targets + 12, server->atom_text, 0);
		code = x11_property_set(server, window, property, X11_ATOM_ATOM, 32U, PROPERTY_REPLACE, targets, sizeof(targets), X11_NO_CLIENT);
		if (code != 0U)
			return 0U;
		return property;
	}

	/* Only the text types. */
	if (target != server->atom_utf8 && target != X11_ATOM_STRING && target != server->atom_text)
		return 0U;

	/* The system's text: its primary selection's for PRIMARY when it has one, else its clipboard's. */
	if (primary) {
		error = server->ops->clipboard_read(server->backend, X11_SELECTION_PRIMARY, &text, &length);
	} else {
		error = server->ops->clipboard_read(server->backend, X11_SELECTION_CLIPBOARD, &text, &length);
	}

	/* Nothing read, nothing given. */
	if (error != 0)
		return 0U;

	/* In the requestor's property, of the type asked (TEXT is given as UTF8_STRING). */
	type = server->atom_utf8;
	if (target == X11_ATOM_STRING)
		type = X11_ATOM_STRING;
	code = x11_property_set(server, window, property, type, 8U, PROPERTY_REPLACE, text, length, X11_NO_CLIENT);
	free(text);
	if (code != 0U)
		return 0U;

	/* Succeeded: the property holds it. */
	x11_log(server, "X11 SELECTION convert requestor=0x%x bytes=%lu", requestor, (unsigned long)length);
	return property;
}

/*
 * The system's text became (or stopped being) a selection's: with text
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
		x11_log(server, "X11 SELECTION owner selection=%s window=root client=desktop", name);
		return;
	}

	/* No text: the server's ownership ends. */
	if (entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT)
		entry->window = 0U;
}

/*
 * Asks a selection's X owner for its text for a request of the system:
 * the request waits, and the owner is asked for UTF8_STRING in the root
 * window's bridge property.  Without an owner, or with too many waiting,
 * the request is answered at once with no text.
 */
static void
selection_bridge_ask(
	struct x11server *server,
	uint32_t atom,
	uint32_t request)
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
		(void)server->ops->clipboard_answer(server->backend, request, NULL, 0U);
		return;
	}

	/* The request waits for the answer. */
	server->pending_sends[server->pending_count] = request;
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

/* Makes an X owner's text the system's selection of CLIPBOARD or PRIMARY, or gives that up; other atoms are not bridged. */
static void
selection_backend_own(
	struct x11server *server,
	uint32_t atom,
	int own)
{
	/* CLIPBOARD. */
	if (atom == server->atom_clipboard) {
		(void)server->ops->clipboard_own(server->backend, X11_SELECTION_CLIPBOARD, own);
		return;
	}

	/* PRIMARY. */
	if (atom == X11_ATOM_PRIMARY)
		(void)server->ops->clipboard_own(server->backend, X11_SELECTION_PRIMARY, own);
}

/* Logs a selection's new owner (its name, cut for the line). */
static void
selection_log_owner(
	struct x11server *server,
	uint32_t atom,
	uint32_t window,
	unsigned client)
{
	const char *name;
	size_t length;

	/* The selection's name. */
	name = x11_atom_name(server, atom, &length);
	if (name == NULL) {
		name = "?";
		length = 1U;
	}
	if (length > SELECTION_NAME_LOG)
		length = SELECTION_NAME_LOG;

	/* Said. */
	x11_log(server, "X11 SELECTION owner selection=%.*s window=0x%x client=%u", (int)length, name, window, client);
}
