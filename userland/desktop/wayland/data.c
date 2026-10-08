/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard between clients (ws035-p079): wl_data_device_manager
 * (version 3), wl_data_source, wl_data_device and wl_data_offer.
 *
 * A client offers data by making a wl_data_source with the MIME types it
 * has and setting it as the selection.  The client with the keyboard is
 * told the selection: a new wl_data_offer (made by the compositor, from
 * the server's ID range) with the same types, then the selection event; it
 * is told again whenever the selection changes while it has the keyboard,
 * and a client that gets the keyboard is told first.  A client that wants
 * the data asks the offer to receive a type into a descriptor; the source's
 * client is asked to send that type into it.  A new selection cancels the
 * source it replaces; a source that goes empties the clipboard.
 *
 * Drag and drop (ws035-p084): start_drag while a button is held takes the
 * pointer from the clients.  The surface under the pointer -- a window's
 * body, or the part of a breadcrumb in a window's titlebar, whose titlebar
 * is told the part first (kl_titlebar_v1 version 2) -- hears enter with a
 * new offer of the source's types and actions, then motion, and leave when
 * the pointer goes elsewhere.  The target accepts a type and says the
 * actions it takes; the compositor chooses one (Ctrl prefers copy, Alt asks) and tells the
 * offer and the source.  The release drops on a target that accepted a
 * type with an action (the source hears dnd_drop_performed, and
 * dnd_finished when the target finishes); otherwise the target hears leave
 * and the source is cancelled.  Esc cancels too.  The compositor draws the
 * drag's icon surface at the pointer, or a badge of its own when it has
 * none.  ws081-p014: a finger the client hears by wl_touch starts a drag
 * as a button does (start_drag with its wl_touch.down's serial); the
 * finger then moves the pointer, and so the drag, and its lift drops
 * (touch.c).
 */

#include "desktop.h"
#include "apps-bar.h"
#include "data.h"
#include "dnd-state.h"
#include "extras.h"
#include "popup.h"
#include "titlebar.h"
#include "touch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The requests of wl_data_device_manager. */
#define MANAGER_CREATE_DATA_SOURCE	0U
#define MANAGER_GET_DATA_DEVICE		1U

/* The requests and events of wl_data_source. */
#define SOURCE_OFFER			0U
#define SOURCE_DESTROY			1U
#define SOURCE_SET_ACTIONS		2U
#define SOURCE_SEND			1U
#define SOURCE_CANCELLED		2U

/* The requests and events of wl_data_device. */
#define DEVICE_START_DRAG		0U
#define DEVICE_SET_SELECTION		1U
#define DEVICE_RELEASE			2U
#define DEVICE_DATA_OFFER		0U
#define DEVICE_ENTER			1U
#define DEVICE_LEAVE			2U
#define DEVICE_MOTION			3U
#define DEVICE_DROP			4U
#define DEVICE_SELECTION		5U

/* The drag and drop events of wl_data_source (version 3 for the last three). */
#define SOURCE_TARGET			0U
#define SOURCE_DND_DROP_PERFORMED	3U
#define SOURCE_DND_FINISHED		4U
#define SOURCE_ACTION			5U

/* The drag and drop events of wl_data_offer (version 3). */
#define OFFER_SOURCE_ACTIONS		1U
#define OFFER_ACTION			2U

/* The drag and drop actions, all of them, and the version that has them. */
#define ACTION_NONE			0U
#define ACTION_COPY			1U
#define ACTION_MOVE			2U
#define ACTION_ASK			4U
#define ACTION_ALL			7U
#define DATA_ACTIONS_VERSION		3U

/* wl_data_source's and wl_data_offer's errors about the actions. */
#define SOURCE_ERROR_INVALID_ACTION_MASK	0U
#define OFFER_ERROR_INVALID_ACTION_MASK		1U
#define OFFER_ERROR_INVALID_ACTION		2U

/* How long a release waits for the target's answer before it decides without it (milliseconds, ws189-p002 F1). */
#define DATA_RELEASE_WAIT_MS		500U

/* Ctrl and Alt in the seat's modifiers (seat.c). */
#define DATA_SEAT_CTRL			0x04U
#define DATA_SEAT_ALT			0x08U

/* The requests and events of wl_data_offer. */
#define OFFER_ACCEPT			0U
#define OFFER_RECEIVE			1U
#define OFFER_DESTROY			2U
#define OFFER_FINISH			3U
#define OFFER_SET_ACTIONS		4U
#define OFFER_OFFER			0U

/* The most MIME types one source may offer, and the longest type (with its NUL). */
#define DATA_MIME_MAX			32U
#define DATA_MIME_LENGTH		256U

static int manager_request(struct kwl_object *manager, uint32_t opcode, const unsigned char *bytes, size_t size);
static int source_request(struct kwl_object *source, uint32_t opcode, const unsigned char *bytes, size_t size);
static int device_request(struct kwl_object *device, uint32_t opcode, const unsigned char *bytes, size_t size);
static int offer_request(struct kwl_object *offer, uint32_t opcode, const unsigned char *bytes, size_t size);
static int set_selection(struct kwl_object *device, uint32_t source_id);
static void selection_changed(struct kwl_server *server);
static void send_selection(struct kwl_server *server, struct kwl_client *client);
static void send_device_selection(struct kwl_server *server, struct kwl_object *device);
static int emit_string(struct kwl_client *client, uint32_t id, uint32_t opcode, const char *text, int descriptor);
static int read_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static uint32_t data_word(const unsigned char *bytes, size_t offset);
static int start_drag(struct kwl_object *device, const unsigned char *bytes, size_t size);
static int offer_accept(struct kwl_object *offer, const unsigned char *bytes, size_t size);
static int offer_set_actions(struct kwl_object *offer, const unsigned char *bytes, size_t size);
static void offer_finish(struct kwl_object *offer);
static struct kwl_object *drag_surface_at(struct kwl_server *server, struct kwl_object **titlebar, uint32_t *id, uint32_t *detail);
static struct kwl_object *drag_plain_window_at(struct kwl_server *server, int32_t x, int32_t y);
static struct kwl_object *drag_device_of(struct kwl_client *client);
static unsigned drag_devices_in_order(struct kwl_client *client, struct kwl_object **devices);
static void drag_update(struct kwl_server *server, uint32_t time);
static void drag_enter(struct kwl_server *server, struct kwl_object *surface);
static void drag_leave(struct kwl_server *server);
static void drag_tell_part(struct kwl_server *server, struct kwl_object *titlebar, uint32_t id, uint32_t detail);
static void drag_action(struct kwl_server *server);
static uint32_t drag_choose(uint32_t source_actions, uint32_t target_actions, uint32_t preferred, uint32_t modifiers);
static void drag_place(struct kwl_server *server, struct kwl_object *surface, uint32_t *x, uint32_t *y);
static void drag_end(struct kwl_server *server);
static int drag_offer_slot(struct kwl_server *server, struct kwl_object *offer);
static void drag_pick(struct kwl_server *server);
static void drag_forget(struct kwl_server *server, struct kwl_object *object);
static void drag_clear_target(struct kwl_server *server);
static int emit_nullable(struct kwl_client *client, uint32_t id, uint32_t opcode, const char *text);

/*
 * Carries out a request of one of the data-sharing interfaces.
 */
int
kwl_data_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* Each interface has its own requests. */
	switch (object->kind) {
	case KWL_DATA_MANAGER:
		error = manager_request(object, opcode, bytes, size);
		break;
	case KWL_DATA_SOURCE:
		error = source_request(object, opcode, bytes, size);
		break;
	case KWL_DATA_DEVICE:
		error = device_request(object, opcode, bytes, size);
		break;
	case KWL_DATA_OFFER:
		error = offer_request(object, opcode, bytes, size);
		break;
	default:
		error = EPROTO;
		break;
	}

	/* Reports a request that was refused (EAGAIN: its descriptor has not come yet). */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Tells the client that is getting the keyboard the selection, before its
 * keyboard hears enter (a client that was told it last is not told again).
 */
void
kwl_data_focus(
	struct kwl_server *server,
	struct kwl_object *focus)
{
	/* No new focus, or the client told last. */
	if (focus == NULL || focus->client->number == server->selection_client)
		return;

	/* The new focus's client hears the selection. */
	send_selection(server, focus->client);
}

/*
 * Unties an object that is going from the clipboard: a source's types are
 * freed, its offers lose it, and when it was the selection the clipboard
 * is empty (the keyboard's client is told).
 */
void
kwl_data_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;
	struct kwl_client *client;
	struct kwl_object *other;
	unsigned index;

	/* A drag loses what goes: its source cancels it, its origin ends it, and the others are forgotten. */
	server = object->client->server;
	if (server->dnd_active) {
		/* The drag's source, or the surface it started from: the drag is over (the target hears leave). */
		if (object == server->dnd_source || object == server->dnd_origin) {
			printf("KWL DATA drag end reason=gone client=%llu\n", (unsigned long long)object->client->number);
			if (object == server->dnd_origin && server->dnd_source != NULL && !server->dnd_source->dead)
				(void)kwl_emit(server->dnd_source->client, server->dnd_source->id, SOURCE_CANCELLED, NULL, 0U);
			if (object == server->dnd_source)
				server->dnd_source = NULL;
			drag_leave(server);
			drag_end(server);
		}

		/* The target's surface: it hears nothing more (a new target is found at the next motion). */
		if (object == server->dnd_target)
			drag_clear_target(server);

		/* One of the target's devices or offers: the others go on (ws189-p002). */
		drag_forget(server, object);

		/* What the drop would do may have changed with the target's devices. */
		if (server->dnd_active)
			kwl_data_drag_mark(server);

		/* The icon, the titlebar told a part. */
		if (object == server->dnd_icon)
			server->dnd_icon = NULL;
		if (object == server->dnd_titlebar)
			server->dnd_titlebar = NULL;
	}

	/* A dropped offer that goes without its finish (the target gave up, the "ask" cancelled): its source is cancelled. */
	if (object->kind == KWL_DATA_OFFER && object->dnd_dropped && object->data_source != NULL && !object->data_source->dead) {
		printf("KWL DATA drag unfinished client=%llu\n", (unsigned long long)object->client->number);
		(void)kwl_emit(object->data_source->client, object->data_source->id, SOURCE_CANCELLED, NULL, 0U);
		object->dnd_dropped = 0;
	}

	/* Only a source has anything more to untie. */
	if (object->kind != KWL_DATA_SOURCE)
		return;

	/* Its types. */
	for (index = 0; index < object->mime_count; index++)
		free(object->mime_types[index]);
	free(object->mime_types);
	object->mime_types = NULL;
	object->mime_count = 0;

	/* The offers made from it have no source any more. */
	for (client = server->clients; client != NULL; client = client->next) {
		/* Each client's offers. */
		for (other = client->objects; other != NULL; other = other->next) {
			/* An offer of this source. */
			if (other->kind == KWL_DATA_OFFER && other->data_source == object)
				other->data_source = NULL;
		}
	}

	/* The selection it was: the clipboard is empty now. */
	if (server->selection == object) {
		server->selection = NULL;
		printf("KWL DATA selection none (source gone)\n");
		selection_changed(server);
	}
}

/*
 * Follows the pointer during a drag: the surface under it hears enter (or
 * leave, when it goes), or motion.
 */
void
kwl_data_drag_motion(
	struct kwl_server *server,
	uint32_t time)
{
	/* Only while dragging, and not once let go (the drop waits for its target's answer). */
	if (!server->dnd_active || server->dnd_releasing)
		return;

	/* The bar's icons first (spring-loading, apps-bar.c), then the target, its mark, and the frame (the icon or the badge moves with the pointer). */
	server->dnd_on_bar = (unsigned)kwl_apps_bar_drag_motion(server);
	drag_update(server, time);
	kwl_data_drag_mark(server);
	server->dirty = 1;
}

/*
 * Ends a drag at the release of the last button: a drop on a target that
 * accepted a type with an action, otherwise the target hears leave and the
 * source is cancelled.
 */
void
kwl_data_drag_release(
	struct kwl_server *server)
{
	struct kwl_object *offer;
	struct kwl_object *source;
	struct kwl_object *device;
	struct kwl_object *other;
	uint32_t action;
	unsigned index;
	unsigned drop;

	/* Only while dragging. */
	if (!server->dnd_active)
		return;

	/*
	 * Let go before the target answered (a quick drag let go at once,
	 * ws189-p002 F1): the drop waits for the answer, or for
	 * DATA_RELEASE_WAIT_MS (kwl_data_tick decides then).
	 */
	offer = server->dnd_offer;
	source = server->dnd_source;
	if (!server->dnd_releasing &&
	    source != NULL &&
	    offer != NULL &&
	    !offer->dnd_answered &&
	    server->dnd_target != NULL) {
		server->dnd_releasing = 1;
		server->dnd_release_deadline = kwl_milliseconds() + DATA_RELEASE_WAIT_MS;
		printf("KWL DATA drag release wait client=%llu\n", (unsigned long long)offer->client->number);
		return;
	}

	/* Decided now (a release that waited ends its wait). */
	server->dnd_releasing = 0;

	/* A drag inside its client drops on any of its surfaces; one with a source needs an accepting target with an action. */
	device = server->dnd_target_device;
	drop = 0;
	if (device != NULL && !device->dead && server->dnd_target != NULL) {
		if (source == NULL)
			drop = 1;
		if (source != NULL && offer != NULL && offer->dnd_accepted && (offer->version < DATA_ACTIONS_VERSION || offer->dnd_action != ACTION_NONE))
			drop = 1;
	}

	/* Nowhere to drop: the target (if any) hears leave, the source is cancelled. */
	if (!drop) {
		printf("KWL DATA drag cancel client=%llu reason=release\n", (unsigned long long)server->dnd_origin->client->number);
		drag_leave(server);
		if (source != NULL && !source->dead)
			(void)kwl_emit(source->client, source->id, SOURCE_CANCELLED, NULL, 0U);
		drag_end(server);
		return;
	}

	/* The target client's other devices (its other windows', ws189-p002) hear leave: only the window that took the drag has the drop. */
	for (index = 0; index < server->dnd_device_count; index++) {
		other = server->dnd_devices[index];
		if (other == NULL || other == device || other->dead)
			continue;
		(void)kwl_emit(other->client, other->id, DEVICE_LEAVE, NULL, 0U);
	}

	/* The drop: the target's device hears it, and its offer awaits the finish; its enter's serial may open a context menu (ask). */
	(void)kwl_emit(device->client, device->id, DEVICE_DROP, NULL, 0U);
	server->dnd_drop_client = device->client->number;
	server->dnd_drop_serial = server->dnd_enter_serial;
	action = ACTION_NONE;
	if (offer != NULL) {
		offer->dnd_dropped = 1;
		action = offer->dnd_action;
	}

	/* The log line the tests read. */
	printf("KWL DATA drag drop client=%llu target=%llu surface=%u action=%u\n", (unsigned long long)server->dnd_origin->client->number, (unsigned long long)device->client->number, server->dnd_target->id, action);

	/* The source hears that the drop was made, and at once that it is finished when the target cannot say so (before version 3). */
	if (source != NULL && !source->dead && source->version >= DATA_ACTIONS_VERSION) {
		(void)kwl_emit(source->client, source->id, SOURCE_DND_DROP_PERFORMED, NULL, 0U);
		if (offer != NULL && offer->version < DATA_ACTIONS_VERSION)
			(void)kwl_emit(source->client, source->id, SOURCE_DND_FINISHED, NULL, 0U);
	}

	/* Succeeded: the drag is over (the finish comes from the target). */
	drag_end(server);
}

/*
 * Decides a drop let go before its target answered (ws189-p002 F1): once
 * the target's offer has answered (with its actions too, when it can say
 * them), or at the deadline, the release is carried out as it would have
 * been.
 */
void
kwl_data_tick(
	struct kwl_server *server)
{
	struct kwl_object *offer;
	uint64_t now;
	int answered;

	/* Only a release waiting. */
	if (!server->dnd_active || !server->dnd_releasing)
		return;

	/* The target's answer: an accept (and the actions of a target that says them, sent with it), or the target gone. */
	offer = server->dnd_offer;
	answered = 0;
	if (offer == NULL || server->dnd_target == NULL)
		answered = 1;
	if (offer != NULL && offer->dnd_answered)
		answered = 1;
	if (offer != NULL && offer->dnd_answered && offer->dnd_accepted && offer->version >= DATA_ACTIONS_VERSION && offer->dnd_action == ACTION_NONE)
		answered = 0;

	/* Not yet, and time is left. */
	now = kwl_milliseconds();
	if (!answered && now < server->dnd_release_deadline)
		return;

	/* Succeeded: the release, decided now. */
	printf("KWL DATA drag release decided answered=%d\n", answered);
	kwl_data_drag_release(server);
}

/*
 * Gives up a drag (Esc): the target hears leave and the source is
 * cancelled.
 */
void
kwl_data_drag_cancel(
	struct kwl_server *server)
{
	struct kwl_object *source;

	/* Only while dragging. */
	if (!server->dnd_active)
		return;

	/* The target leaves, the source is cancelled, the drag is over. */
	printf("KWL DATA drag cancel client=%llu reason=key\n", (unsigned long long)server->dnd_origin->client->number);
	drag_leave(server);
	source = server->dnd_source;
	if (source != NULL && !source->dead)
		(void)kwl_emit(source->client, source->id, SOURCE_CANCELLED, NULL, 0U);
	drag_end(server);
}

/*
 * Decides the drag's mark (what its drop would do where the pointer is,
 * dnd-state.c, ws189-p002) from its target's answer; a changed mark is
 * logged and drawn.
 */
void
kwl_data_drag_mark(
	struct kwl_server *server)
{
	struct kwl_dnd_facts facts;
	struct kwl_object *offer;
	unsigned mark;

	/* Only while dragging. */
	if (!server->dnd_active)
		return;

	/* What is known of the drag: its source, its target, the target's answer, the bar. */
	memset(&facts, 0, sizeof(facts));
	if (server->dnd_source != NULL && !server->dnd_source->dead)
		facts.has_source = 1;
	if (server->dnd_target != NULL)
		facts.has_target = 1;
	if (server->dnd_target != NULL && server->dnd_target == server->dnd_origin)
		facts.over_origin = 1;
	offer = server->dnd_offer;
	if (offer != NULL && offer->dnd_accepted) {
		facts.accepted = 1;
		facts.action = ACTION_COPY;
		if (offer->version >= DATA_ACTIONS_VERSION)
			facts.action = offer->dnd_action;
	}

	/* A rest on the bar's applications waits for a window to come forward. */
	facts.on_bar = server->dnd_on_bar;

	/* The mark; an unchanged one is not told. */
	mark = kwl_dnd_mark(&facts);
	if (mark == server->dnd_state)
		return;

	/* Succeeded: the new mark is logged (the tests read it) and drawn. */
	server->dnd_state = mark;
	server->dirty = 1;
	printf("KWL DATA drag state=%s\n", kwl_dnd_mark_name(mark));
}

/*
 * Sends an event whose only argument is a string, with a descriptor beside
 * it (-1 for none), for the primary selection too (primary.c).
 */
int
kwl_data_emit_string(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const char *text,
	int descriptor)
{
	int error;

	/* The clipboard's own way. */
	error = emit_string(client, id, opcode, text, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the event is queued. */
	return 0;
}

/*
 * Reads a string argument at an offset, for the primary selection too
 * (primary.c).
 */
int
kwl_data_read_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	int error;

	/* The clipboard's own way. */
	error = read_string(bytes, size, offset, text, next);
	if (error != 0)
		return error;

	/* Succeeded: the text and where the next argument starts. */
	return 0;
}

/* Carries out a request of wl_data_device_manager: a new source, or a seat's data device. */
static int
manager_request(
	struct kwl_object *manager,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *seat;
	struct kwl_server *server;
	uint32_t id;
	uint32_t seat_id;

	/* A source: its new ID. */
	if (opcode == MANAGER_CREATE_DATA_SOURCE) {
		if (size != 4U)
			return EPROTO;
		id = data_word(bytes, 0U);
		created = kwl_create(manager->client, id, KWL_DATA_SOURCE, manager->version);
		if (created == NULL)
			return EPROTO;
		return 0;
	}

	/* Only get_data_device is left: its new ID and the client's seat. */
	if (opcode != MANAGER_GET_DATA_DEVICE || size != 8U)
		return EPROTO;
	id = data_word(bytes, 0U);
	seat_id = data_word(bytes, 4U);
	seat = kwl_find(manager->client, seat_id);
	if (seat == NULL || seat->kind != KWL_SEAT)
		return EPROTO;
	created = kwl_create(manager->client, id, KWL_DATA_DEVICE, manager->version);
	if (created == NULL)
		return EPROTO;

	/* A device of the client with the keyboard hears the selection at once. */
	server = manager->client->server;
	if (server->focus != NULL && server->focus->client == manager->client)
		send_device_selection(server, created);

	/* Succeeded: the client has a data device. */
	return 0;
}

/* Carries out a request of wl_data_source: a type offered, destroy, or the drag actions. */
static int
source_request(
	struct kwl_object *source,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const char *text;
	char **types;
	size_t next;
	size_t length;
	int error;

	/* The source goes (kwl_data_object_gone empties the clipboard when it held it). */
	if (opcode == SOURCE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(source);
		return 0;
	}

	/* The drag actions it offers (version 3), only the known ones. */
	if (opcode == SOURCE_SET_ACTIONS) {
		if (size != 4U)
			return EPROTO;
		source->dnd_actions = data_word(bytes, 0U);
		if ((source->dnd_actions & ~ACTION_ALL) != 0U) {
			error = kwl_error_code(source->client, source->id, SOURCE_ERROR_INVALID_ACTION_MASK, "unknown drag and drop action");
			return error;
		}

		/* Kept with the source. */
		return 0;
	}

	/* Only offer is left: one MIME type. */
	if (opcode != SOURCE_OFFER)
		return EPROTO;
	error = read_string(bytes, size, 0U, &text, &next);
	if (error != 0 || next != size)
		return EPROTO;

	/* A type too long, or one too many, is left out. */
	length = strlen(text);
	if (length >= DATA_MIME_LENGTH || source->mime_count == DATA_MIME_MAX)
		return 0;

	/* One more slot for it. */
	types = realloc(source->mime_types, (source->mime_count + 1U) * sizeof(*types));
	if (types == NULL)
		return 0;
	source->mime_types = types;

	/* Its copy. */
	types[source->mime_count] = strdup(text);
	if (types[source->mime_count] == NULL)
		return 0;
	source->mime_count++;

	/* Succeeded: the source has the type. */
	return 0;
}

/* Carries out a request of wl_data_device: a drag, the selection, or release. */
static int
device_request(
	struct kwl_object *device,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t source_id;
	int error;

	/* Each request by its opcode. */
	switch (opcode) {
	case DEVICE_START_DRAG:
		/* A drag: source, origin, icon, serial. */
		error = start_drag(device, bytes, size);
		if (error != 0)
			return error;
		return 0;
	case DEVICE_SET_SELECTION:
		/* The selection (a source or none) and the serial of the input that asked. */
		if (size != 8U)
			return EPROTO;
		source_id = data_word(bytes, 0U);
		error = set_selection(device, source_id);
		if (error != 0)
			return error;
		return 0;
	case DEVICE_RELEASE:
		/* The device goes (version 2). */
		if (size != 0U || device->version < 2U)
			return EPROTO;
		kwl_object_destroy(device);
		return 0;
	default:
		break;
	}

	/* No other request exists. */
	return EPROTO;
}

/* Carries out a request of wl_data_offer: receive a type into a descriptor, destroy, or the drag requests. */
static int
offer_request(
	struct kwl_object *offer,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *source;
	const char *text;
	size_t next;
	int descriptor;
	int error;

	/* Each request by its opcode. */
	switch (opcode) {
	case OFFER_ACCEPT:
		/* The target accepts a type, or none. */
		error = offer_accept(offer, bytes, size);
		if (error != 0)
			return error;
		return 0;
	case OFFER_SET_ACTIONS:
		/* The actions the target takes, and the one it prefers. */
		error = offer_set_actions(offer, bytes, size);
		if (error != 0)
			return error;
		return 0;
	case OFFER_FINISH:
		/* The target is done with a drop. */
		if (size != 0U)
			return EPROTO;
		offer_finish(offer);
		return 0;
	case OFFER_DESTROY:
		/* The offer goes. */
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(offer);
		return 0;
	case OFFER_RECEIVE:
		break;
	default:
		return EPROTO;
	}

	/* receive: the type, and the descriptor beside the message. */
	error = read_string(bytes, size, 0U, &text, &next);
	if (error != 0 || next != size)
		return EPROTO;
	descriptor = kwl_take_fd(offer->client);
	if (descriptor < 0)
		return EAGAIN;

	/* An offer of the compositor's own selection: the compositor writes the text (clipboard.c). */
	if (offer->data_offered) {
		printf("KWL DATA receive client=%llu mime=%s source=history\n", (unsigned long long)offer->client->number, text);
		kwl_clipboard_offer_write(descriptor);
		return 0;
	}

	/*
	 * A drag's data goes only to the window it was dropped on (ws189-p002):
	 * an offer of a drag not dropped on gets nothing (the reader sees the
	 * end at once).
	 */
	if (offer->dnd_offer && !offer->dnd_dropped) {
		close(descriptor);
		printf("KWL DATA receive client=%llu mime=%s refused=not-dropped\n", (unsigned long long)offer->client->number, text);
		return 0;
	}

	/* An offer whose source has gone has nothing to send; the descriptor is closed (the reader sees its end). */
	source = offer->data_source;
	if (source == NULL || source->dead || source->client->fatal) {
		close(descriptor);
		printf("KWL DATA receive client=%llu mime=%s source=none\n", (unsigned long long)offer->client->number, text);
		return 0;
	}

	/* Succeeded: the source's client writes the type into the descriptor (the event carries it away). */
	printf("KWL DATA receive client=%llu mime=%s source=%llu\n", (unsigned long long)offer->client->number, text, (unsigned long long)source->client->number);
	(void)emit_string(source->client, source->id, SOURCE_SEND, text, descriptor);
	return 0;
}

/*
 * Asks a source's client to write a type into a descriptor (the clipboard's
 * history reads the text so, clipboard.c); the event takes the descriptor.
 */
int
kwl_data_send(
	struct kwl_object *source,
	const char *type,
	int descriptor)
{
	int error;

	/* The source's send event. */
	error = emit_string(source->client, source->id, SOURCE_SEND, type, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the client writes. */
	return 0;
}

/*
 * Makes the compositor's own text (an item of the clipboard's history,
 * clipboard.c) the selection: the source it replaces is cancelled, and the
 * client with the keyboard hears it.
 */
void
kwl_data_select_offered(
	struct kwl_server *server)
{
	struct kwl_object *previous;

	/* The source replaced hears that it is not the selection any more. */
	previous = server->selection;
	if (previous != NULL && !previous->dead)
		(void)kwl_emit(previous->client, previous->id, SOURCE_CANCELLED, NULL, 0U);

	/* The compositor's selection, told to the client with the keyboard. */
	server->selection = NULL;
	server->selection_offered = 1;
	printf("KWL DATA selection history\n");
	selection_changed(server);
}

/* Sets the selection from a client's device: a source of the client, or none; the replaced source is cancelled. */
static int
set_selection(
	struct kwl_object *device,
	uint32_t source_id)
{
	struct kwl_server *server;
	struct kwl_object *source;
	struct kwl_object *previous;
	unsigned types;

	/* The source must be one of the client's. */
	source = NULL;
	if (source_id != 0U) {
		source = kwl_find(device->client, source_id);
		if (source == NULL || source->kind != KWL_DATA_SOURCE)
			return EPROTO;
	}

	/* An unchanged selection tells nobody. */
	server = device->client->server;
	previous = server->selection;
	if (previous == source)
		return 0;

	/* The source replaced hears that it is not the selection any more. */
	if (previous != NULL && !previous->dead)
		(void)kwl_emit(previous->client, previous->id, SOURCE_CANCELLED, NULL, 0U);

	/* The new selection (with how many types it has), and the client with the keyboard hears it. */
	server->selection = source;
	server->selection_offered = 0;
	types = 0;
	if (source != NULL)
		types = source->mime_count;
	printf("KWL DATA selection client=%llu source=%u types=%u\n", (unsigned long long)device->client->number, source_id, types);
	selection_changed(server);

	/* Its text joins the clipboard's history (clipboard.c). */
	kwl_clipboard_selected(server, source);

	/* Succeeded: the clipboard holds the source. */
	return 0;
}

/* Tells the client with the keyboard that the selection changed. */
static void
selection_changed(
	struct kwl_server *server)
{
	/* Without a focus nobody is told now (the next focus is). */
	server->selection_client = 0;
	if (server->focus == NULL || server->focus->dead)
		return;

	/* The focused client hears it. */
	send_selection(server, server->focus->client);
}

/* Tells every data device of a client the selection. */
static void
send_selection(
	struct kwl_server *server,
	struct kwl_client *client)
{
	struct kwl_object *device;

	/* A failed client hears nothing. */
	if (client->fatal)
		return;

	/* Each live data device. */
	for (device = client->objects; device != NULL; device = device->next) {
		/* Only live devices. */
		if (device->kind == KWL_DATA_DEVICE && !device->dead)
			send_device_selection(server, device);
	}

	/* The client has been told. */
	server->selection_client = client->number;
}

/*
 * Tells one data device the selection: a new offer with the source's types
 * and the selection event naming it, or the selection event naming none.
 */
static void
send_device_selection(
	struct kwl_server *server,
	struct kwl_object *device)
{
	struct kwl_object *source;
	struct kwl_object *offer;
	uint32_t word;
	unsigned index;

	/* The compositor's own selection, an item of the clipboard's history, is offered as text. */
	source = server->selection;
	word = 0;
	if (source == NULL && server->selection_offered) {
		offer = kwl_create_server(device->client, KWL_DATA_OFFER, device->version);
		if (offer == NULL)
			return;
		offer->data_offered = 1;
		word = offer->id;
		(void)kwl_emit(device->client, device->id, DEVICE_DATA_OFFER, &word, sizeof(word));
		(void)emit_string(device->client, offer->id, OFFER_OFFER, "text/plain;charset=utf-8", -1);
		(void)emit_string(device->client, offer->id, OFFER_OFFER, "text/plain", -1);
		(void)kwl_emit(device->client, device->id, DEVICE_SELECTION, &word, sizeof(word));
		printf("KWL DATA offer client=%llu offer=%u types=2 history=1\n", (unsigned long long)device->client->number, offer->id);
		return;
	}

	/* An empty clipboard: the selection names no offer. */
	if (source == NULL || source->dead) {
		(void)kwl_emit(device->client, device->id, DEVICE_SELECTION, &word, sizeof(word));
		return;
	}

	/* The offer, made by the compositor. */
	offer = kwl_create_server(device->client, KWL_DATA_OFFER, device->version);
	if (offer == NULL)
		return;
	offer->data_source = source;

	/* It is introduced, with each of the source's types. */
	word = offer->id;
	(void)kwl_emit(device->client, device->id, DEVICE_DATA_OFFER, &word, sizeof(word));
	for (index = 0; index < source->mime_count; index++)
		(void)emit_string(device->client, offer->id, OFFER_OFFER, source->mime_types[index], -1);

	/* Succeeded: the selection names it. */
	(void)kwl_emit(device->client, device->id, DEVICE_SELECTION, &word, sizeof(word));
	printf("KWL DATA offer client=%llu offer=%u types=%u\n", (unsigned long long)device->client->number, offer->id, source->mime_count);
}

/* Sends an event whose only argument is a string, with a descriptor beside it when there is one (-1 for none). */
static int
emit_string(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const char *text,
	int descriptor)
{
	unsigned char payload[4U + DATA_MIME_LENGTH + 4U];
	uint32_t length;
	size_t padded;
	int error;

	/* The length with its NUL, the text, the padding. */
	length = (uint32_t)strlen(text) + 1U;
	if (length > DATA_MIME_LENGTH) {
		if (descriptor >= 0)
			close(descriptor);
		return EINVAL;
	}

	/* The payload. */
	memset(payload, 0, sizeof(payload));
	memcpy(payload, &length, sizeof(length));
	memcpy(payload + 4, text, length);
	padded = 4U + (((size_t)length + 3U) & ~(size_t)3U);

	/* The event (the descriptor goes with it). */
	if (descriptor >= 0) {
		error = kwl_emit_fd(client, id, opcode, payload, padded, descriptor);
	} else {
		error = kwl_emit(client, id, opcode, payload, padded);
	}

	/* Reports an event that could not be queued. */
	if (error != 0)
		return error;

	/* Succeeded: the event is queued. */
	return 0;
}

/* Reads a string argument at an offset: its length word, its bytes with the NUL, the padding. */
static int
read_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t padded;

	/* The length word. */
	if (offset + 4U > size)
		return EPROTO;
	length = data_word(bytes, offset);
	if (length == 0U)
		return EPROTO;

	/* The bytes and their padding must fit, and end with the NUL. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;

	/* Succeeded: the text and where the next argument starts. */
	*text = (const char *)(bytes + offset + 4U);
	*next = offset + 4U + padded;
	return 0;
}

/* Reads one native-endian protocol word. */
static uint32_t
data_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}

