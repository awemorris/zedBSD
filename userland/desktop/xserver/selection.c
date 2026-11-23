/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Selections, and the bridge between X's CLIPBOARD and PRIMARY and the
 * clipboard of the system the backend shows the server on.
 *
 * Selections: a selection's owner is a client's window, told
 * SelectionClear when another takes it; a ConvertSelection is passed to
 * the owner as SelectionRequest, and the owner answers the requestor
 * through SendEvent (SelectionNotify), as the ICCCM has it.
 *
 * The bridge copies at once.  When an X client takes CLIPBOARD or
 * PRIMARY, the server asks it for UTF8_STRING (STRING when that is
 * refused) into a property of the root window, as any requestor would,
 * taking a large text in pieces (INCR, ICCCM 2.7.2); the text that comes
 * is kept and offered to the backend, so the system's clipboard has it
 * even after the X client ends.  When the owner goes, the root window
 * owns the selection for the copy.  When another program's text becomes
 * the system's selection, the root window owns the selection for it and
 * a ConvertSelection reads the text from the backend.  Either way the
 * server answers the requestor itself, giving a large text in pieces
 * (INCR) as the requestor deletes each.
 *
 * Without an owner of PRIMARY, PRIMARY answers as CLIPBOARD does while
 * the root window owns that.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The events of selections. */
#define EVENT_SELECTION_CLEAR	29U
#define EVENT_SELECTION_REQUEST	30U
#define EVENT_SELECTION_NOTIFY	31U

/* ChangeProperty's replace and append modes. */
#define PROPERTY_REPLACE	0U
#define PROPERTY_APPEND		2U

/* How long an owner answering INCR may take to listen to the root window's properties before its pieces start anyway (ms). */
#define SELECTION_INCR_WAIT	1000U

/* The longest selection name a log line shows. */
#define SELECTION_NAME_LOG	64

/*
 * The largest text given to a requestor in one property, and the size of
 * each piece of a larger one (INCR).  A requestor that cannot take INCR
 * (the tree's zterm reads up to 1 MiB at once) still gets any text up to
 * the first.
 */
#define SELECTION_WHOLE_MAX	(1024U * 1024U)
#define SELECTION_CHUNK		(256U * 1024U)

static struct x11_selection *selection_find(struct x11server *server, uint32_t atom, int create);
static void selection_event(struct x11server *server, unsigned owner, const uint8_t *event);
static void selection_notify(struct x11server *server, unsigned owner, uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property);
static void selection_clear(struct x11server *server, struct x11_selection *entry);
static struct x11_bridge *selection_bridge_of(struct x11server *server, uint32_t atom);
static void selection_bridge_ask(struct x11server *server, struct x11_bridge *bridge, const struct x11_selection *entry, uint32_t target);
static void selection_bridge_stop(struct x11_bridge *bridge);
static void selection_bridge_take(struct x11server *server, struct x11_bridge *bridge, const struct x11_property *property);
static void selection_bridge_piece(struct x11server *server, struct x11_bridge *bridge);
static void selection_bridge_offer(struct x11server *server, struct x11_bridge *bridge, uint8_t *text, size_t length);
static void selection_bridge_release(struct x11server *server, struct x11_selection *entry, int had_owner);
static uint32_t selection_bridge_convert(struct x11server *server, struct x11_bridge *bridge, unsigned index, uint32_t requestor, uint32_t target, uint32_t property);
static uint8_t *selection_bridge_text(struct x11server *server, struct x11_bridge *bridge, size_t *length);
static uint32_t selection_transfer_start(struct x11server *server, unsigned index, struct x11_window *window, uint32_t property, uint32_t type, uint8_t *text, size_t length);
static void selection_transfer_next(struct x11server *server, struct x11_transfer *transfer, struct x11_window *window);
static void selection_transfer_end(struct x11_transfer *transfer);
static uint8_t *selection_latin1(const uint8_t *text, size_t length, size_t *converted);
static void selection_log_owner(struct x11server *server, uint32_t atom, uint32_t window, unsigned client);
static const char *selection_bridge_name(const struct x11server *server, const struct x11_bridge *bridge);

