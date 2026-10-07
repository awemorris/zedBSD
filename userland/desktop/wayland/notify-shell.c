/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The applications' notifications on the wire (ws156-p002,
 * plan/ws156/phase001/phase.md section 2): kl_system_notify_v1, made by
 * the system manager's get_notify (since its version 13) for a client of
 * the compositor's own user.
 *
 *   post(request, replaces, app, title, body, flags)  a notification, or
 *       new words for the client's earlier one; answered by posted(request,
 *       id), or by result(request, INVALID or BUSY) when it is refused
 *   withdraw(request, id)  the client's notification goes; closed(id,
 *       WITHDRAWN) and result(request, OK)
 *   activated(id)  its body was clicked (an ACTION one; the popup, p003)
 *   closed(id, reason)  it went: dismissed, expired out of the log, cleared,
 *       withdrawn
 *
 * The notifications are kept in one model for the compositor's life
 * (notify.c); an empty application name is the client's window's app_id
 * (p003 draws it).  The compositor's own notifications (a device plugged
 * in, p003) come by kwl_notify_post_system as client 0.  A closed
 * notification whose client or object went is told to nobody.
 */

#include "kwl.h"
#include "notify.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest string a post carries, with its NUL (longer words than the model keeps are refused as INVALID, not as a broken request). */
#define NOTIFY_WIRE_TEXT_MAX	4096U

static struct kwl_notify_model notify_model;
static int notify_ready;

static void notify_open(void);
static void notify_left(uint64_t client, uint32_t object);
static int notify_post(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int notify_withdraw(struct kwl_object *object, const unsigned char *bytes, size_t size);
static void notify_tell_closed(struct kwl_server *server, const struct kwl_notify_closed *closed);
static void notify_result(struct kwl_object *object, uint32_t request, uint32_t applied);
static int notify_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static uint32_t notify_word(const unsigned char *bytes, size_t offset);

/*
 * Makes a notify object for a manager's get_notify (new id).  Returns 0,
 * or EPROTO for a malformed request.
 */
int
kwl_notify_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = notify_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_NOTIFY, manager->version);
	if (created == NULL)
		return EPROTO;
	notify_open();
	printf("KWL NOTIFY object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);

	/* Succeeded: the object is the client's. */
	return 0;
}

/*
 * Carries out a request of a notify object.  Returns 0, or EPROTO for a
 * malformed request.
 */
int
kwl_notify_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	/* The object goes; its notifications stay, with no action and nobody to tell (ws177-p005). */
	if (opcode == KL_SYSTEM_NOTIFY_DESTROY) {
		if (size != 0U)
			return EPROTO;
		notify_left(object->client->number, object->id);
		kwl_object_destroy(object);
		return 0;
	}

	/* A notification. */
	if (opcode == KL_SYSTEM_NOTIFY_POST)
		return notify_post(object, bytes, size);

	/* A notification taken back. */
	if (opcode == KL_SYSTEM_NOTIFY_WITHDRAW)
		return notify_withdraw(object, bytes, size);

	/* No other request. */
	return EPROTO;
}

/*
 * Leaves a client's notifications with nobody to tell when the client
 * goes (ws177-p005): they stay shown and logged, without their action.
 */
void
kwl_notify_client_gone(
	struct kwl_client *client)
{
	/* No notification was ever made: nothing to leave. */
	if (!notify_ready)
		return;

	/* Every one of the client's. */
	notify_left(client->number, 0U);
}

/*
 * Posts a notification of the compositor's own (client 0): a title, a
 * body and flags.  Returns its number, or 0 when it could not be kept.
 */
uint32_t
kwl_notify_post_system(
	struct kwl_server *server,
	const char *title,
	const char *body,
	unsigned flags)
{
	struct kwl_notify_closed closed[2];
	size_t closed_count;
	uint32_t id;
	int error;

	/* Kept as the System's. */
	notify_open();
	error = kwl_notify_post(&notify_model, 0U, 0U, 0U, "System", title, body, flags, &id, closed, &closed_count);
	if (error != 0) {
		printf("KWL NOTIFY post-failed client=0 error=%d\n", error);
		return 0U;
	}

	/* An oldest pushed out of the log is told to its client; the log the tests read. */
	if (closed_count > 0U)
		notify_tell_closed(server, &closed[0]);
	printf("KWL NOTIFY post client=0 id=%u flags=%u title=\"%.40s\" waiting=%lu\n", id, flags, title, (unsigned long)kwl_notify_waiting(&notify_model));
	return id;
}

/*
 * Ends the show of the notification shown (the popup's board started to
 * leave, or it is not shown now): it goes to the log, whose oldest it may
 * push out (that client told).
 */
void
kwl_notify_hide_shown(
	struct kwl_server *server)
{
	struct kwl_notify_closed closed[2];
	size_t closed_count;
	int error;

