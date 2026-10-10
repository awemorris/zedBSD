/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Relays mediastorage metadata operations and committed changes without owning its database. */
#include "kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Matches the backend's bounded helper queue; results remain owned during backpressure. */
#define MEDIA_PENDING_MAX 16U

/* One client request keeps its completed snapshot until the event queue accepts it. */
struct media_pending {
	struct kwl_object *object;
	uint32_t backend;
	uint32_t request;
	unsigned command;
	int descriptor;
	int error;
	int ready;
	int snapshot_sent;
};

/* Only the compositor's event-loop thread accesses this helper state. */
static struct {
	struct kl_backend_media *backend;
	struct media_pending pending[MEDIA_PENDING_MAX];
	uint32_t generation;
	int opened;
} media_state;

static uint32_t media_word(const unsigned char *bytes, size_t offset);
static int media_argument(const unsigned char *bytes, size_t size, const char **argument);
static void media_answers(struct kwl_server *server);
static void media_deliver(struct media_pending *pending);
static void media_watchers(struct kwl_server *server);

/*
 * Creates a version-one metadata object for a version-29 manager.
 */
int
kwl_media_library_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *object;
	uint32_t id;

	/* The manager provides only the new object id. */
	if (size != 4U)
		return EPROTO;
	id = media_word(bytes, 0U);
	object = kwl_create(manager->client, id, KWL_SYSTEM_MEDIA, 1U);
	if (object == NULL)
		return EPROTO;

	/* Opens the session endpoint even if the client only registers a watcher. */
	if (!media_state.opened) {
		media_state.opened = 1;
		media_state.backend = kl_backend_media_open();
		media_state.generation = 1U;
	}

	/* Succeeded: the client may query, add paths, apply metadata or watch. */
	return 0;
}

/*
 * Delegates a bounded metadata operation to the asynchronous OS backend.
 */
int
kwl_media_library_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct media_pending *pending;
	const char *argument;
	uint32_t request;
	uint32_t payload[2];
	unsigned command;
	size_t index;
	int descriptor;
	int error;

	/* Destroy and watch never start database operations. */
	if (opcode == KL_SYSTEM_MEDIA_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A watcher receives each generation once, including the state at registration. */
	if (opcode == KL_SYSTEM_MEDIA_WATCH) {
		if (size != 4U)
			return EPROTO;
		object->media_watching = media_word(bytes, 0U);
		object->media_generation = 0U;
		return 0;
	}

	/* CLI mutations wake watchers through the compositor extension after their disk commit. */
	if (opcode == KL_SYSTEM_MEDIA_NOTIFY) {
		if (size != 0U)
			return EPROTO;
		media_state.generation++;
		if (media_state.generation == 0U)
			media_state.generation = 1U;
		return 0;
	}

	/* Validates complete wire arguments before consuming a descriptor. */
	argument = "";
	descriptor = -1;
	command = 0U;
	if (opcode == KL_SYSTEM_MEDIA_LIST && size == 4U) {
		command = KL_BACKEND_MEDIA_LIST;
	} else if (opcode == KL_SYSTEM_MEDIA_ADD) {
		error = media_argument(bytes, size, &argument);
		if (error != 0)
			return error;
		command = KL_BACKEND_MEDIA_ADD;
	} else if (opcode == KL_SYSTEM_MEDIA_APPLY && size == 4U) {
		descriptor = kwl_take_fd(object->client);
		if (descriptor < 0)
			return EAGAIN;
		command = KL_BACKEND_MEDIA_APPLY;
	} else {
		return EPROTO;
	}

	/* Finds a slot that can retain both success and failure through output backpressure. */
	request = media_word(bytes, 0U);
	for (index = 0U; index < MEDIA_PENDING_MAX; index++) {
		if (media_state.pending[index].backend == 0U)
			break;
	}

	/* Refuses busy work using the same explicit errno result as a CLI failure. */
	if (index == MEDIA_PENDING_MAX) {
		if (descriptor >= 0)
			close(descriptor);
		payload[0] = request;
		payload[1] = EBUSY;
		error = kwl_emit(object->client, object->id, KL_SYSTEM_MEDIA_EVENT_DONE, payload, sizeof(payload));
		if (error != 0)
			return error;
		return 0;
	}

	/* Opens the per-session CLI backend before the first requested operation. */
	if (!media_state.opened) {
		media_state.opened = 1;
		media_state.backend = kl_backend_media_open();
		media_state.generation = 1U;
	}

	/* A sentinel backend id holds an immediate failure until the client can receive it. */
	pending = &media_state.pending[index];
	memset(pending, 0, sizeof(*pending));
	pending->object = object;
	pending->request = request;
	pending->command = command;
	pending->descriptor = -1;
	pending->backend = UINT32_MAX;
	error = kl_backend_media_request(media_state.backend, command, argument, descriptor, &pending->backend);
	if (descriptor >= 0)
		close(descriptor);
	if (error != 0) {
		pending->error = error;
		pending->ready = 1;
	}

	/* Succeeded: the request has a queued CLI operation or a retained error answer. */
	return 0;
}

/*
 * Collects CLI results, retries retained events and tells every subscribed app of changes.
 */
void
kwl_media_library_tick(
	struct kwl_server *server)
{
	/* The login compositor cannot operate the signed-in user's media library. */
	if (server->greeter || !media_state.opened)
		return;
	kl_backend_media_update(media_state.backend);

	/* Completes pending operations before publishing the latest generation. */
	media_answers(server);
	media_watchers(server);
}

/*
 * Detaches requests when their client object is destroyed, preserving backend ownership.
 */
void
kwl_media_library_gone(
	struct kwl_object *object)
{
	size_t index;

