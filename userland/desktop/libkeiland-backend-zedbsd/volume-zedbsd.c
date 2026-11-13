/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The removable media on zedBSD (ws132-p004): volumed's list, read from
 * /run/volumed.sock (plan/ws132/phase004/phase.md V2).  The connection says
 * HELLO and keeps the volumes volumed reports (VOLUME and GONE lines, each
 * batch ended by DONE); a mount or an eject is a line with a request
 * number, answered by a RESULT line.  A volumed that is not running is not
 * a failure: the connection is tried again every two seconds, and the list
 * is empty meanwhile.
 *
 * The connection is a thread's of its own (daemon-link-zedbsd.h): it
 * connects (which may wait), reads the lines and writes the requests the
 * event loop hands it.  The event loop's calls copy what the thread
 * published, number the requests and hand them over; none of them waits.
 * A request that cannot be written is answered by a result with its errno
 * value.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* volumed's socket (a host test builds with another path). */
#ifndef VOLUME_SOCKET_PATH
#define VOLUME_SOCKET_PATH	"/run/volumed.sock"
#endif

/* The wait before connecting again, the longest line, and the answers kept. */
#define VOLUME_RETRY_MS		2000U
#define VOLUME_LINE_MAX		512U
#define VOLUME_RESULTS_MAX	8U

/* One answer of volumed. */
struct volume_result {
	uint32_t request;
	int error;
	char user[64];
};

/* What a request handed to the thread asks. */
#define VOLUME_OP_MOUNT		1U
#define VOLUME_OP_EJECT		2U

/*
 * The thread's following of volumed: the connection (-1 while none), when
 * to connect again, the bytes of a line not ended yet, the volumes of the
 * last DONE and those coming before the next, the answers not published
 * yet, and what changed since the last publication
 * (KL_BACKEND_VOLUMES_CHANGED_* bits).
 *
 * Only the link's thread reads and writes it, from the link's start until
 * its stop; after the stop the record's close releases the connection.
 */
struct volume_link {
	int socket;
	uint64_t retry_ms;
	char input[VOLUME_LINE_MAX];
	size_t used;
	struct kl_backend_volume list[KL_BACKEND_VOLUMES_MAX];
	size_t count;
	struct kl_backend_volume next[KL_BACKEND_VOLUMES_MAX];
	size_t next_count;
	struct volume_result results[VOLUME_RESULTS_MAX];
	size_t result_count;
	unsigned changed;
};

/*
 * What the event loop sees of the following: the volumes of the last
 * complete report, whether a connection is there (requests are refused
 * without one), and the answers not taken yet, the oldest first.
 */
struct volume_view {
	struct kl_backend_volume list[KL_BACKEND_VOLUMES_MAX];
	size_t count;
	unsigned connected;
	struct volume_result results[VOLUME_RESULTS_MAX];
	size_t result_count;
};

/*
 * One following of volumed: the link (its thread and lock), the thread's
 * own record, the view the thread published with the changes the event
 * loop has not taken (both under the link's lock), the event loop's copy
 * of the view, and the event loop's number of the last request.
 *
 * It lives from kl_backend_volumes_open to kl_backend_volumes_close.
 */
struct kl_backend_volumes {
	struct kl_daemon_link link;
	struct volume_link inner;
	struct volume_view published;
	unsigned published_changed;
	struct volume_view view;
	uint32_t request;
};

/* One request handed to the thread: a mount or an eject of a volume, with its number. */
struct volume_op {
	struct kl_daemon_link_op op;
	unsigned kind;
	uint32_t request;
	char id[KL_BACKEND_VOLUME_ID_MAX];
};

static void volume_step(void *owner);
static size_t volume_poll_fill(void *owner, struct pollfd *descriptors, size_t capacity, int *timeout);
static int volume_publish(void *owner);

/* What volumed's connection does on its link's thread. */
static const struct kl_daemon_link_ops volume_link_ops = {
	volume_step,
	volume_poll_fill,
	volume_publish,
};

static void volume_op_run(void *owner, struct kl_daemon_link_op *op);
static int volume_connect(struct volume_link *volumes);
static void volume_drop(struct volume_link *volumes);
static void volume_line(struct volume_link *volumes, char *line);
static void volume_parse(char *line, struct kl_backend_volume *volume);
static void volume_unescape(const char *text, char *output, size_t size);
static void volume_gone(struct volume_link *volumes, const char *id);
static void volume_result(struct volume_link *volumes, const char *line);
static void volume_result_keep(struct volume_link *volumes, uint32_t request, int error, const char *user);
static int volume_ask(struct kl_backend_volumes *volumes, unsigned kind, const char *id, uint32_t *request);
static uint64_t volume_milliseconds(void);