/*
 * Starts a drag (wl_data_device.start_drag): a source of the client (or
 * none, for a drag inside the client), the surface it starts from, an icon
 * surface (or none) and the serial of the press.  The drag needs a button
 * held; otherwise, or while another drag runs, the source is cancelled at
 * once.
 */
static int
start_drag(
	struct kwl_object *device,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *source;
	struct kwl_object *origin;
	struct kwl_object *icon;
	uint32_t source_id;
	uint32_t origin_id;
	uint32_t icon_id;
	uint32_t serial;
	uint32_t actions;
	unsigned types;
	int held;

	/* The arguments: source (or none), origin, icon (or none), serial. */
	if (size != 16U)
		return EPROTO;
	source_id = data_word(bytes, 0U);
	origin_id = data_word(bytes, 4U);
	icon_id = data_word(bytes, 8U);

	/* The source must be one of the client's sources. */
	source = NULL;
	if (source_id != 0U) {
		source = kwl_find(device->client, source_id);
		if (source == NULL || source->kind != KWL_DATA_SOURCE)
			return EPROTO;
	}

	/* The origin must be one of its surfaces. */
	origin = kwl_find(device->client, origin_id);
	if (origin == NULL || origin->kind != KWL_SURFACE)
		return EPROTO;

	/* And so must the icon, when there is one; it is a drag's icon from here, the drag refused or not. */
	icon = NULL;
	if (icon_id != 0U) {
		icon = kwl_find(device->client, icon_id);
		if (icon == NULL || icon->kind != KWL_SURFACE)
			return EPROTO;
		icon->drag_icon = 1U;
	}

	/*
	 * Without a button held or the client's finger of the serial down
	 * (ws081-p014), or while another drag runs, there is no drag: the
	 * source is cancelled.  A finger that starts it is the drag's from here.
	 */
	server = device->client->server;
	serial = data_word(bytes, 12U);
	held = 0;
	if (!server->dnd_active && server->buttons_down != 0U)
		held = 1;
	if (!server->dnd_active && !held)
		held = kwl_touch_drag_start(server, device->client, serial);
	if (!held) {
		if (source != NULL)
			(void)kwl_emit(source->client, source->id, SOURCE_CANCELLED, NULL, 0U);
		printf("KWL DATA drag refused client=%llu buttons=%u serial=%u\n", (unsigned long long)device->client->number, server->buttons_down, serial);
		return 0;
	}

	/* The drag, with no target yet; its icon's corner starts at the pointer (offsets attached from here move it). */
	server->dnd_active = 1;
	server->dnd_source = source;
	server->dnd_origin = origin;
	server->dnd_icon = icon;
	if (icon != NULL) {
		icon->offset_x = 0;
		icon->offset_y = 0;
		icon->pending_dx = 0;
		icon->pending_dy = 0;
	}

	/* No target, no breadcrumb part, nothing to say yet. */
	drag_clear_target(server);
	server->dnd_titlebar = NULL;
	server->dnd_part_id = 0;
	server->dnd_part_detail = 0;
	server->dnd_state = KWL_DND_STATE_NEUTRAL;

	/* The source's types and actions (none without a source), for the log line the tests read. */
	types = 0;
	actions = 0;
	if (source != NULL) {
		types = source->mime_count;
		actions = source->dnd_actions;
	}

	/* The line. */
	printf("KWL DATA drag start client=%llu source=%u types=%u actions=%u icon=%u\n", (unsigned long long)device->client->number, source_id, types, actions, icon_id);

	/* The pointer is the drag's now: the surface it was on hears leave. */
	if (server->pointer_surface != NULL) {
		kwl_seat_pointer_move(server, server->pointer_surface, NULL);
		server->pointer_surface = NULL;
	}

	/* Succeeded: no rest on the bar yet, the surface under the pointer is the first target, and the drag's mark is told. */
	kwl_apps_bar_drag_end(server);
	server->dnd_on_bar = (unsigned)kwl_apps_bar_drag_motion(server);
	drag_update(server, 0U);
	kwl_data_drag_mark(server);
	server->dirty = 1;
	return 0;
}