	/* A late result is reaped and closed without dereferencing a dead object. */
	for (index = 0U; index < MEDIA_PENDING_MAX; index++) {
		if (media_state.pending[index].object == object)
			media_state.pending[index].object = NULL;
	}
}

/*
 * Closes the CLI queue and every response retained by the compositor.
 */
void
kwl_media_library_close(
	void)
{
	size_t index;

	/* Only live pending slots can own a completed descriptor. */
	for (index = 0U; index < MEDIA_PENDING_MAX; index++) {
		if (media_state.pending[index].backend != 0U && media_state.pending[index].descriptor >= 0)
			close(media_state.pending[index].descriptor);
	}

	/* Stops owned children and releases the session endpoint through the OS backend. */
	kl_backend_media_close(media_state.backend);
	memset(&media_state, 0, sizeof(media_state));
}

/*
 * Routes a helper exit collected by Home to the backend that owns its response.
 */
int
kwl_media_library_child(
	int64_t child,
	int status)
{
	int owned;

	/* The backend preserves the CLI result even if Home collected the process first. */
	owned = kl_backend_media_child(media_state.backend, child, status);

	/* Succeeded: Home can distinguish its app exits from media helper exits. */
	return owned;
}

/* Reads one native-endian Wayland word. */
static uint32_t
media_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The compositor wire is aligned but copying also tolerates an unaligned caller. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the wire word has a host scalar representation. */
	return word;
}

/* Validates the one bounded path-list string carried by add. */
static int
media_argument(
	const unsigned char *bytes,
	size_t size,
	const char **argument)
{
	uint32_t length;
	size_t padded;
	const void *embedded;

	/* Requires request plus string length, followed by its complete padded contents. */
	if (size < 8U)
		return EPROTO;
	length = media_word(bytes, 4U);
	if (length == 0U || length > KL_SYSTEM_MEDIA_PATHS_MAX + 1U)
		return EPROTO;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (size != 8U + padded || bytes[8U + length - 1U] != '\0')
		return EPROTO;
	embedded = memchr(bytes + 8U, '\0', length - 1U);
	if (embedded != NULL)
		return EPROTO;
	*argument = (const char *)(bytes + 8U);

	/* Succeeded: a path list can be passed as data to the CLI's stdin. */
	return 0;
}

/* Matches finished helpers to their original objects and retries retained responses. */
static void
media_answers(
	struct kwl_server *server)
{
	struct media_pending *pending;
	uint32_t id;
	size_t index;
	int error;
	int descriptor;
	int taken;

	/* The state is process-local; object ownership already selects each client. */
	(void)server;

	/* Takes every completed helper, including responses whose client has gone. */
	for (;;) {
		taken = kl_backend_media_take(media_state.backend, &id, &error, &descriptor);
		if (!taken)
			break;
		for (index = 0U; index < MEDIA_PENDING_MAX; index++) {
			if (media_state.pending[index].backend == id)
				break;
		}

		/* An unmatched response owns only its descriptor. */
		if (index == MEDIA_PENDING_MAX) {
			if (descriptor >= 0)
				close(descriptor);
			continue;
		}

		/* The slot now owns the snapshot until both snapshot and done are queued. */
		pending = &media_state.pending[index];
		pending->descriptor = descriptor;
		pending->error = error;
		pending->ready = 1;
		if (pending->command != KL_BACKEND_MEDIA_LIST) {
			media_state.generation++;
			if (media_state.generation == 0U)
				media_state.generation = 1U;
		}
	}

	/* Retries only the unsent suffix; a successfully queued descriptor is never resent. */
	for (index = 0U; index < MEDIA_PENDING_MAX; index++) {
		if (media_state.pending[index].ready)
			media_deliver(&media_state.pending[index]);
	}
}

/* Queues one retained snapshot and result, respecting kwl_emit_fd's consuming ownership. */
static void
media_deliver(
	struct media_pending *pending)
{
	uint32_t payload[2];
	int copy;
	int error;

	/* A dead client's result is discarded only after the CLI has completed. */
	if (pending->object != NULL) {
		payload[0] = pending->request;
		if (pending->descriptor >= 0 && !pending->snapshot_sent) {
			copy = fcntl(pending->descriptor, F_DUPFD_CLOEXEC, 3);
			if (copy < 0)
				return;
			error = kwl_emit_fd(pending->object->client, pending->object->id, KL_SYSTEM_MEDIA_EVENT_SNAPSHOT, payload, 4U, copy);
			if (error != 0)
				return;
			pending->snapshot_sent = 1;
		}

		/* done follows the descriptor so a client observes one complete operation. */
		payload[1] = (uint32_t)pending->error;
		error = kwl_emit(pending->object->client, pending->object->id, KL_SYSTEM_MEDIA_EVENT_DONE, payload, sizeof(payload));
		if (error != 0)
			return;
	}

	/* The queued packet owns its duplicate; the retained original can now close. */
	if (pending->descriptor >= 0)
		close(pending->descriptor);
	memset(pending, 0, sizeof(*pending));
	pending->descriptor = -1;
}

/* Tells subscribed apps to requery after CLI changes, retrying slow readers. */
static void
media_watchers(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;
	int error;

	/* A generation is a wakeup, not a replacement for querying the latest database. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->kind != KWL_SYSTEM_MEDIA || object->dead || !object->media_watching)
				continue;
			if (object->media_generation == media_state.generation)
				continue;
			error = kwl_emit(client, object->id, KL_SYSTEM_MEDIA_EVENT_CHANGED, &media_state.generation, 4U);
			if (error == 0)
				object->media_generation = media_state.generation;
		}
	}
}