/*
 * Starts following volumed; the link's thread connects at once, and again
 * two seconds after each connection that could not be made.
 */
struct kl_backend_volumes *
kl_backend_volumes_open(
	void)
{
	struct kl_backend_volumes *volumes;
	int error;

	/* The record, with no connection and no volume. */
	volumes = calloc(1U, sizeof(*volumes));
	if (volumes == NULL)
		return NULL;
	volumes->inner.socket = -1;

	/* The thread that connects, reads and writes. */
	error = kl_daemon_link_start(&volumes->link, "volumed", &volume_link_ops, volumes);
	if (error != 0) {
		free(volumes);
		return NULL;
	}

	/* Succeeded. */
	return volumes;
}

/*
 * Stops following volumed.
 */
void
kl_backend_volumes_close(
	struct kl_backend_volumes *volumes)
{
	/* Nothing was opened. */
	if (volumes == NULL)
		return;

	/* The thread ends; the connection is this thread's to close, then the record. */
	kl_daemon_link_stop(&volumes->link);
	if (volumes->inner.socket >= 0)
		(void)close(volumes->inner.socket);
	free(volumes);
}

/*
 * Takes what the link's thread found since the last update: the volumes
 * of the last complete report and the answers that came.
 */
int
kl_backend_volumes_update(
	struct kl_backend_volumes *volumes,
	unsigned *changed)
{
	size_t room;
	size_t moved;

	/* A record and somewhere to say what changed. */
	if (volumes == NULL || changed == NULL)
		return EINVAL;

	/* Copies the published view and moves its answers after the ones not taken yet (the newest that do not fit are dropped). */
	kl_daemon_link_lock(&volumes->link);

	memcpy(volumes->view.list, volumes->published.list, volumes->published.count * sizeof(volumes->view.list[0]));
	volumes->view.count = volumes->published.count;
	volumes->view.connected = volumes->published.connected;
	room = VOLUME_RESULTS_MAX - volumes->view.result_count;
	moved = volumes->published.result_count;
	if (moved > room)
		moved = room;
	memcpy(&volumes->view.results[volumes->view.result_count], volumes->published.results, moved * sizeof(volumes->view.results[0]));
	volumes->view.result_count += moved;
	volumes->published.result_count = 0U;
	*changed = volumes->published_changed;
	volumes->published_changed = 0U;

	kl_daemon_link_unlock(&volumes->link);

	/* Succeeded. */
	return 0;
}

/*
 * Copies the volumes of the last complete report.
 */
size_t
kl_backend_volumes_get(
	const struct kl_backend_volumes *volumes,
	struct kl_backend_volume *list,
	size_t capacity)
{
	size_t count;

	/* As many as fit. */
	if (volumes == NULL || list == NULL)
		return 0U;
	count = volumes->view.count;
	if (count > capacity)
		count = capacity;
	memcpy(list, volumes->view.list, count * sizeof(list[0]));

	/* Succeeded. */
	return count;
}

/*
 * Asks volumed to mount a volume.
 */
int
kl_backend_volumes_mount(
	struct kl_backend_volumes *volumes,
	const char *id,
	uint32_t *request)
{
	int error;

	/* The line, numbered, for the thread to write. */
	error = volume_ask(volumes, VOLUME_OP_MOUNT, id, request);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes as a result. */
	return 0;
}

/*
 * Asks volumed to eject a volume.
 */
int
kl_backend_volumes_eject(
	struct kl_backend_volumes *volumes,
	const char *id,
	uint32_t *request)
{
	int error;

	/* The line, numbered, for the thread to write. */
	error = volume_ask(volumes, VOLUME_OP_EJECT, id, request);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes as a result. */
	return 0;
}

/*
 * Takes the oldest answer.
 */
int
kl_backend_volumes_take_result(
	struct kl_backend_volumes *volumes,
	uint32_t *request,
	int *error,
	char *user,
	size_t size)
{
	/* None waiting. */
	if (volumes == NULL || volumes->view.result_count == 0U)
		return 0;

	/* The oldest, then the others move up. */
	*request = volumes->view.results[0].request;
	*error = volumes->view.results[0].error;
	if (user != NULL && size > 0U)
		(void)snprintf(user, size, "%s", volumes->view.results[0].user);
	volumes->view.result_count--;
	memmove(&volumes->view.results[0], &volumes->view.results[1], volumes->view.result_count * sizeof(volumes->view.results[0]));

	/* Succeeded: one answer. */
	return 1;
}