	/* Into the log. */
	notify_open();
	error = kwl_notify_hide(&notify_model, closed, &closed_count);
	if (error != 0)
		return;

	/* An oldest pushed out of the log is told to its client. */
	if (closed_count > 0U)
		notify_tell_closed(server, &closed[0]);
}

/*
 * Dismisses a notification (its close sign): it goes, not logged, and its
 * client is told closed(DISMISSED).  Returns 0, or ENOENT.
 */
int
kwl_notify_dismiss_id(
	struct kwl_server *server,
	uint32_t id)
{
	struct kwl_notify_closed closed;
	int error;

	/* Gone from wherever it is. */
	notify_open();
	error = kwl_notify_dismiss(&notify_model, id, &closed);
	if (error != 0)
		return error;

	/* Its client told. */
	notify_tell_closed(server, &closed);
	return 0;
}

/*
 * Activates a notification (its body clicked, an ACTION one): its client
 * is told activated(id), then it goes as dismissed (not logged).  The
 * compositor's own runs its action (kwl_notify_system_activated).  Returns
 * 0, or ENOENT.
 */
int
kwl_notify_activate(
	struct kwl_server *server,
	uint32_t id)
{
	const struct kwl_notification *item;
	struct kwl_client *client;
	struct kwl_object *object;
	uint64_t number;
	uint32_t word;
	int error;

	/* The notification. */
	notify_open();
	item = kwl_notify_find(&notify_model, id);
	if (item == NULL)
		return ENOENT;
	number = item->client;

	/* Its client's object, told; the compositor's own runs its action. */
	if (number == 0U) {
		kwl_notify_system_activated(server, id);
	} else {
		for (client = server->clients; client != NULL; client = client->next) {
			if (client->number != number || client->fatal)
				continue;
			object = kwl_find(client, item->object);
			if (object == NULL || object->dead || object->kind != KWL_SYSTEM_NOTIFY)
				break;
			word = id;
			(void)kwl_emit(client, object->id, KL_SYSTEM_NOTIFY_EVENT_ACTIVATED, &word, sizeof(word));
			break;
		}
	}

	/* Then it goes, not logged. */
	error = kwl_notify_dismiss_id(server, id);
	if (error != 0)
		return error;

	/* Succeeded: activated. */
	return 0;
}

/*
 * Clears the log ("Clear all notifications", p004): every logged
 * notification goes, each client told closed(CLEARED).  Returns how many
 * went.
 */
size_t
kwl_notify_clear_log(
	struct kwl_server *server)
{
	struct kwl_notify_closed closed[KWL_NOTIFY_LOG];
	size_t count;
	size_t index;

	/* The log emptied. */
	notify_open();
	count = kwl_notify_clear(&notify_model, closed, KWL_NOTIFY_LOG);

	/* Each told. */
	for (index = 0U; index < count && index < KWL_NOTIFY_LOG; index++)
		notify_tell_closed(server, &closed[index]);

	/* Succeeded: how many went. */
	return count;
}

/* Gives the model of the notifications (for the popup and the log, p003). */
struct kwl_notify_model *
kwl_notify_model(void)
{
	/* Made the first time. */
	notify_open();
	return &notify_model;
}

/* Makes the model the first time it is needed. */
static void
notify_open(void)
{
	/* Once. */
	if (notify_ready)
		return;
	kwl_notify_model_init(&notify_model);
	notify_ready = 1;
}

/* Leaves a client's notifications (of one object, or 0 for all) with nobody to tell; logged when there were some. */
static void
notify_left(
	uint64_t client,
	uint32_t object)
{
	size_t count;

	/* The model's. */
	notify_open();
	count = kwl_notify_orphan(&notify_model, client, object);

	/* The log the tests read. */
	if (count != 0U)
		printf("KWL NOTIFY left client=%llu object=%u count=%lu\n", (unsigned long long)client, object, (unsigned long)count);
}