/*
 * Carries out wl_data_offer.accept: the serial of the enter and a type (or
 * none).  The drag's source hears the type as its target.
 */
static int
offer_accept(
	struct kwl_object *offer,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *source;
	const char *text;
	uint32_t length;
	size_t next;
	int error;
	int slot;

	/* The serial, then the type or a null string. */
	if (size < 8U)
		return EPROTO;
	text = NULL;
	length = data_word(bytes, 4U);
	if (length != 0U) {
		error = read_string(bytes, size, 4U, &text, &next);
		if (error != 0 || next != size)
			return EPROTO;
	}

	/* Only the offers of the drag's target count; a selection's or an old one's accept does nothing. */
	server = offer->client->server;
	slot = drag_offer_slot(server, offer);
	if (!server->dnd_active || slot < 0)
		return 0;

	/* Whether a type is accepted; the offer has answered either way. */
	offer->dnd_answered = 1;
	offer->dnd_accepted = 0;
	if (text != NULL)
		offer->dnd_accepted = 1;
	if (text != NULL) {
		printf("KWL DATA drag accept client=%llu mime=%s\n", (unsigned long long)offer->client->number, text);
	} else {
		printf("KWL DATA drag accept client=%llu mime=(none)\n", (unsigned long long)offer->client->number);
	}

	/* The offer of the window that accepted becomes the drag's (ws189-p002); another window's refusal changes nothing. */
	drag_pick(server);
	kwl_data_drag_mark(server);
	if (offer != server->dnd_offer)
		return 0;

	/* Succeeded: the source hears it as its target. */
	source = offer->data_source;
	if (source != NULL && !source->dead)
		(void)emit_nullable(source->client, source->id, SOURCE_TARGET, text);
	return 0;
}