/* Steps the following on the link's thread: connects again when the wait is over, then reads each whole line. */
static void
volume_step(
	void *owner)
{
	struct kl_backend_volumes *record;
	struct volume_link *volumes;
	ssize_t count;
	uint64_t now;
	char *end;
	size_t length;
	int error;

	/* The thread's own part of the record. */
	record = owner;
	volumes = &record->inner;

	/* No connection: another try once the wait is over. */
	if (volumes->socket < 0) {
		now = volume_milliseconds();
		if (now < volumes->retry_ms)
			return;
		error = volume_connect(volumes);
		if (error != 0)
			return;
	}

	/* Every byte there now, line by line. */
	for (;;) {
		count = recv(volumes->socket, volumes->input + volumes->used, sizeof(volumes->input) - 1U - volumes->used, MSG_DONTWAIT);
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;
		if (count <= 0) {
			volume_drop(volumes);
			break;
		}

		/* Each whole line. */
		volumes->used += (size_t)count;
		volumes->input[volumes->used] = '\0';
		for (;;) {
			end = strchr(volumes->input, '\n');
			if (end == NULL)
				break;
			*end = '\0';
			volume_line(volumes, volumes->input);
			length = (size_t)(end + 1 - volumes->input);
			memmove(volumes->input, end + 1, volumes->used - length + 1U);
			volumes->used -= length;
		}

		/* A line longer than any is a broken connection. */
		if (volumes->used >= sizeof(volumes->input) - 1U) {
			volume_drop(volumes);
			break;
		}
	}
}

/* Gives the connection to wait for, and while there is none the wait until the next try. */
static size_t
volume_poll_fill(
	void *owner,
	struct pollfd *descriptors,
	size_t capacity,
	int *timeout)
{
	struct kl_backend_volumes *record;
	uint64_t now;

	/* The thread's own part of the record. */
	record = owner;

	/* No connection: the wait ends when the next try is due. */
	if (record->inner.socket < 0 || capacity == 0U) {
		now = volume_milliseconds();
		if (record->inner.retry_ms <= now)
			*timeout = 0;
		else if (record->inner.retry_ms - now < (uint64_t)*timeout)
			*timeout = (int)(record->inner.retry_ms - now);
		return 0U;
	}

	/* Succeeded: volumed's lines wake the thread. */
	descriptors[0].fd = record->inner.socket;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	return 1U;
}

/* Publishes the volumes and the answers to the event loop's side (under the link's lock); 1 when something changed. */
static int
volume_publish(
	void *owner)
{
	struct kl_backend_volumes *record;
	struct volume_link *volumes;
	unsigned connected;
	size_t room;
	size_t moved;

	/* The thread's record and whether requests can go. */
	record = owner;
	volumes = &record->inner;
	connected = 0U;
	if (volumes->socket >= 0)
		connected = 1U;

	/* Nothing new: no wake. */
	if (volumes->changed == 0U && connected == record->published.connected)
		return 0;

	/* The list, whether requests can go, and the answers after those not taken yet (the newest that do not fit are dropped). */
	memcpy(record->published.list, volumes->list, volumes->count * sizeof(record->published.list[0]));
	record->published.count = volumes->count;
	record->published.connected = connected;
	room = VOLUME_RESULTS_MAX - record->published.result_count;
	moved = volumes->result_count;
	if (moved > room)
		moved = room;
	memcpy(&record->published.results[record->published.result_count], volumes->results, moved * sizeof(volumes->results[0]));
	record->published.result_count += moved;
	volumes->result_count = 0U;
	record->published_changed |= volumes->changed;
	volumes->changed = 0U;

	/* Succeeded: the event loop has something to take. */
	return 1;
}

/* Writes a mount or an eject on the link's thread; one that cannot be written is answered with its errno value. */
static void
volume_op_run(
	void *owner,
	struct kl_daemon_link_op *op)
{
	struct kl_backend_volumes *record;
	struct volume_link *volumes;
	struct volume_op *request;
	const char *word;
	char line[96];
	ssize_t sent;
	int length;

	/* The thread's record and the request. */
	record = owner;
	volumes = &record->inner;
	request = (struct volume_op *)op;

