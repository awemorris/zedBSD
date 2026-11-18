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
 *
 * An application the user turned off on Settings' Notifications page
 * (notify.allow.<its name> 0, ws177-p026) is refused: result(request,
 * DENIED); its name is the post's, or its window's app_id.
 *
 * On Linux the desktop's other programs post on the session bus
 * (org.freedesktop.Notifications, ws156-p006): the backend serves it and
 * hands each notification here, as a client of its own for each bus
 * connection (NOTIFY_BUS_CLIENT and the backend's sender number, never a
 * Wayland client's number), with no object.  Their closes and clicks go
 * back to the backend, which tells the sender.  A sender with as many as it
 * may has its oldest logged one expired for the new one, as a program on
 * the bus does not expect a refusal.
 */

#include "kwl.h"
#include "notify.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest string a post carries, with its NUL (longer words than the model keeps are refused as INVALID, not as a broken request). */
#define NOTIFY_WIRE_TEXT_MAX	4096U

/*
 * The first client number of the bus's senders (ws156-p006): a sender's
 * notifications are kept as client NOTIFY_BUS_CLIENT + its number.
 * Wayland clients are numbered from 1 up and never come near it.
 */
#define NOTIFY_BUS_CLIENT	(1ULL << 62)

static struct kwl_notify_model notify_model;
static int notify_ready;

static void notify_open(void);
static void notify_left(uint64_t client, uint32_t object);
static int notify_post(struct kwl_object *object, const unsigned char *bytes, size_t size, int lock);
static int notify_allowed(struct kwl_object *object, const char *app);
static int notify_allowed_name(struct kwl_server *server, const char *name);
static int notify_from_bus(uint64_t client);
static unsigned notify_bus_reason(unsigned reason);
static uint32_t notify_bus_posted(void *data, uint32_t sender, const char *app, const char *title, const char *body, uint32_t replaces, unsigned flags, int *error);
static int notify_bus_withdrawn(void *data, uint32_t sender, uint32_t id);
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
		return notify_post(object, bytes, size, 0);

	/* A notification with what the lock screen shows of it (ws197-p004c, version 27). */
	if (opcode == KL_SYSTEM_NOTIFY_POST_LOCK && object->version >= KL_SYSTEM_SINCE_NOTIFY_LOCK)
		return notify_post(object, bytes, size, 1);

	/* A notification taken back. */
	if (opcode == KL_SYSTEM_NOTIFY_WITHDRAW)
		return notify_withdraw(object, bytes, size);

	/* No other request. */
	return EPROTO;
}

/*
 * Serves the notifications of the desktop's bus (ws156-p006, Linux's
 * session bus) for a session: the backend hands each one here.  Returns 0,
 * or the backend's error (ENOTSUP where the system has no such bus).
 */
int
kwl_notify_bus_open(
	struct kwl_server *server)
{
	struct kl_backend_notify_callbacks callbacks;
	int error;

	/* What the backend calls with the bus's posts and withdrawals. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.posted = notify_bus_posted;
	callbacks.withdrawn = notify_bus_withdrawn;

	/* The bus, served by the backend (the log the tests read). */
	notify_open();
	error = kl_backend_notify_open(server->backend, &callbacks, server);
	if (error != 0) {
		if (error != ENOTSUP)
			printf("KWL NOTIFY bus unavailable errno=%d\n", error);
		return error;
	}

	/* Succeeded: the bus's notifications come here. */
	printf("KWL NOTIFY bus serving\n");
	return 0;
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
	uint32_t id;

	/* As the System's, nothing on the lock screen. */
	id = kwl_notify_post_as(server, "System", title, body, flags, NULL);
	return id;
}

/*
 * Posts a notification of the compositor's own (client 0) under an
 * application's name (ws197-p004c: the phone's messages when the phone
 * program is not running), with what the lock screen shows of it (NULL:
 * nothing).  Returns its number, or 0 when it could not be kept.
 */