/*
 * Carries out wl_data_offer.set_actions: the actions the target takes and
 * the one it prefers (one of them, or none).  The drag's action is chosen
 * again.
 */
static int
offer_set_actions(
	struct kwl_object *offer,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	uint32_t actions;
	uint32_t preferred;
	int error;
	int slot;

	/* The two masks. */
	if (size != 8U)
		return EPROTO;
	actions = data_word(bytes, 0U);
	preferred = data_word(bytes, 4U);

	/* Only known actions. */
	if ((actions & ~ACTION_ALL) != 0U) {
		error = kwl_error_code(offer->client, offer->id, OFFER_ERROR_INVALID_ACTION_MASK, "unknown drag and drop action");
		return error;
	}

	/* The preferred one is one action (or none). */
	if (preferred != ACTION_NONE && preferred != ACTION_COPY && preferred != ACTION_MOVE && preferred != ACTION_ASK) {
		error = kwl_error_code(offer->client, offer->id, OFFER_ERROR_INVALID_ACTION, "invalid preferred drag and drop action");
		return error;
	}

	/* Kept with the offer. */
	offer->dnd_actions = actions;
	offer->dnd_preferred = preferred;

	/*
	 * After a drop with "ask", the target says the action the user chose;
	 * the offer and the source hear it (a target that destroyed the offer
	 * at once drops the offer's event).
	 */
	if (offer->dnd_dropped && offer->dnd_action == ACTION_ASK && preferred != ACTION_NONE && preferred != ACTION_ASK) {
		offer->dnd_action = preferred;
		printf("KWL DATA drag chosen client=%llu action=%u\n", (unsigned long long)offer->client->number, preferred);
		if (offer->version >= DATA_ACTIONS_VERSION)
			(void)kwl_emit(offer->client, offer->id, OFFER_ACTION, &preferred, sizeof(preferred));
		if (offer->data_source != NULL && !offer->data_source->dead && offer->data_source->version >= DATA_ACTIONS_VERSION)
			(void)kwl_emit(offer->data_source->client, offer->data_source->id, SOURCE_ACTION, &preferred, sizeof(preferred));
		return 0;
	}

	/* The drag's action is chosen again when this is one of its target's offers. */
	server = offer->client->server;
	slot = drag_offer_slot(server, offer);
	if (server->dnd_active && slot >= 0) {
		drag_pick(server);
		drag_action(server);
		kwl_data_drag_mark(server);
	}

	/* Succeeded: the actions are kept. */
	return 0;
}