	/* The connection went since the request was made. */
	if (volumes->socket < 0) {
		volume_result_keep(volumes, request->request, ENOTCONN, "");
		return;
	}

	/* The line, numbered by the event loop. */
	word = "MOUNT";
	if (request->kind == VOLUME_OP_EJECT)
		word = "EJECT";
	length = snprintf(line, sizeof(line), "%s %u %s\n", word, request->request, request->id);
	sent = send(volumes->socket, line, (size_t)length, MSG_DONTWAIT);
	if (sent != (ssize_t)length)
		volume_result_keep(volumes, request->request, EPIPE, "");
}

/* Connects to volumed and says HELLO; an error leaves the next try two seconds away. */
static int
volume_connect(
	struct volume_link *volumes)
{
	struct sockaddr_un address;
	ssize_t sent;
	int descriptor;
	int status;

	/* The next try, should this one fail. */
	volumes->retry_ms = volume_milliseconds() + VOLUME_RETRY_MS;

	/* The connection. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", VOLUME_SOCKET_PATH);
	status = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status != 0) {
		status = errno;
		(void)close(descriptor);
		return status;
	}

	/* The greeting; the list follows. */
	sent = send(descriptor, "HELLO 1\n", 8U, MSG_DONTWAIT);
	if (sent != 8) {
		(void)close(descriptor);
		return EPIPE;
	}

	/* Succeeded: connected, nothing read yet. */
	volumes->socket = descriptor;
	volumes->used = 0U;
	volumes->next_count = 0U;
	return 0;
}

/* Drops the connection: the list empties. */
static void
volume_drop(
	struct volume_link *volumes)
{
	/* The connection goes; the next try is two seconds away. */
	(void)close(volumes->socket);
	volumes->socket = -1;
	volumes->retry_ms = volume_milliseconds() + VOLUME_RETRY_MS;
	volumes->used = 0U;

	/* Without volumed there is no volume. */
	if (volumes->count != 0U)
		volumes->changed |= KL_BACKEND_VOLUMES_CHANGED_LIST;
	volumes->count = 0U;
	volumes->next_count = 0U;
}

/* Carries out one line of volumed. */
static void
volume_line(
	struct volume_link *volumes,
	char *line)
{
	struct kl_backend_volume volume;
	int same;

	/* A volume: added to the coming report, in place of one by its ID. */
	same = strncmp(line, "VOLUME ", 7U);
	if (same == 0) {
		memset(&volume, 0, sizeof(volume));
		volume_parse(line + 7, &volume);
		if (volume.id[0] == '\0')
			return;
		volume_gone(volumes, volume.id);
		if (volumes->next_count < KL_BACKEND_VOLUMES_MAX) {
			volumes->next[volumes->next_count] = volume;
			volumes->next_count++;
		}

		/* Kept for the DONE. */
		return;
	}

	/* A volume gone. */
	same = strncmp(line, "GONE id=", 8U);
	if (same == 0) {
		volume_gone(volumes, line + 8);
		return;
	}

	/* One state: the coming report becomes the list (it starts from it again for the next changes). */
	same = strcmp(line, "DONE");
	if (same == 0) {
		memcpy(volumes->list, volumes->next, volumes->next_count * sizeof(volumes->list[0]));
		volumes->count = volumes->next_count;
		volumes->changed |= KL_BACKEND_VOLUMES_CHANGED_LIST;
		return;
	}

	/* An answer. */
	same = strncmp(line, "RESULT ", 7U);
	if (same == 0)
		volume_result(volumes, line + 7);
}

/* Reads a VOLUME line's key=value words into a volume. */
static void
volume_parse(
	char *line,
	struct kl_backend_volume *volume)
{
	char *word;
	char *rest;
	char *value;
	int same;

	/* Each word, ended by a space or the line's end. */
	for (word = line;
	     word != NULL;
	     word = rest) {
		rest = strchr(word, ' ');
		if (rest != NULL) {
			*rest = '\0';
			rest++;
		}

		/* A key and its value. */
		value = strchr(word, '=');
		if (value == NULL)
			continue;
		*value = '\0';
		value++;

		/* The keys a volume has; others are passed over. */
		same = strcmp(word, "id");
		if (same == 0)
			(void)snprintf(volume->id, sizeof(volume->id), "%s", value);
		same = strcmp(word, "fs");
		if (same == 0)
			(void)snprintf(volume->fs, sizeof(volume->fs), "%s", value);
		same = strcmp(word, "label");
		if (same == 0)
			volume_unescape(value, volume->label, sizeof(volume->label));
		same = strcmp(word, "path");
		if (same == 0)
			volume_unescape(value, volume->path, sizeof(volume->path));
		same = strcmp(word, "size");
		if (same == 0)
			volume->bytes = strtoull(value, NULL, 10);
		same = strcmp(word, "new");
		if (same == 0)
			volume->fresh = (unsigned)strtoul(value, NULL, 10);
	}