/* Carries out post(request, replaces, app, title, body, flags). */
static int
notify_post(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_notify_closed closed[2];
	const char *app;
	const char *title;
	const char *body;
	size_t closed_count;
	size_t offset;
	uint32_t request;
	uint32_t replaces;
	uint32_t flags;
	uint32_t words[2];
	uint32_t id;
	int error;

	/* The arguments. */
	if (size < 8U)
		return EPROTO;
	request = notify_word(bytes, 0U);
	replaces = notify_word(bytes, 4U);
	error = notify_string(bytes, size, 8U, &app, &offset);
	if (error == 0)
		error = notify_string(bytes, size, offset, &title, &offset);
	if (error == 0)
		error = notify_string(bytes, size, offset, &body, &offset);
	if (error != 0 || offset + 4U != size)
		return EPROTO;
	flags = notify_word(bytes, offset);

	/* A client posting faster than the rate is refused as busy (ws177-p005). */
	error = kwl_notify_rate_take(&notify_model, object->client->number, kwl_milliseconds());
	if (error != 0) {
		printf("KWL NOTIFY refused client=%llu request=%u error=%d rate=1\n", (unsigned long long)object->client->number, request, error);
		notify_result(object, request, KL_SYSTEM_RESULT_BUSY);
		return 0;
	}

	/* Kept, or refused: words too long, a number not the client's, too many. */
	error = kwl_notify_post(&notify_model, object->client->number, object->id, replaces, app, title, body, flags, &id, closed, &closed_count);
	if (error != 0) {
		printf("KWL NOTIFY refused client=%llu request=%u error=%d\n", (unsigned long long)object->client->number, request, error);
		if (error == EBUSY)
			notify_result(object, request, KL_SYSTEM_RESULT_BUSY);
		else
			notify_result(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* An oldest pushed out of the log is told to its client. */
	if (closed_count > 0U)
		notify_tell_closed(object->client->server, &closed[0]);

	/* Its number (the log the tests read). */
	words[0] = request;
	words[1] = id;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_NOTIFY_EVENT_POSTED, words, sizeof(words));
	printf("KWL NOTIFY post client=%llu id=%u replaces=%u flags=%u app=\"%.40s\" title=\"%.40s\" waiting=%lu\n",
	       (unsigned long long)object->client->number, id, replaces, flags, app, title, (unsigned long)kwl_notify_waiting(&notify_model));
	return 0;
}

/* Carries out withdraw(request, id). */
static int
notify_withdraw(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_notify_closed closed;
	uint32_t request;
	uint32_t id;
	int error;

	/* The arguments. */
	if (size != 8U)
		return EPROTO;
	request = notify_word(bytes, 0U);
	id = notify_word(bytes, 4U);

	/* The client's own goes; another's or none is refused. */
	error = kwl_notify_withdraw(&notify_model, object->client->number, id, &closed);
	if (error != 0) {
		notify_result(object, request, KL_SYSTEM_RESULT_INVALID);
		return 0;
	}

	/* Told as closed, then answered (logged). */
	notify_tell_closed(object->client->server, &closed);
	notify_result(object, request, KL_SYSTEM_RESULT_OK);
	printf("KWL NOTIFY withdraw client=%llu id=%u\n", (unsigned long long)object->client->number, id);
	return 0;
}

/* Tells a notification's client that it closed, when the client and its object are still there (logged). */
static void
notify_tell_closed(
	struct kwl_server *server,
	const struct kwl_notify_closed *closed)
{
	struct kwl_client *client;
	struct kwl_object *object;
	uint32_t words[2];

	/* The log the tests read. */
	printf("KWL NOTIFY closed client=%llu id=%u reason=%u\n", (unsigned long long)closed->client, closed->id, closed->reason);

	/* The compositor's own are told to nobody. */
	if (closed->client == 0U)
		return;

	/* The client, and its object of the notification. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->number != closed->client || client->fatal)
			continue;
		object = kwl_find(client, closed->object);
		if (object == NULL || object->dead || object->kind != KWL_SYSTEM_NOTIFY)
			return;
		words[0] = closed->id;
		words[1] = closed->reason;
		(void)kwl_emit(client, object->id, KL_SYSTEM_NOTIFY_EVENT_CLOSED, words, sizeof(words));
		return;
	}
}

/* Answers a request: result(request, applied, saved). */
static void
notify_result(
	struct kwl_object *object,
	uint32_t request,
	uint32_t applied)
{
	uint32_t words[3];

	/* request, applied, saved. */
	words[0] = request;
	words[1] = applied;
	words[2] = applied;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_NOTIFY_EVENT_RESULT, words, sizeof(words));
}

/*
 * Reads a string argument in place; *next is the offset after it.
 * Returns 0, or EPROTO for one that does not fit the request or the bound.
 */
static int
notify_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	const void *inner;
	uint32_t length;
	size_t padded;

	/* The length, with the NUL, within the request and the bound. */
	if (offset + 4U > size)
		return EPROTO;
	length = notify_word(bytes, offset);
	if (length == 0U || length > NOTIFY_WIRE_TEXT_MAX)
		return EPROTO;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;

	/* The text ends with its NUL and has no other. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;
	inner = memchr(bytes + offset + 4U, '\0', length - 1U);
	if (inner != NULL)
		return EPROTO;

	/* Succeeded: the text where it is, and where the next argument starts. */
	*text = (const char *)(bytes + offset + 4U);
	*next = offset + 4U + padded;
	return 0;
}

/* Reads a 32-bit word of a request in the wire's native byte order. */
static uint32_t
notify_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