/* Carries out wl_data_offer.finish: the source of a drop hears that the target is done. */
static void
offer_finish(
	struct kwl_object *offer)
{
	struct kwl_object *source;

	/* Only an offer that was dropped on. */
	if (!offer->dnd_dropped)
		return;
	offer->dnd_dropped = 0;

	/* The source, still there, hears it. */
	source = offer->data_source;
	printf("KWL DATA drag finish client=%llu action=%u\n", (unsigned long long)offer->client->number, offer->dnd_action);
	if (source != NULL && !source->dead && source->version >= DATA_ACTIONS_VERSION)
		(void)kwl_emit(source->client, source->id, SOURCE_DND_FINISHED, NULL, 0U);
}

/*
 * Finds the surface under the pointer that a drag can be dropped on: in the
 * glass look, a part of a breadcrumb in a window's titlebar (with its
 * titlebar and the part), or else a window's body; in the plain look the
 * window under the pointer.  NULL for none.
 */
static struct kwl_object *
drag_surface_at(
	struct kwl_server *server,
	struct kwl_object **titlebar,
	uint32_t *id,
	uint32_t *detail)
{
	struct kwl_object *surface;
	int found;

	/* No part yet. */
	*titlebar = NULL;
	*id = 0;
	*detail = 0;

	/* The plain look has windows only. */
	if (!server->glass) {
		surface = drag_plain_window_at(server, server->pointer_x, server->pointer_y);
		return surface;
	}

	/* A part of a breadcrumb in a titlebar (titlebar-shell.c). */
	found = kwl_titlebar_drop_at(server, server->pointer_x, server->pointer_y, &surface, titlebar, id, detail);
	if (found)
		return surface;

	/* A window's body (shell.c). */
	surface = kwl_glass_body_at(server, server->pointer_x, server->pointer_y);
	if (surface != NULL)
		return surface;

	/* Succeeded: the desktop's icons where no window is, or none (desktop.c). */
	surface = kwl_desktop_at(server, server->pointer_x, server->pointer_y);
	return surface;
}