uint32_t
kwl_notify_post_as(
	struct kwl_server *server,
	const char *app,
	const char *title,
	const char *body,
	unsigned flags,
	const char *lock_text)
{
	struct kwl_notify_closed closed[2];
	size_t closed_count;
	uint32_t id;
	int error;

	/* Kept as the application's. */
	notify_open();
	error = kwl_notify_post(&notify_model, 0U, 0U, 0U, app, title, body, flags, &id, closed, &closed_count);
	if (error != 0) {
		printf("KWL NOTIFY post-failed client=0 error=%d\n", error);
		return 0U;
	}

	/* What the lock screen shows of it. */
	if (lock_text != NULL)
		(void)kwl_notify_set_lock_text(&notify_model, id, lock_text);

	/* An oldest pushed out of the log is told to its client; the log the tests read. */
	if (closed_count > 0U)
		notify_tell_closed(server, &closed[0]);
	printf("KWL NOTIFY post client=0 id=%u flags=%u title=\"%.40s\" waiting=%lu\n", id, flags, title, (unsigned long)kwl_notify_waiting(&notify_model));
	return id;
}

/*
 * Finds the application ID of a client's mapped window ("" when it has
 * none): the name of a notification that has none of its own.
 */
const char *
kwl_notify_app_id(
	const struct kwl_server *server,
	uint64_t client_number)
{
	struct kwl_client *client;
	struct kwl_object *surface;

	/* The client, then its first mapped window with an ID. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->number != client_number || client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != KWL_SURFACE || surface->dead || !surface->mapped)
				continue;
			if (surface->role == NULL || surface->app_id[0] == '\0')
				continue;
			return surface->app_id;
		}
	}

	/* Succeeded: no ID. */
	return "";
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
	int bus;

	/* The notification. */
	notify_open();
	item = kwl_notify_find(&notify_model, id);
	if (item == NULL)
		return ENOENT;
	number = item->client;

	/* Its client's object, told; the compositor's own runs its action; the bus's sender is told by the backend. */
	bus = notify_from_bus(number);
	if (number == 0U) {
		kwl_notify_system_activated(server, id);
	} else if (bus) {
		kl_backend_notify_activated(server->backend, id);
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

/* Carries out post(request, replaces, app, title, body, flags), or post_lock(..., flags, lock_text) when lock is 1. */
static int
notify_post(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size,
	int lock)
{
	struct kwl_notify_closed closed[2];
	const char *lock_text;
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
	int allowed;
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
	if (error != 0 || offset + 4U > size)
		return EPROTO;
	flags = notify_word(bytes, offset);
	offset += 4U;

	/* What the lock screen shows of it, the last argument of post_lock. */
	lock_text = "";
	if (lock) {
		error = notify_string(bytes, size, offset, &lock_text, &offset);
		if (error != 0)
			return EPROTO;
	}

	/* Nothing after the last argument. */
	if (offset != size)
		return EPROTO;

	/* A client posting faster than the rate is refused as busy (ws177-p005). */
	error = kwl_notify_rate_take(&notify_model, object->client->number, kwl_milliseconds());
	if (error != 0) {
		printf("KWL NOTIFY refused client=%llu request=%u error=%d rate=1\n", (unsigned long long)object->client->number, request, error);
		notify_result(object, request, KL_SYSTEM_RESULT_BUSY);
		return 0;
	}

	/* An application the user turned off (ws177-p026). */
	allowed = notify_allowed(object, app);
	if (!allowed) {
		printf("KWL NOTIFY denied client=%llu request=%u app=\"%.40s\"\n", (unsigned long long)object->client->number, request, app);
		notify_result(object, request, KL_SYSTEM_RESULT_DENIED);
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

	/* What the lock screen shows of it (words too long show nothing there). */
	if (lock)
		(void)kwl_notify_set_lock_text(&notify_model, id, lock_text);

	/* Its number (the log the tests read). */
	words[0] = request;
	words[1] = id;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_NOTIFY_EVENT_POSTED, words, sizeof(words));
	printf("KWL NOTIFY post client=%llu id=%u replaces=%u flags=%u app=\"%.40s\" title=\"%.40s\" waiting=%lu\n",
	       (unsigned long long)object->client->number, id, replaces, flags, app, title, (unsigned long)kwl_notify_waiting(&notify_model));
	return 0;
}

/*
 * Tells whether the user lets an application notify: its setting
 * notify.allow.<name> (the post's name, or its window's app_id) is not 0;
 * a name without a setting is let.
 */
static int
notify_allowed(
	struct kwl_object *object,
	const char *app)
{
	const char *name;
	int allowed;

	/* The application's name: the post's, or its window's app_id. */
	name = app;
	if (name[0] == '\0')
		name = kwl_notify_app_id(object->client->server, object->client->number);

	/* Its setting. */
	allowed = notify_allowed_name(object->client->server, name);
	return allowed;
}

/*
 * Tells whether the user lets an application of a name notify: its setting
 * notify.allow.<name> is not 0.  An empty name, and a name without a
 * setting, is let.
 */
static int
notify_allowed_name(
	struct kwl_server *server,
	const char *name)
{
	char key[96];
	int number;
	int error;

	/* No name, nothing to look up. */
	if (name[0] == '\0')
		return 1;

	/* Its setting; none (or no settings) lets it. */
	(void)snprintf(key, sizeof(key), "notify.allow.%s", name);
	error = kwl_settings_number(server, key, &number);
	if (error != 0)
		return 1;

	/* Succeeded: whether it is let. */
	if (number == 0)
		return 0;
	return 1;
}

/* Tells whether a notification's client is one of the bus's senders (ws156-p006). */
static int
notify_from_bus(
	uint64_t client)
{
	/* The bus's senders are numbered from NOTIFY_BUS_CLIENT up. */
	if (client >= NOTIFY_BUS_CLIENT)
		return 1;

	/* A Wayland client, or the compositor. */
	return 0;
}

/* Gives the bus's reason of a close (KL_BACKEND_NOTIFY_*) for the compositor's (KWL_NOTIFY_*). */
static unsigned
notify_bus_reason(
	unsigned reason)
{
	/* Aged out of the log. */
	if (reason == KWL_NOTIFY_EXPIRED)
		return KL_BACKEND_NOTIFY_EXPIRED;

	/* Taken back by its sender. */
	if (reason == KWL_NOTIFY_WITHDRAWN)
		return KL_BACKEND_NOTIFY_CLOSED;

	/* The user's: its close sign, or the log's clearing. */
	return KL_BACKEND_NOTIFY_DISMISSED;
}

/*
 * Takes a notification of the bus (the backend's posted callback): kept as
 * its sender's, under the same rate and the same user's choice as a
 * Wayland client's.  Returns its number, or 0 with *error EBUSY (too fast,
 * or too many with none logged to expire), EACCES (the user turned the
 * program off) or the model's error.
 */
static uint32_t
notify_bus_posted(
	void *data,
	uint32_t sender,
	const char *app,
	const char *title,
	const char *body,
	uint32_t replaces,
	unsigned flags,
	int *error)
{
	struct kwl_notify_closed closed[2];
	struct kwl_notify_closed expired;
	struct kwl_server *server;
	size_t closed_count;
	uint64_t client;
	unsigned kept_flags;
	uint32_t id;
	int allowed;
	int status;

	/* The sender as a client of its own. */
	server = data;
	client = NOTIFY_BUS_CLIENT + (uint64_t)sender;
	notify_open();

	/* A sender posting faster than the rate is refused as busy (ws177-p005). */
	status = kwl_notify_rate_take(&notify_model, client, kwl_milliseconds());
	if (status != 0) {
		printf("KWL NOTIFY bus-refused sender=%u error=%d rate=1\n", sender, status);
		*error = EBUSY;
		return 0U;
	}

	/* A program the user turned off (ws177-p026). */
	allowed = notify_allowed_name(server, app);
	if (!allowed) {
		printf("KWL NOTIFY bus-denied sender=%u app=\"%.40s\"\n", sender, app);
		*error = EACCES;
		return 0U;
	}

	/* The flags the compositor keeps. */
	kept_flags = 0U;
	if ((flags & KL_BACKEND_NOTIFY_URGENT) != 0U)
		kept_flags |= KWL_NOTIFY_URGENT;
	if ((flags & KL_BACKEND_NOTIFY_ACTION) != 0U)
		kept_flags |= KWL_NOTIFY_ACTION;

	/* Kept; a replaces that is not the sender's (gone, or another's) makes a new one. */
	status = kwl_notify_post(&notify_model, client, 0U, replaces, app, title, body, kept_flags, &id, closed, &closed_count);
	if (status == ENOENT && replaces != 0U) {
		replaces = 0U;
		status = kwl_notify_post(&notify_model, client, 0U, 0U, app, title, body, kept_flags, &id, closed, &closed_count);
	}

	/* A sender with as many as it may: its oldest logged one expires for this one. */
	if (status == EBUSY) {
		status = kwl_notify_expire_oldest(&notify_model, client, &expired);
		if (status != 0) {
			printf("KWL NOTIFY bus-refused sender=%u error=%d\n", sender, EBUSY);
			*error = EBUSY;
			return 0U;
		}

		/* The expired one told, then the post again. */
		notify_tell_closed(server, &expired);
		status = kwl_notify_post(&notify_model, client, 0U, replaces, app, title, body, kept_flags, &id, closed, &closed_count);
	}

	/* Refused otherwise (the words are cut to fit by the backend, so this is rare). */
	if (status != 0) {
		printf("KWL NOTIFY bus-refused sender=%u error=%d\n", sender, status);
		*error = status;
		return 0U;
	}

	/* An oldest pushed out of the log is told to its client. */
	if (closed_count > 0U)
		notify_tell_closed(server, &closed[0]);

	/* Succeeded: its number (the log the tests read). */
	printf("KWL NOTIFY post bus sender=%u id=%u replaces=%u flags=%u app=\"%.40s\" title=\"%.40s\" waiting=%lu\n",
	       sender, id, replaces, kept_flags, app, title, (unsigned long)kwl_notify_waiting(&notify_model));
	return id;
}

/*
 * Takes back a notification of the bus (the backend's withdrawn callback,
 * CloseNotification): the sender's own only.  Returns 0, or ENOENT.
 */
static int
notify_bus_withdrawn(
	void *data,
	uint32_t sender,
	uint32_t id)
{
	struct kwl_notify_closed closed;
	struct kwl_server *server;
	uint64_t client;
	int error;

	/* The sender's own. */
	server = data;
	client = NOTIFY_BUS_CLIENT + (uint64_t)sender;
	notify_open();
	error = kwl_notify_withdraw(&notify_model, client, id, &closed);
	if (error != 0)
		return error;

	/* Told as closed (NotificationClosed with the reason of a close by its sender). */
	notify_tell_closed(server, &closed);
	printf("KWL NOTIFY withdraw bus sender=%u id=%u\n", sender, id);
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
	int bus;

	/* The log the tests read. */
	printf("KWL NOTIFY closed client=%llu id=%u reason=%u\n", (unsigned long long)closed->client, closed->id, closed->reason);

	/* The compositor's own are told to nobody. */
	if (closed->client == 0U)
		return;

	/* The bus's sender is told by the backend (ws156-p006). */
	bus = notify_from_bus(closed->client);
	if (bus) {
		kl_backend_notify_closed(server->backend, closed->id, notify_bus_reason(closed->reason));
		return;
	}

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