/*
 * Makes the atoms the server itself uses (CLIPBOARD, UTF8_STRING,
 * TARGETS, TEXT, INCR and the properties of its bridges) before any
 * client asks, and the bridges of CLIPBOARD and PRIMARY.
 */
void
x11_selection_init(
	struct x11server *server)
{
	struct x11_bridge *clipboard;
	struct x11_bridge *primary;

	/* Each name in turn gets its number. */
	server->atom_clipboard = x11_atom_intern(server, "CLIPBOARD", 9U, 0);
	server->atom_utf8 = x11_atom_intern(server, "UTF8_STRING", 11U, 0);
	server->atom_targets = x11_atom_intern(server, "TARGETS", 7U, 0);
	server->atom_text = x11_atom_intern(server, "TEXT", 4U, 0);
	server->atom_incr = x11_atom_intern(server, "INCR", 4U, 0);

	/* The bridges, each asking into a property of the root window of its own. */
	memset(server->bridges, 0, sizeof(server->bridges));
	clipboard = &server->bridges[X11_SELECTION_CLIPBOARD];
	clipboard->atom = server->atom_clipboard;
	clipboard->property = x11_atom_intern(server, "ZED_SELECTION", 13U, 0);
	primary = &server->bridges[X11_SELECTION_PRIMARY];
	primary->atom = X11_ATOM_PRIMARY;
	primary->property = x11_atom_intern(server, "ZED_PRIMARY", 11U, 0);

	/* No transfer yet. */
	memset(server->transfers, 0, sizeof(server->transfers));
}

/*
 * SetSelectionOwner: a window (or None) owns a selection from the time
 * given (CurrentTime: the server's); a time before the selection's last
 * change changes nothing.  The owner before, when another window, is told
 * SelectionClear.  CLIPBOARD or PRIMARY taken by an X client is copied
 * at once; given up, the copy stays the root window's.
 */
unsigned
x11_request_set_selection_owner(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_selection *entry;
	struct x11_bridge *bridge;
	struct x11_window *found;
	struct x11_client *client;
	uint32_t window;
	uint32_t atom;
	uint32_t time;
	int had_owner;
	int valid;

	/* The owner window (None, or a window), the selection and the time. */
	client = &server->clients[index];
	if (length != 16U)
		return X11_BAD_LENGTH;
	window = x11_read32(request + 4, client->order);
	atom = x11_read32(request + 8, client->order);
	time = x11_read32(request + 12, client->order);
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
	if (time == 0U)
		time = server->now;

	/*
	 * A time before the last change is ignored (the protocol's rule).  A
	 * time after the server's is not refused: the backend stamps input
	 * with its own reading of the same clock, which may run ahead of the
	 * last tick, and a copy stamped with its button's time must work.
	 */
	if (entry->changed && (int32_t)(time - entry->time) < 0)
		return 0U;

	/* The owner before, when another window, is told. */
	if (entry->window != 0U && entry->window != window)
		selection_clear(server, entry);

	/* The new owner. */
	had_owner = 0;
	if (entry->window != 0U && entry->owner != X11_NO_CLIENT)
		had_owner = 1;
	entry->window = window;
	entry->owner = index;
	entry->time = time;
	entry->changed = 1;
	selection_log_owner(server, atom, window, index);

	/* Only CLIPBOARD and PRIMARY are bridged. */
	bridge = selection_bridge_of(server, atom);
	if (bridge == NULL)
		return 0U;

	/* An X owner's text is asked for at once. */
	if (window != 0U) {
		selection_bridge_ask(server, bridge, entry, server->atom_utf8);
		return 0U;
	}

	/* Succeeded: given up, the copy of an X owner's text stays the root window's. */
	selection_bridge_release(server, entry, had_owner);
	return 0U;
}

/* GetSelectionOwner: the owner window (the root window while the server answers for it), or None. */
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
 * answers itself for a selection the root window owns (and for PRIMARY
 * without an owner while the root window owns CLIPBOARD); without an
 * owner the requestor is told None at once.
 */