/* Finds the window on top whose image is under a point (the plain look); NULL for none. */
static struct kwl_object *
drag_plain_window_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	struct kwl_object *found;
	uint32_t width;
	uint32_t height;

	/* The mapped window of the desktop shown with the highest map order. */
	found = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed client has no target. */
		if (client->fatal)
			continue;

		/* Each of its windows. */
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only mapped windows of the desktop shown. */
			if (surface->kind != KWL_SURFACE || surface->dead || !surface->mapped || surface->role == NULL)
				continue;
			if (surface->desktop != server->desktop || surface->minimized)
				continue;

			/* The point on its image. */
			kwl_surface_size(surface, &width, &height);
			if (x < surface->x || y < surface->y || x >= surface->x + (int32_t)width || y >= surface->y + (int32_t)height)
				continue;

			/* Above what was found so far. */
			if (found == NULL || surface->map_order > found->map_order)
				found = surface;
		}
	}

	/* Succeeded: the window, or NULL. */
	return found;
}

/* Finds a client's first live data device; NULL when it has none. */
static struct kwl_object *
drag_device_of(
	struct kwl_client *client)
{
	struct kwl_object *object;

	/* A failed client has none. */
	if (client->fatal)
		return NULL;

	/* The first live one. */
	for (object = client->objects; object != NULL; object = object->next) {
		/* Only data devices. */
		if (object->kind == KWL_DATA_DEVICE && !object->dead)
			return object;
	}

	/* None. */
	return NULL;
}

/*
 * Gives a client's live data devices, the oldest first (the client's list
 * holds its objects newest first), at most KWL_DND_DEVICES of them: the
 * oldest ones when it has more.  Returns how many.
 */
static unsigned
drag_devices_in_order(
	struct kwl_client *client,
	struct kwl_object **devices)
{
	struct kwl_object *object;
	unsigned count;
	unsigned index;
	unsigned half;

	/* A failed client has none. */
	if (client->fatal)
		return 0;

	/* The live devices newest first; past the most kept, the newest goes so that the oldest stay. */
	count = 0;
	for (object = client->objects; object != NULL; object = object->next) {
		/* Only data devices. */
		if (object->kind != KWL_DATA_DEVICE || object->dead)
			continue;

		/* A full list drops its newest, at the front. */
		if (count == KWL_DND_DEVICES) {
			memmove(devices, devices + 1, sizeof(devices[0]) * (KWL_DND_DEVICES - 1U));
			count--;
		}

		/* The device after the newer ones. */
		devices[count] = object;
		count++;
	}

	/* The list turned round: the oldest first. */
	half = count / 2U;
	for (index = 0; index < half; index++) {
		object = devices[index];
		devices[index] = devices[count - 1U - index];
		devices[count - 1U - index] = object;
	}

	/* Succeeded: how many devices there are. */
	return count;
}

/*
 * Finds the drag's target under the pointer and tells it: a new target
 * hears enter (the old one leave), the same one motion; a titlebar hears
 * the part of its breadcrumb first.
 */