	/* "-" is no path: the volume is not mounted. */
	same = strcmp(volume->path, "-");
	if (same == 0)
		volume->path[0] = '\0';
}

/* Turns a word's %xx back into bytes. */
static void
volume_unescape(
	const char *text,
	char *output,
	size_t size)
{
	char hex[3];
	size_t used;

	/* Each byte, or the byte of each %xx. */
	used = 0U;
	while (*text != '\0' && used + 1U < size) {
		if (text[0] == '%' && text[1] != '\0' && text[2] != '\0') {
			hex[0] = text[1];
			hex[1] = text[2];
			hex[2] = '\0';
			output[used] = (char)strtoul(hex, NULL, 16);
			text += 3;
		} else {
			output[used] = *text;
			text++;
		}

		/* One byte more. */
		used++;
	}

	/* Ended. */
	output[used] = '\0';
}

/* Takes a volume off the coming report. */
static void
volume_gone(
	struct volume_link *volumes,
	const char *id)
{
	size_t index;
	int same;

	/* The one of the ID, if there. */
	for (index = 0U; index < volumes->next_count; index++) {
		same = strcmp(volumes->next[index].id, id);
		if (same != 0)
			continue;
		volumes->next_count--;
		volumes->next[index] = volumes->next[volumes->next_count];
		return;
	}
}

/* Keeps an answer: "<request> <errno> [user=<program>]". */
static void
volume_result(
	struct volume_link *volumes,
	const char *line)
{
	const char *user;
	unsigned request;
	int error;
	int count;

	/* The request and its errno. */
	count = sscanf(line, "%u %d", &request, &error);
	if (count != 2)
		return;

	/* Kept, with the program that holds a busy volume. */
	user = strstr(line, " user=");
	if (user != NULL)
		user += 6;
	else
		user = "";
	volume_result_keep(volumes, request, error, user);
}

/* Keeps an answer for the next publication; one that does not fit is dropped. */
static void
volume_result_keep(
	struct volume_link *volumes,
	uint32_t request,
	int error,
	const char *user)
{
	struct volume_result *result;

	/* No room. */
	if (volumes->result_count >= VOLUME_RESULTS_MAX)
		return;

	/* The answer, told by the next update. */
	result = &volumes->results[volumes->result_count];
	volumes->result_count++;
	memset(result, 0, sizeof(*result));
	result->request = request;
	result->error = error;
	(void)snprintf(result->user, sizeof(result->user), "%s", user);
	volumes->changed |= KL_BACKEND_VOLUMES_CHANGED_RESULT;
}

/* Numbers a mount or an eject and hands it to the link's thread. */
static int
volume_ask(
	struct kl_backend_volumes *volumes,
	unsigned kind,
	const char *id,
	uint32_t *request)
{
	struct volume_op *ask;
	const char *space;
	int length;

	/* A connection, a volume's name of one word, and somewhere to put the number. */
	if (volumes == NULL || id == NULL || request == NULL || id[0] == '\0')
		return EINVAL;
	space = strpbrk(id, " \n");
	length = (int)strnlen(id, KL_BACKEND_VOLUME_ID_MAX);
	if (space != NULL || length >= (int)KL_BACKEND_VOLUME_ID_MAX)
		return EINVAL;
	if (!volumes->view.connected)
		return ENOTCONN;

	/* The request, the link's from the submit on. */
	ask = calloc(1U, sizeof(*ask));
	if (ask == NULL)
		return ENOMEM;

	/* The number the answer will carry (never 0). */
	volumes->request++;
	if (volumes->request == 0U)
		volumes->request = 1U;
	ask->op.run = volume_op_run;
	ask->kind = kind;
	ask->request = volumes->request;
	(void)snprintf(ask->id, sizeof(ask->id), "%s", id);
	kl_daemon_link_submit(&volumes->link, &ask->op);

	/* Succeeded: the answer will carry the number. */
	*request = volumes->request;
	return 0;
}

/* The monotonic time in milliseconds. */
static uint64_t
volume_milliseconds(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