unsigned
x11_request_convert_selection(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_selection *clipboard;
	struct x11_selection *entry;
	struct x11_bridge *bridge;
	struct x11_window *found;
	struct x11_client *client;
	struct x11_client *owner;
	uint8_t event[32];
	uint32_t requestor;
	uint32_t selection;
	uint32_t target;
	uint32_t property;
	uint32_t answered;
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

	/* The server answers for a selection the root window owns. */
	entry = selection_find(server, selection, 0);
	bridge = NULL;
	if (entry != NULL && entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT) {
		bridge = selection_bridge_of(server, selection);
	} else if (selection == X11_ATOM_PRIMARY && (entry == NULL || entry->window == 0U)) {
		/* PRIMARY without an owner answers as CLIPBOARD does while the root window owns that. */
		clipboard = selection_find(server, server->atom_clipboard, 0);
		if (clipboard != NULL && clipboard->window == X11_ROOT_XID && clipboard->owner == X11_NO_CLIENT)
			bridge = &server->bridges[X11_SELECTION_CLIPBOARD];
	}
	if (bridge != NULL) {
		answered = selection_bridge_convert(server, bridge, index, requestor, target, property);
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
 * Takes an X owner's answer to the bridge (a SelectionNotify the client
 * of the slot sent to the root window; the event least significant byte
 * first): its property holds the text, or the start of a text in pieces
 * (INCR); None asks again for STRING after UTF8_STRING, else ends.  An
 * answer the bridge does not wait for, or from another client, is
 * dropped.
 */
void
x11_selection_bridge_done(
	struct x11server *server,
	unsigned slot,
	const uint8_t *event)
{
	struct x11_selection *entry;
	struct x11_property *kept;
	struct x11_bridge *bridge;
	uint32_t selection;
	uint32_t target;
	uint32_t property;

	/* The selection, the target and the property the answer names. */
	selection = x11_read32(event + 12, 0);
	target = x11_read32(event + 16, 0);
	property = x11_read32(event + 20, 0);

	/* Only the answer the bridge waits for, from the owner it asked. */
	bridge = selection_bridge_of(server, selection);
	if (bridge == NULL || bridge->asking == 0U || bridge->incr)
		return;
	if (slot != bridge->owner || target != bridge->asking)
		return;

	/* None: STRING is asked for after UTF8_STRING, while the same window still owns it. */
	if (property == 0U) {
		entry = selection_find(server, selection, 0);
		if (bridge->asking == server->atom_utf8 && entry != NULL && entry->window == bridge->window) {
			selection_bridge_ask(server, bridge, entry, X11_ATOM_STRING);
			return;
		}
		x11_log(server, "X11 SELECTION refused selection=%s", selection_bridge_name(server, bridge));
		selection_bridge_stop(bridge);
		return;
	}

	/* The text must be in the property the bridge named. */
	kept = NULL;
	if (property == bridge->property)
		kept = x11_property_find(server->root, property);
	if (kept == NULL) {
		selection_bridge_stop(bridge);
		return;
	}

	/* Succeeded: taken whole, or the pieces begin. */
	selection_bridge_take(server, bridge, kept);
}

/*
 * Forgets the selections a window owned: such a selection has no owner
 * any more (for CLIPBOARD and PRIMARY, the root window owns the copy of
 * its text), and what a bridge or a transfer waited for from it.
 */
void
x11_selection_forget_window(
	struct x11server *server,
	uint32_t window)
{
	struct x11_bridge *bridge;
	unsigned index;

	/* The selections it owned. */
	for (index = 0; index < server->selection_count; index++) {
		/* Only this window's. */
		if (server->selections[index].window != window)
			continue;

		/* No owner now; the copy stays the root window's. */
		selection_bridge_release(server, &server->selections[index], 1);
	}

	/* A bridge asking it, which waits no more. */
	for (index = 0U; index < X11_BRIDGES; index++) {
		bridge = &server->bridges[index];
		if (bridge->asking != 0U && bridge->window == window)
			selection_bridge_stop(bridge);
	}

	/* The transfers to it. */
	for (index = 0U; index < X11_TRANSFERS; index++) {
		if (server->transfers[index].data != NULL && server->transfers[index].window == window)
			selection_transfer_end(&server->transfers[index]);
	}
}

/*
 * Forgets the selections a closed client owned (through a window of
 * another client): such a selection has no owner any more, and the
 * transfers to the client end.
 */
void
x11_selection_forget_client(
	struct x11server *server,
	unsigned client)
{
	struct x11_bridge *bridge;
	unsigned index;

	/* The selections it owned. */
	for (index = 0; index < server->selection_count; index++) {
		/* Only this client's. */
		if (server->selections[index].window == 0U || server->selections[index].owner != client)
			continue;

		/* No owner now; the copy stays the root window's. */
		selection_bridge_release(server, &server->selections[index], 1);
	}

	/* A bridge asking it, which waits no more. */
	for (index = 0U; index < X11_BRIDGES; index++) {
		bridge = &server->bridges[index];
		if (bridge->asking != 0U && bridge->owner == client)
			selection_bridge_stop(bridge);
	}

	/* The transfers to it. */
	for (index = 0U; index < X11_TRANSFERS; index++) {
		if (server->transfers[index].data != NULL && server->transfers[index].client == client)
			selection_transfer_end(&server->transfers[index]);
	}
}

/*
 * Frees what the bridges and the transfers hold when the server ends.
 */
void
x11_selection_destroy(
	struct x11server *server)
{
	unsigned index;

	/* The bridges' texts. */
	for (index = 0U; index < X11_BRIDGES; index++) {
		selection_bridge_stop(&server->bridges[index]);
		free(server->bridges[index].kept);
		server->bridges[index].kept = NULL;
	}

	/* The transfers' texts. */
	for (index = 0U; index < X11_TRANSFERS; index++)
		selection_transfer_end(&server->transfers[index]);
}

/*
 * Hears that a window's property changed or was deleted: a piece of an X
 * owner's text in pieces (a new value of a bridge's property on the root
 * window), or a requestor that took a piece the server gave (a deleted
 * property of a transfer).
 */
void
x11_selection_property_event(
	struct x11server *server,
	struct x11_window *window,
	uint32_t atom,
	int deleted)
{
	struct x11_transfer *transfer;
	struct x11_bridge *bridge;
	unsigned index;

	/* A new piece for a bridge taking a text in pieces (once they started). */
	if (!deleted && window == server->root) {
		for (index = 0U; index < X11_BRIDGES; index++) {
			bridge = &server->bridges[index];
			if (bridge->incr &&
			    bridge->incr_started &&
			    bridge->property == atom) {
				selection_bridge_piece(server, bridge);
				return;
			}
		}
	}

	/* A piece taken by a requestor: the next one. */
	if (!deleted)
		return;
	for (index = 0U; index < X11_TRANSFERS; index++) {
		transfer = &server->transfers[index];
		if (transfer->data != NULL &&
		    transfer->window == window->id &&
		    transfer->property == atom) {
			selection_transfer_next(server, transfer, window);
			return;
		}
	}
}

/*
 * Starts the pieces of an X owner's text (INCR) once the owner listens to
 * the root window's property changes, or once it took too long: the
 * bridge's property is deleted, which asks for the first piece.  Returns
 * nonzero while a bridge still waits (the next tick should come soon).
 */
int
x11_selection_tick(
	struct x11server *server)
{
	struct x11_bridge *bridge;
	unsigned index;
	uint32_t mask;
	int waiting;

	/* Each bridge waiting to start its pieces. */
	waiting = 0;
	for (index = 0U; index < X11_BRIDGES; index++) {
		bridge = &server->bridges[index];
		if (!bridge->incr || bridge->incr_started)
			continue;

		/* Not yet: the owner does not listen, and the time allowed is not over. */
		mask = x11_event_mask_of(server->root, bridge->owner);
		if ((mask & X11_MASK_PROPERTY_CHANGE) == 0U && server->now - bridge->incr_since < SELECTION_INCR_WAIT) {
			waiting = 1;
			continue;
		}

		/* The first deletion. */
		bridge->incr_started = 1;
		x11_log(server, "X11 SELECTION incr start selection=%s listening=%d", selection_bridge_name(server, bridge), (mask & X11_MASK_PROPERTY_CHANGE) != 0U);
		x11_property_delete(server, server->root, bridge->property);
	}

	/* Succeeded: whether one still waits. */
	return waiting;
}

/*
 * Hears whether another program's text is the system's selection (the
 * backend): with text the root window owns CLIPBOARD or PRIMARY for it
 * (the X owner before is told SelectionClear); without, the root
 * window's ownership ends.  Either way the copy of an X owner's text is
 * the system's no more.
 */
void
x11server_backend_clipboard(
	struct x11server *server,
	unsigned selection,
	int text)
{
	struct x11_selection *entry;
	struct x11_bridge *bridge;

	/* The bridge and its selection. */
	if (selection >= X11_BRIDGES)
		return;
	bridge = &server->bridges[selection];
	entry = selection_find(server, bridge->atom, 1);
	if (entry == NULL)
		return;

	/* The copy goes; an X owner's text still on the way is not offered. */
	free(bridge->kept);
	bridge->kept = NULL;
	bridge->kept_length = 0U;
	selection_bridge_stop(bridge);

	/* Text: the root window owns it (the X owner before is told). */
	if (text) {
		if (entry->window != 0U && entry->window != X11_ROOT_XID)
			selection_clear(server, entry);
		entry->window = X11_ROOT_XID;
		entry->owner = X11_NO_CLIENT;
		entry->time = server->now;
		entry->changed = 1;
		x11_log(server, "X11 SELECTION owner selection=%s window=root client=desktop", selection_bridge_name(server, bridge));
		return;
	}

	/* Succeeded: no text, the root window's ownership ends. */
	if (entry->window == X11_ROOT_XID && entry->owner == X11_NO_CLIENT)
		entry->window = 0U;
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

/* Returns the bridge of CLIPBOARD or PRIMARY; NULL for another selection. */
static struct x11_bridge *
selection_bridge_of(
	struct x11server *server,
	uint32_t atom)
{
	unsigned index;

	/* The bridge of the selection. */
	for (index = 0U; index < X11_BRIDGES; index++) {
		if (server->bridges[index].atom == atom)
			return &server->bridges[index];
	}

	/* Not bridged. */
	return NULL;
}

/*
 * Asks a selection's X owner for its text as the target (UTF8_STRING,
 * or STRING), the root window as the requestor and the bridge's property
 * to put it in, at the time it took the selection.  A text in pieces
 * still coming from an owner before is dropped.
 */
static void
selection_bridge_ask(
	struct x11server *server,
	struct x11_bridge *bridge,
	const struct x11_selection *entry,
	uint32_t target)
{
	struct x11_client *owner;
	uint8_t event[32];

	/* What came before is dropped (and the property it used). */
	selection_bridge_stop(bridge);
	x11_property_delete(server, server->root, bridge->property);

	/* Nobody to ask. */
	owner = x11_client_of(server, entry->owner);
	if (owner == NULL)
		return;

	/* The bridge waits for that owner's answer. */
	bridge->asking = target;
	bridge->window = entry->window;
	bridge->owner = entry->owner;

	/* Succeeded: the owner is asked. */
	memset(event, 0, sizeof(event));
	event[0] = EVENT_SELECTION_REQUEST;
	x11_write32(event + 4, entry->time, owner->order);
	x11_write32(event + 8, entry->window, owner->order);
	x11_write32(event + 12, X11_ROOT_XID, owner->order);
	x11_write32(event + 16, bridge->atom, owner->order);
	x11_write32(event + 20, target, owner->order);
	x11_write32(event + 24, bridge->property, owner->order);
	selection_event(server, entry->owner, event);
}

/* Ends what a bridge waits for: an answer, or the pieces of a text (dropped). */
static void
selection_bridge_stop(
	struct x11_bridge *bridge)
{
	/* Nothing waited for, nothing gathered. */
	bridge->asking = 0U;
	bridge->incr = 0;
	bridge->incr_started = 0;
	free(bridge->data);
	bridge->data = NULL;
	bridge->length = 0U;
}

/*
 * Takes the X owner's answer in the bridge's property of the root
 * window: 8-bit text is copied and offered; INCR starts the pieces (the
 * property is deleted, which tells the owner to put the first); anything
 * else ends the asking.
 */
static void
selection_bridge_take(
	struct x11server *server,
	struct x11_bridge *bridge,
	const struct x11_property *property)
{
	uint8_t *text;
	size_t length;

	/*
	 * INCR: the pieces follow, each put after the server deletes the one
	 * before.  The first deletion waits for the tick (x11_selection_tick):
	 * an owner selects the root window's property changes after it sent
	 * its answer, and would miss a deletion made at once.
	 */
	if (property->type == server->atom_incr) {
		bridge->incr = 1;
		bridge->incr_started = 0;
		bridge->incr_since = server->now;
		bridge->length = 0U;
		x11_log(server, "X11 SELECTION incr selection=%s", selection_bridge_name(server, bridge));
		return;
	}

	/* Only 8-bit text of the type asked. */
	if (property->format != 8U || property->type != bridge->asking) {
		selection_bridge_stop(bridge);
		x11_property_delete(server, server->root, bridge->property);
		return;
	}

	/* The text, as UTF-8 (STRING is Latin-1). */
	if (property->type == X11_ATOM_STRING) {
		text = selection_latin1(property->data, property->length, &length);
	} else {
		length = property->length;
		text = malloc(length + 1U);
		if (text != NULL) {
			memcpy(text, property->data, length);
			text[length] = 0U;
		}
	}

	/* The property goes; the bridge waits no more. */
	selection_bridge_stop(bridge);
	x11_property_delete(server, server->root, bridge->property);
	if (text == NULL)
		return;

	/* Succeeded: offered. */
	selection_bridge_offer(server, bridge, text, length);
}

/*
 * Takes a piece of an X owner's text in pieces from the bridge's
 * property and deletes it (which asks for the next): an empty piece ends
 * the text, which is offered; a text past the longest the clipboard
 * passes is dropped.
 */
static void
selection_bridge_piece(
	struct x11server *server,
	struct x11_bridge *bridge)
{
	struct x11_property *property;
	uint8_t *grown;
	uint8_t *text;
	size_t length;

	/* The piece (a change the server made itself, by deleting, is not one). */
	property = x11_property_find(server->root, bridge->property);
	if (property == NULL)
		return;

	/* An empty piece: the whole text, offered. */
	if (property->length == 0U) {
		text = bridge->data;
		length = bridge->length;
		if (bridge->asking == X11_ATOM_STRING && text != NULL)
			text = selection_latin1(bridge->data, bridge->length, &length);
		if (text != bridge->data)
			free(bridge->data);
		bridge->data = NULL;
		bridge->length = 0U;
		selection_bridge_stop(bridge);
		x11_property_delete(server, server->root, bridge->property);
		if (text == NULL)
			text = calloc(1U, 1U);
		if (text == NULL)
			return;
		x11_log(server, "X11 SELECTION incr done selection=%s bytes=%lu", selection_bridge_name(server, bridge), (unsigned long)length);
		selection_bridge_offer(server, bridge, text, length);
		return;
	}

	/* A piece of 8-bit text that keeps the text within the longest passed. */
	if (property->format != 8U ||
	    property->length > X11_CLIPBOARD_TEXT_MAX - bridge->length) {
		x11_log(server, "X11 SELECTION incr dropped selection=%s", selection_bridge_name(server, bridge));
		selection_bridge_stop(bridge);
		x11_property_delete(server, server->root, bridge->property);
		return;
	}

	/* Added to what came, with room for a NUL. */
	grown = realloc(bridge->data, bridge->length + property->length + 1U);
	if (grown == NULL) {
		selection_bridge_stop(bridge);
		x11_property_delete(server, server->root, bridge->property);
		return;
	}
	memcpy(grown + bridge->length, property->data, property->length);
	bridge->data = grown;
	bridge->length += property->length;
	bridge->data[bridge->length] = 0U;

	/* Succeeded: deleted, which asks the owner for the next piece. */
	x11_property_delete(server, server->root, bridge->property);
}

/*
 * Keeps the text copied from an X owner (taking it) and offers it to the
 * backend, which puts it on the system's clipboard or primary selection
 * (or refuses it by its policy; the copy is kept for X either way).
 */
static void
selection_bridge_offer(
	struct x11server *server,
	struct x11_bridge *bridge,
	uint8_t *text,
	size_t length)
{
	unsigned selection;
	int error;

	/* The copy. */
	free(bridge->kept);
	bridge->kept = text;
	bridge->kept_length = length;

	/* Succeeded: offered. */
	selection = (unsigned)(bridge - server->bridges);
	error = server->ops->clipboard_offer(server->backend, selection, text, length);
	x11_log(server, "X11 SELECTION offer selection=%s bytes=%lu error=%d", selection_bridge_name(server, bridge), (unsigned long)length, error);
}

/*
 * A selection's X owner is gone or gave it up: for CLIPBOARD and PRIMARY
 * with a copy of an X owner's text, the root window owns the selection
 * for the copy; otherwise there is no owner.
 */
static void
selection_bridge_release(
	struct x11server *server,
	struct x11_selection *entry,
	int had_owner)
{
	struct x11_bridge *bridge;

	/* No owner, unless the copy takes it. */
	entry->window = 0U;
	bridge = selection_bridge_of(server, entry->atom);
	if (bridge == NULL)
		return;
	selection_bridge_stop(bridge);
	if (!had_owner || bridge->kept == NULL)
		return;

	/* Succeeded: the root window owns it for the copy. */
	entry->window = X11_ROOT_XID;
	entry->owner = X11_NO_CLIENT;
	x11_log(server, "X11 SELECTION owner selection=%s window=root client=copy", selection_bridge_name(server, bridge));
}

/*
 * Answers a conversion of a selection the root window owns: TARGETS
 * lists the text types; a text type puts the text (the copy of an X
 * owner's, else the system's) in the requestor's property, as
 * UTF8_STRING or STRING as asked, a large one in pieces (INCR).
 * Returns the property, or 0 (None) when there is nothing to give.
 */
static uint32_t
selection_bridge_convert(
	struct x11server *server,
	struct x11_bridge *bridge,
	unsigned index,
	uint32_t requestor,
	uint32_t target,
	uint32_t property)
{
	struct x11_window *window;
	uint8_t targets[16];
	uint32_t type;
	size_t length;
	uint8_t *text;
	unsigned code;

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

	/* The text. */
	text = selection_bridge_text(server, bridge, &length);
	if (text == NULL)
		return 0U;

	/* Of the type asked (TEXT is given as UTF8_STRING). */
	type = server->atom_utf8;
	if (target == X11_ATOM_STRING)
		type = X11_ATOM_STRING;

	/* A large text goes in pieces, which takes the text. */
	if (length > SELECTION_WHOLE_MAX)
		return selection_transfer_start(server, index, window, property, type, text, length);

	/* Whole in the requestor's property. */
	code = x11_property_set(server, window, property, type, 8U, PROPERTY_REPLACE, text, length, X11_NO_CLIENT);
	free(text);
	if (code != 0U)
		return 0U;

	/* Succeeded: the property holds it. */
	x11_log(server, "X11 SELECTION convert requestor=0x%x bytes=%lu", requestor, (unsigned long)length);
	return property;
}

/*
 * Gives the text a bridge answers with: a copy of the X owner's text it
 * kept, else the system's (read from the backend).  Returns malloc'd
 * text (the caller frees it) with its length, or NULL.
 */
static uint8_t *
selection_bridge_text(
	struct x11server *server,
	struct x11_bridge *bridge,
	size_t *length)
{
	uint8_t *text;
	unsigned selection;
	int error;

	/* A copy of the kept text. */
	*length = 0U;
	if (bridge->kept != NULL) {
		text = malloc(bridge->kept_length + 1U);
		if (text == NULL)
			return NULL;
		memcpy(text, bridge->kept, bridge->kept_length);
		text[bridge->kept_length] = 0U;
		*length = bridge->kept_length;
		return text;
	}

	/* The system's text. */
	selection = (unsigned)(bridge - server->bridges);
	error = server->ops->clipboard_read(server->backend, selection, &text, length);
	if (error != 0)
		return NULL;

	/* Succeeded: read. */
	return text;
}

/*
 * Starts giving a large text in pieces (INCR) to a requestor's property:
 * the property says INCR with the text's size, and each time the
 * requestor deletes it the next piece is put, an empty one last.  The
 * transfer takes the text.  Returns the property, or 0 (None) when no
 * transfer is free or the property cannot be set.
 */
static uint32_t
selection_transfer_start(
	struct x11server *server,
	unsigned index,
	struct x11_window *window,
	uint32_t property,
	uint32_t type,
	uint8_t *text,
	size_t length)
{
	struct x11_transfer *transfer;
	uint8_t size[4];
	unsigned slot;
	unsigned code;

	/* A transfer to the same property is replaced; else a free one. */
	transfer = NULL;
	for (slot = 0U; slot < X11_TRANSFERS; slot++) {
		if (server->transfers[slot].data != NULL &&
		    server->transfers[slot].window == window->id &&
		    server->transfers[slot].property == property) {
			selection_transfer_end(&server->transfers[slot]);
			transfer = &server->transfers[slot];
			break;
		}
	}
	for (slot = 0U; slot < X11_TRANSFERS && transfer == NULL; slot++) {
		if (server->transfers[slot].data == NULL)
			transfer = &server->transfers[slot];
	}
	if (transfer == NULL) {
		free(text);
		return 0U;
	}

	/* The property says INCR, with a lower bound of the size. */
	x11_write32(size, (uint32_t)length, 0);
	code = x11_property_set(server, window, property, server->atom_incr, 32U, PROPERTY_REPLACE, size, sizeof(size), X11_NO_CLIENT);
	if (code != 0U) {
		free(text);
		return 0U;
	}

	/* Succeeded: the pieces follow the requestor's deletions. */
	transfer->window = window->id;
	transfer->property = property;
	transfer->type = type;
	transfer->client = index;
	transfer->data = text;
	transfer->length = length;
	transfer->offset = 0U;
	transfer->done = 0;
	x11_log(server, "X11 SELECTION incr requestor=0x%x bytes=%lu", window->id, (unsigned long)length);
	return property;
}

/*
 * The requestor took a piece (deleted the property): the next piece is
 * put, an empty one after the last; once the empty one is taken the
 * transfer ends.
 */
static void
selection_transfer_next(
	struct x11server *server,
	struct x11_transfer *transfer,
	struct x11_window *window)
{
	size_t piece;
	unsigned code;

	/* The empty piece was taken: the end. */
	if (transfer->done) {
		x11_log(server, "X11 SELECTION incr sent requestor=0x%x bytes=%lu", transfer->window, (unsigned long)transfer->length);
		selection_transfer_end(transfer);
		return;
	}

	/* The next piece, or the empty one. */
	piece = transfer->length - transfer->offset;
	if (piece > SELECTION_CHUNK)
		piece = SELECTION_CHUNK;
	if (piece == 0U)
		transfer->done = 1;
	transfer->offset += piece;

	/* Put (a property that cannot be set ends the transfer). */
	code = x11_property_set(server, window, transfer->property, transfer->type, 8U, PROPERTY_REPLACE, transfer->data + transfer->offset - piece, piece, X11_NO_CLIENT);
	if (code != 0U)
		selection_transfer_end(transfer);
}

/* Ends a transfer and frees its text. */
static void
selection_transfer_end(
	struct x11_transfer *transfer)
{
	/* The slot is free. */
	free(transfer->data);
	memset(transfer, 0, sizeof(*transfer));
}

/*
 * Converts Latin-1 text (X's STRING) to UTF-8.  Returns malloc'd text
 * with a NUL after it (the caller frees it) and its length, or NULL.
 */
static uint8_t *
selection_latin1(
	const uint8_t *text,
	size_t length,
	size_t *converted)
{
	uint8_t *result;
	size_t index;
	size_t used;

	/* At most two bytes a character, and a NUL. */
	*converted = 0U;
	if (length > (X11_CLIPBOARD_TEXT_MAX - 1U) / 2U)
		return NULL;
	result = malloc(length * 2U + 1U);
	if (result == NULL)
		return NULL;

	/* Each character: ASCII as it is, the rest as two bytes. */
	used = 0U;
	for (index = 0U; index < length; index++) {
		if (text[index] < 0x80U) {
			result[used++] = text[index];
			continue;
		}
		result[used++] = (uint8_t)(0xc0U | (text[index] >> 6));
		result[used++] = (uint8_t)(0x80U | (text[index] & 0x3fU));
	}

	/* Succeeded: ended by a NUL. */
	result[used] = 0U;
	*converted = used;
	return result;
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

/* Returns the name a log line gives a bridge's selection. */
static const char *
selection_bridge_name(
	const struct x11server *server,
	const struct x11_bridge *bridge)
{
	/* PRIMARY. */
	if (bridge == &server->bridges[X11_SELECTION_PRIMARY])
		return "PRIMARY";

	/* CLIPBOARD. */
	return "CLIPBOARD";
}