static void
drag_update(
	struct kwl_server *server,
	uint32_t time)
{
	struct kwl_object *surface;
	struct kwl_object *titlebar;
	struct kwl_object *device;
	uint32_t words[3];
	uint32_t id;
	uint32_t detail;
	unsigned index;

	/* The surface under the pointer, and the part of a breadcrumb there; none while the drag rests on the bar's applications (apps-bar.c). */
	surface = drag_surface_at(server, &titlebar, &id, &detail);
	if (server->dnd_on_bar) {
		surface = NULL;
		titlebar = NULL;
	}

	/* A drag inside its client has only that client's surfaces as targets. */
	if (surface != NULL && server->dnd_source == NULL && surface->client != server->dnd_origin->client) {
		surface = NULL;
		titlebar = NULL;
	}

	/* A client without a data device cannot take a drop. */
	device = NULL;
	if (surface != NULL)
		device = drag_device_of(surface->client);
	if (device == NULL) {
		surface = NULL;
		titlebar = NULL;
	}

	/* The titlebar part first, so that the client knows it before the enter or the motion. */
	drag_tell_part(server, titlebar, id, detail);

	/* A new target: the old one leaves, the new one enters. */
	if (surface != server->dnd_target) {
		drag_leave(server);
		if (surface != NULL)
			drag_enter(server, surface);
		return;
	}

	/* No target: nobody to tell. */
	if (surface == NULL || server->dnd_device_count == 0U)
		return;

	/* The same target hears the motion on each of its client's devices, and the action may change with Ctrl. */
	words[0] = time;
	drag_place(server, surface, &words[1], &words[2]);
	for (index = 0; index < server->dnd_device_count; index++) {
		device = server->dnd_devices[index];
		if (device == NULL || device->dead)
			continue;
		(void)kwl_emit(device->client, device->id, DEVICE_MOTION, words, sizeof(words));
	}

	/* The action, which Ctrl and Alt may change. */
	drag_action(server);
}

/*
 * Tells a new target the drag: each data device of its client (each
 * window of a program has one, ws189-p002) hears a new offer of its own
 * with the source's types and actions (none for a drag inside its
 * client), then enter at the pointer's place.  The window the surface is
 * takes the offer; the others refuse theirs.
 */
static void
drag_enter(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_object *devices[KWL_DND_DEVICES];
	struct kwl_object *object;
	struct kwl_object *offer;
	struct kwl_object *source;
	uint32_t words[5];
	uint32_t word;
	uint32_t serial;
	unsigned found;
	unsigned count;
	unsigned slot;
	unsigned index;

	/* One serial for the enter on every device (kept: a context menu may answer it after a drop). */
	serial = kwl_next_serial(server);
	server->dnd_enter_serial = serial;
	source = server->dnd_source;

	/*
	 * The client's live data devices in the order they were made (its
	 * first window's first), so that when several windows take the drag
	 * the first of them has the drop (drag_pick; ws189-p002 F7).
	 */
	found = drag_devices_in_order(surface->client, devices);

	/* Each of them, as many as are kept. */
	count = 0;
	for (slot = 0; slot < found; slot++) {
		object = devices[slot];

		/* The offer of the source's types, made by the compositor, when there is a source. */
		offer = NULL;
		if (source != NULL && !source->dead) {
			offer = kwl_create_server(object->client, KWL_DATA_OFFER, object->version);
			if (offer == NULL)
				continue;
			offer->data_source = source;
			offer->dnd_offer = 1;

			/* It is introduced with each type. */
			word = offer->id;
			(void)kwl_emit(object->client, object->id, DEVICE_DATA_OFFER, &word, sizeof(word));
			for (index = 0; index < source->mime_count; index++)
				(void)emit_string(object->client, offer->id, OFFER_OFFER, source->mime_types[index], -1);

			/* And the source's actions (version 3; a source before that copies). */
			word = ACTION_COPY;
			if (source->version >= DATA_ACTIONS_VERSION)
				word = source->dnd_actions;
			if (offer->version >= DATA_ACTIONS_VERSION)
				(void)kwl_emit(object->client, offer->id, OFFER_SOURCE_ACTIONS, &word, sizeof(word));
		}

		/* Enter: the serial, the surface, the pointer's place on it and the offer. */
		words[0] = serial;
		words[1] = surface->id;
		drag_place(server, surface, &words[2], &words[3]);
		words[4] = 0;
		if (offer != NULL)
			words[4] = offer->id;
		(void)kwl_emit(object->client, object->id, DEVICE_ENTER, words, sizeof(words));

		/* The device and its offer are the target's. */
		server->dnd_devices[count] = object;
		server->dnd_offers[count] = offer;
		count++;
	}

	/* No device heard it: no target. */
	if (count == 0U)
		return;

	/* Succeeded: the target, its devices, and the first pair until a window accepts. */
	server->dnd_target = surface;
	server->dnd_device_count = count;
	drag_pick(server);
	printf("KWL DATA drag enter client=%llu surface=%u devices=%u output=%u x=%d y=%d\n", (unsigned long long)surface->client->number, surface->id, count, surface->output, (int32_t)words[2] / 256, (int32_t)words[3] / 256);
}

/* Tells the target that the drag left it, on each of its client's devices (their offers are no longer the drag's). */
static void
drag_leave(
	struct kwl_server *server)
{
	struct kwl_object *device;
	unsigned index;

	/* No target. */
	if (server->dnd_target == NULL) {
		drag_clear_target(server);
		return;
	}

	/* Each device hears leave. */
	for (index = 0; index < server->dnd_device_count; index++) {
		device = server->dnd_devices[index];
		if (device == NULL || device->dead)
			continue;
		(void)kwl_emit(device->client, device->id, DEVICE_LEAVE, NULL, 0U);
	}

	/* The log line the tests read. */
	printf("KWL DATA drag leave client=%llu surface=%u\n", (unsigned long long)server->dnd_target->client->number, server->dnd_target->id);

	/* Succeeded: no target. */
	drag_clear_target(server);
}

/*
 * Tells a titlebar the part of its breadcrumb the drag is over (an
 * unchanged part is not told again); the titlebar told before hears that
 * the drag is over none of its parts.
 */
static void
drag_tell_part(
	struct kwl_server *server,
	struct kwl_object *titlebar,
	uint32_t id,
	uint32_t detail)
{
	/* The same part: nothing new. */
	if (titlebar == server->dnd_titlebar && id == server->dnd_part_id && detail == server->dnd_part_detail)
		return;

	/* The titlebar told before, when it is another one, hears none. */
	if (server->dnd_titlebar != NULL && server->dnd_titlebar != titlebar)
		kwl_titlebar_send_drop_target(server->dnd_titlebar, 0U, 0U);

	/* The new part. */
	server->dnd_titlebar = titlebar;
	server->dnd_part_id = id;
	server->dnd_part_detail = detail;

	/* Succeeded: its titlebar hears it. */
	if (titlebar != NULL)
		kwl_titlebar_send_drop_target(titlebar, id, detail);
}

/*
 * Chooses the drag's action from the source's actions and the target's
 * (Ctrl held prefers copy), and tells the offer and the source when it
 * changed.
 */
static void
drag_action(
	struct kwl_server *server)
{
	struct kwl_object *offer;
	struct kwl_object *source;
	uint32_t source_actions;
	uint32_t target_actions;
	uint32_t preferred;
	uint32_t action;

	/* Only an offer of a source still there. */
	offer = server->dnd_offer;
	source = server->dnd_source;
	if (offer == NULL || source == NULL || source->dead)
		return;

	/* The source's actions; one before version 3 copies. */
	source_actions = ACTION_COPY;
	if (source->version >= DATA_ACTIONS_VERSION)
		source_actions = source->dnd_actions;

	/* The target's; one before version 3 copies. */
	target_actions = ACTION_COPY;
	preferred = ACTION_COPY;
	if (offer->version >= DATA_ACTIONS_VERSION) {
		target_actions = offer->dnd_actions;
		preferred = offer->dnd_preferred;
	}

	/* The choice; an unchanged one is not told. */
	action = drag_choose(source_actions, target_actions, preferred, server->modifiers);
	if (action == offer->dnd_action)
		return;
	offer->dnd_action = action;
	printf("KWL DATA drag action client=%llu action=%u\n", (unsigned long long)offer->client->number, action);

	/* The offer hears it (version 3). */
	if (offer->version >= DATA_ACTIONS_VERSION)
		(void)kwl_emit(offer->client, offer->id, OFFER_ACTION, &action, sizeof(action));

	/* And so does the source. */
	if (source->version >= DATA_ACTIONS_VERSION)
		(void)kwl_emit(source->client, source->id, SOURCE_ACTION, &action, sizeof(action));
}

/*
 * Chooses one action both sides take: copy while Ctrl is held, ask while
 * Alt is, otherwise the target's preferred one, otherwise copy, move and
 * ask in that order.
 * ACTION_NONE when they share none.
 */
static uint32_t
drag_choose(
	uint32_t source_actions,
	uint32_t target_actions,
	uint32_t preferred,
	uint32_t modifiers)
{
	uint32_t both;

	/* What both take. */
	both = source_actions & target_actions;

	/* Ctrl asks for a copy. */
	if ((modifiers & DATA_SEAT_CTRL) != 0U && (both & ACTION_COPY) != 0U)
		return ACTION_COPY;

	/* Alt asks the target to ask (it offers the choice after the drop). */
	if ((modifiers & DATA_SEAT_ALT) != 0U && (both & ACTION_ASK) != 0U)
		return ACTION_ASK;

	/* The target's preference. */
	if (preferred != ACTION_NONE && (both & preferred) != 0U)
		return preferred;

	/* Otherwise the first both take. */
	if ((both & ACTION_COPY) != 0U)
		return ACTION_COPY;
	if ((both & ACTION_MOVE) != 0U)
		return ACTION_MOVE;
	if ((both & ACTION_ASK) != 0U)
		return ACTION_ASK;

	/* None. */
	return ACTION_NONE;
}

/*
 * Gives the pointer's place on a surface in 24.8 fixed point: from the
 * body's corner as it is drawn in the glass look (so a titlebar above the
 * body is at a negative y), from the surface's place otherwise.
 */
static void
drag_place(
	struct kwl_server *server,
	struct kwl_object *surface,
	uint32_t *x,
	uint32_t *y)
{
	int32_t left;
	int32_t top;

	/* The surface's corner on the output. */
	left = surface->x;
	top = surface->y;
	if (server->glass)
		(void)kwl_glass_body_origin(server, surface, &left, &top);

	/* Succeeded: the pointer from there. */
	*x = (uint32_t)((server->pointer_x - left) * 256);
	*y = (uint32_t)((server->pointer_y - top) * 256);
}

/*
 * Ends a drag: its state is cleared, a titlebar told a part hears none,
 * and the pointer goes back to the surface it belongs to.
 */
static void
drag_end(
	struct kwl_server *server)
{
	/* The part of a breadcrumb is none now. */
	drag_tell_part(server, NULL, 0U, 0U);

	/* The drag's state. */
	server->dnd_active = 0;
	server->dnd_releasing = 0;
	server->dnd_source = NULL;
	server->dnd_origin = NULL;
	server->dnd_icon = NULL;
	drag_clear_target(server);
	server->dnd_state = KWL_DND_STATE_NEUTRAL;
	server->dnd_on_bar = 0;
	server->dirty = 1;

	/* Spring-loading ends with it (apps-bar.c). */
	kwl_apps_bar_drag_end(server);

	/* Succeeded: the pointer's surface hears enter again. */
	kwl_seat_pointer_update(server);
}

/* Sends an event whose only argument is a string that may be null (a zero length). */
static int
emit_nullable(
	struct kwl_client *client,
	uint32_t id,
	uint32_t opcode,
	const char *text)
{
	uint32_t word;
	int error;

	/* A string is sent as it is. */
	if (text != NULL) {
		error = emit_string(client, id, opcode, text, -1);
		return error;
	}

	/* A null string is its zero length alone. */
	word = 0;
	error = kwl_emit(client, id, opcode, &word, sizeof(word));
	if (error != 0)
		return error;

	/* Succeeded: the event is queued. */
	return 0;
}

/* Finds which of the drag's target's offers an offer is; -1 when it is none of them. */
static int
drag_offer_slot(
	struct kwl_server *server,
	struct kwl_object *offer)
{
	unsigned index;

	/* Each offer the target's devices heard. */
	for (index = 0; index < server->dnd_device_count; index++) {
		if (server->dnd_offers[index] == offer && offer != NULL)
			return (int)index;
	}

	/* None of them. */
	return -1;
}

/*
 * Chooses the target's device and offer the drag goes on with: the first
 * whose window accepted a type; else, with a source, the first offer still
 * there; else the first device still there (ws189-p002).
 */
static void
drag_pick(
	struct kwl_server *server)
{
	unsigned index;

	/* Nothing chosen yet. */
	server->dnd_target_device = NULL;
	server->dnd_offer = NULL;

	/* A window that accepted a type. */
	for (index = 0; index < server->dnd_device_count; index++) {
		if (server->dnd_offers[index] == NULL || !server->dnd_offers[index]->dnd_accepted)
			continue;
		server->dnd_target_device = server->dnd_devices[index];
		server->dnd_offer = server->dnd_offers[index];
		return;
	}

	/* An offer still there (its window has not answered). */
	for (index = 0; index < server->dnd_device_count; index++) {
		if (server->dnd_offers[index] == NULL || server->dnd_devices[index] == NULL)
			continue;
		server->dnd_target_device = server->dnd_devices[index];
		server->dnd_offer = server->dnd_offers[index];
		return;
	}

	/* A drag with a source whose offers have all gone has no target device; one without a source takes the first device. */
	if (server->dnd_source != NULL)
		return;

	/* Without a source the first device still there takes it. */
	for (index = 0; index < server->dnd_device_count; index++) {
		if (server->dnd_devices[index] == NULL)
			continue;
		server->dnd_target_device = server->dnd_devices[index];
		return;
	}
}

/*
 * Takes a device or an offer that goes out of the drag's target: its slot
 * is emptied (a device's offer with it), and the drag goes on with what is
 * left; with no device left there is no target.
 */
static void
drag_forget(
	struct kwl_server *server,
	struct kwl_object *object)
{
	unsigned index;
	unsigned found;
	unsigned left;

	/* The slots that hold it. */
	found = 0;
	left = 0;
	for (index = 0; index < server->dnd_device_count; index++) {
		/* The device, with its offer. */
		if (server->dnd_devices[index] == object) {
			server->dnd_devices[index] = NULL;
			server->dnd_offers[index] = NULL;
			found = 1;
		}

		/* The offer alone (its window refused the drag). */
		if (server->dnd_offers[index] == object) {
			server->dnd_offers[index] = NULL;
			found = 1;
		}

		/* The devices that still hear the drag. */
		if (server->dnd_devices[index] != NULL)
			left++;
	}

	/* Not one of the target's. */
	if (!found)
		return;

	/* No device left: no target (a new one is found at the next motion). */
	if (left == 0U) {
		drag_clear_target(server);
		return;
	}

	/* Succeeded: the drag goes on with what is left. */
	drag_pick(server);
}

/* Forgets the drag's target, its devices and its offers. */
static void
drag_clear_target(
	struct kwl_server *server)
{
	unsigned index;

	/* Every slot, then the pair chosen. */
	for (index = 0; index < KWL_DND_DEVICES; index++) {
		server->dnd_devices[index] = NULL;
		server->dnd_offers[index] = NULL;
	}

	/* No target, no pair. */
	server->dnd_device_count = 0;
	server->dnd_target = NULL;
	server->dnd_target_device = NULL;
	server->dnd_offer = NULL;
}
