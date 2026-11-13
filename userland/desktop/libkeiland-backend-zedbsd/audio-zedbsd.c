/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound output's volume through audiod (ws100-p003, plan/ws100/
 * design.md section 3.3): one connection that says HELLO and SUBSCRIBE,
 * reads audiod's VOLUME_CHANGED reports as they come, and sends
 * DEVICE_VOLUME and FEEDBACK requests.  audiod's protocol is
 * userland/base/audiod/protocol.h.
 *
 * The connection is a thread's of its own (daemon-link-zedbsd.h): it
 * connects (which may wait), reads the reports as they come and sends the
 * requests the event loop hands it.  The event loop's calls copy what the
 * thread published and hand requests over; none of them waits.  A request
 * that cannot be written whole drops the connection (the thread connects
 * again a second later), and the answers (DONE, ERROR) are read and passed
 * over.  audiod sends each message whole; the bytes are kept and cut by the
 * headers' length.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.h"

#include "userland/base/audiod/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* audiod's socket (a host test builds with another path; plan/ws100/tests/host-audio.sh). */
#ifndef AUDIO_SOCKET_PATH
#define AUDIO_SOCKET_PATH AUDIOD_SOCKET_PATH
#endif

/* The wait before connecting again, in milliseconds. */
#define AUDIO_RETRY_MS 1000U

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The bytes kept of messages not read whole yet. */
#define AUDIO_PENDING (AUDIOD_MESSAGE_MAX * 8U)

/* The subscription's mask: the device volume. */
#define AUDIO_SUBSCRIBE_VOLUME 1U

/* What a request handed to the thread asks. */
#define AUDIO_OP_VOLUME		1U
#define AUDIO_OP_FEEDBACK	2U

/*
 * The thread's following of audiod: the connection (-1 while none), the
 * serial of the next request, when to connect again, the bytes of messages
 * not read whole, what audiod reported, and what changed since the last
 * publication (KL_BACKEND_AUDIO_CHANGED_* bits).
 *
 * Only the link's thread reads and writes it, from the link's start until
 * its stop; after the stop the record's close releases the connection.
 */
struct audio_link {
	int socket;
	uint32_t serial;
	uint64_t retry_ms;
	uint8_t pending[AUDIO_PENDING];
	size_t pending_used;
	struct kl_backend_audio_state state;
	unsigned changed;
};

/*
 * What the event loop sees of the following: audiod's state as reported,
 * and whether a connection is there (requests are refused without one).
 */
struct audio_view {
	struct kl_backend_audio_state state;
	unsigned connected;
};

/*
 * One following of audiod: the link (its thread and lock), the thread's
 * own record, the view the thread published with the changes the event
 * loop has not taken (both under the link's lock), and the event loop's
 * copy of the view, which its getters read.
 *
 * The subscriber owns this allocation from open until close.
 */
struct kl_backend_audio {
	struct kl_daemon_link link;
	struct audio_link inner;
	struct audio_view published;
	unsigned published_changed;
	struct audio_view view;
};

/*
 * One request handed to the thread: a device volume (the levels and the
 * mute) or the feedback sound.
 */
struct audio_op {
	struct kl_daemon_link_op op;
	unsigned kind;
	unsigned left;
	unsigned right;
	unsigned muted;
};

static void audio_step(void *owner);
static size_t audio_poll_fill(void *owner, struct pollfd *descriptors, size_t capacity, int *timeout);
static int audio_publish(void *owner);

/* What audiod's connection does on its link's thread. */
static const struct kl_daemon_link_ops audio_link_ops = {
	audio_step,
	audio_poll_fill,
	audio_publish,
};

static void audio_op_run(void *owner, struct kl_daemon_link_op *op);
static int audio_submit(struct kl_backend_audio *audio, unsigned kind, unsigned left, unsigned right, unsigned muted);
static int audio_connect(struct audio_link *audio);
static void audio_drop(struct audio_link *audio);
static int audio_send(struct audio_link *audio, const void *message, uint32_t length);
static void audio_read(struct audio_link *audio);
static void audio_message(struct audio_link *audio, const uint8_t *bytes, uint32_t length);
static uint64_t audio_milliseconds(void);

/*
 * Starts following audiod's volume; the link's thread connects at once,
 * and again a second after each connection that could not be made.
 */
struct kl_backend_audio *
kl_backend_audio_open(
	void)
{
	struct kl_backend_audio *audio;
	int error;

	/* The record, with no connection. */
	audio = calloc(1, sizeof(*audio));
	if (audio == NULL)
		return NULL;

	/* The thread's following starts without a connection, due at once. */
	audio->inner.socket = -1;
	audio->inner.serial = 1U;

	/* The thread that connects, reads and sends. */
	error = kl_daemon_link_start(&audio->link, "audiod", &audio_link_ops, audio);
	if (error != 0) {
		free(audio);
		return NULL;
	}

	/* Succeeded: the record exists, connected or not. */
	return audio;
}

/*
 * Stops following audiod.
 */
void
kl_backend_audio_close(
	struct kl_backend_audio *audio)
{
	/* Nothing to close. */
	if (audio == NULL)
		return;

	/* The thread ends; the connection is this thread's to close. */
	kl_daemon_link_stop(&audio->link);
	if (audio->inner.socket >= 0)
		(void)close(audio->inner.socket);

	/* Releases the subscription after its owned socket is closed. */
	free(audio);

	/* Succeeded: the subscription and its connection are released. */
	return;
}

/*
 * Gives the descriptor to poll: none, since the link's thread reads the
 * connection and wakes the event loop itself.
 */
int
kl_backend_audio_fd(
	const struct kl_backend_audio *audio)
{
	UNUSED_PARAMETER(audio);

	/* Succeeded: nothing for the event loop to poll. */
	return -1;
}

/*
 * Takes what the link's thread found since the last update: the state
 * audiod reported and what changed.
 */
int
kl_backend_audio_update(
	struct kl_backend_audio *audio,
	unsigned *changed)
{
	/* Nothing has changed yet. */
	*changed = 0U;

	/* Refuses to update or request audio without a subscription record. */
	if (audio == NULL)
		return EINVAL;

	/* Copies the published view and takes its changes. */
	kl_daemon_link_lock(&audio->link);

	audio->view = audio->published;
	*changed = audio->published_changed;
	audio->published_changed = 0U;

	kl_daemon_link_unlock(&audio->link);

	/* Succeeded: *changed says what the thread found. */
	return 0;
}

/*
 * Copies what audiod last reported.
 */
void
kl_backend_audio_get_state(
	const struct kl_backend_audio *audio,
	struct kl_backend_audio_state *state)
{
	/* A missing record knows nothing. */
	memset(state, 0, sizeof(*state));

	/* Leaves the empty output when no subscription exists. */
	if (audio == NULL)
		return;

	/* The state as the last update took it. */
	*state = audio->view.state;

	/* Succeeded: the caller holds the last reported audio state. */
	return;
}

/*
 * Asks audiod for a volume and mute (DEVICE_VOLUME); the thread sends it.
 */
int
kl_backend_audio_set_volume(
	struct kl_backend_audio *audio,
	unsigned left,
	unsigned right,
	unsigned muted)
{
	int error;

	/* A volume within range, on a connection. */
	if (audio == NULL ||
	    left > 100U ||
	    right > 100U ||
	    muted > 1U)
		return EINVAL;

	/* Hands the request to the thread. */
	error = audio_submit(audio, AUDIO_OP_VOLUME, left, right, muted);
	if (error != 0)
		return error;

	/* Succeeded: the new volume comes back as a report. */
	return 0;
}

/*
 * Asks audiod for its short feedback sound (FEEDBACK); the thread sends it.
 */
int
kl_backend_audio_feedback(
	struct kl_backend_audio *audio)
{
	int error;

	/* Refuses to update or request audio without a subscription record. */
	if (audio == NULL)
		return EINVAL;

	/* Hands the request to the thread. */
	error = audio_submit(audio, AUDIO_OP_FEEDBACK, 0U, 0U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the sound plays from audiod's next period. */
	return 0;
}

/*
 * Tells whether audiod runs: 1 when its socket is there, 0 when it is not.
 *
 * Nothing connects and nothing waits; an audiod that runs may still have no
 * sound device (kl_backend_audio_state's device).
 */
int
kl_backend_audio_available(
	void)
{
	struct stat status;
	int error;

	/* audiod's socket; one that is not there is audiod not running. */
	error = stat(AUDIO_SOCKET_PATH, &status);
	if (error != 0)
		return 0;

	/* Only a socket counts. */
	if ((status.st_mode & S_IFMT) != S_IFSOCK)
		return 0;

	/* Succeeded: audiod's socket is present. */
	return 1;
}

/* Steps the following on the link's thread: connects again when the wait is over, then reads what came. */
static void
audio_step(
	void *owner)
{
	struct kl_backend_audio *record;
	struct audio_link *audio;
	uint64_t now;
	int error;

	/* The thread's own part of the record. */
	record = owner;
	audio = &record->inner;

	/* A lost connection is made again when its wait is over. */
	if (audio->socket < 0) {
		/* Retries only after the previous connection attempt's delay. */
		now = audio_milliseconds();
		if (now >= audio->retry_ms) {
			error = audio_connect(audio);
			if (error == 0)
				audio->changed |= KL_BACKEND_AUDIO_CHANGED_REACHABLE;
		}
	}

	/* The messages that have come. */
	if (audio->socket >= 0)
		audio_read(audio);
}

/* Gives the connection to wait for, and while there is none the wait until the next try. */
static size_t
audio_poll_fill(
	void *owner,
	struct pollfd *descriptors,
	size_t capacity,
	int *timeout)
{
	struct kl_backend_audio *record;
	uint64_t now;

	/* The thread's own part of the record. */
	record = owner;

	/* No connection: the wait ends when the next try is due. */
	if (record->inner.socket < 0 || capacity == 0U) {
		now = audio_milliseconds();
		if (record->inner.retry_ms <= now)
			*timeout = 0;
		else if (record->inner.retry_ms - now < (uint64_t)*timeout)
			*timeout = (int)(record->inner.retry_ms - now);
		return 0U;
	}

	/* Succeeded: the reports wake the thread. */
	descriptors[0].fd = record->inner.socket;
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	return 1U;
}

/* Publishes the state and the changes to the event loop's side (under the link's lock); 1 when something changed. */
static int
audio_publish(
	void *owner)
{
	struct kl_backend_audio *record;
	unsigned connected;

	/* The thread's record and what the event loop will see of it. */
	record = owner;
	connected = 0U;
	if (record->inner.socket >= 0)
		connected = 1U;

	/* Nothing new: no wake. */
	if (record->inner.changed == 0U && connected == record->published.connected)
		return 0;

	/* The state, whether requests can go, and the changes not taken yet. */
	record->published.state = record->inner.state;
	record->published.connected = connected;
	record->published_changed |= record->inner.changed;
	record->inner.changed = 0U;

	/* Succeeded: the event loop has something to take. */
	return 1;
}

/* Sends a request on the link's thread; one made while no connection is there is passed over. */
static void
audio_op_run(
	void *owner,
	struct kl_daemon_link_op *op)
{
	struct kl_backend_audio *record;
	struct audiod_volume volume;
	struct audiod_header header;
	struct audio_op *request;
	struct audio_link *audio;

	/* The thread's record and the request. */
	record = owner;
	audio = &record->inner;
	request = (struct audio_op *)op;

	/* The connection went since the request was made: the volume shown follows audiod's next report. */
	if (audio->socket < 0)
		return;

	/* DEVICE_VOLUME; a failed send drops the connection, which the next update says. */
	if (request->kind == AUDIO_OP_VOLUME) {
		memset(&volume, 0, sizeof(volume));
		volume.header.type = AUDIOD_DEVICE_VOLUME;
		volume.header.length = sizeof(volume);
		volume.header.serial = audio->serial++;
		volume.left = request->left;
		volume.right = request->right;
		volume.muted = request->muted;
		(void)audio_send(audio, &volume, sizeof(volume));
		return;
	}

	/* FEEDBACK, the header only (an older audiod answers ERROR, which is passed over). */
	memset(&header, 0, sizeof(header));
	header.type = AUDIOD_FEEDBACK;
	header.length = sizeof(header);
	header.serial = audio->serial++;
	(void)audio_send(audio, &header, sizeof(header));
}

/* Hands a request to the link's thread; 0, ENOTCONN (no connection as last published) or ENOMEM. */
static int
audio_submit(
	struct kl_backend_audio *audio,
	unsigned kind,
	unsigned left,
	unsigned right,
	unsigned muted)
{
	struct audio_op *request;

	/* Refuses a request while the daemon connection is absent. */
	if (!audio->view.connected)
		return ENOTCONN;

	/* The request, the link's from the submit on. */
	request = calloc(1U, sizeof(*request));
	if (request == NULL)
		return ENOMEM;
	request->op.run = audio_op_run;
	request->kind = kind;
	request->left = left;
	request->right = right;
	request->muted = muted;
	kl_daemon_link_submit(&audio->link, &request->op);

	/* Succeeded: the thread sends it before its next read. */
	return 0;
}

/* Connects to audiod, says HELLO and SUBSCRIBE; returns 0 or an errno value. */
static int
audio_connect(
	struct audio_link *audio)
{
	struct sockaddr_un address;
	struct audiod_hello hello;
	struct audiod_subscribe subscribe;
	int descriptor;
	int flags;
	int error;

	/* Whatever happens, the next try waits a while. */
	audio->retry_ms = audio_milliseconds() + AUDIO_RETRY_MS;

	/* A stream socket that the programs the desktop starts do not inherit. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return errno;

	/* audiod's socket; one that is not there is audiod not running. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", AUDIO_SOCKET_PATH);
	error = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* From now on nothing waits. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags >= 0)
		(void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);

	/* Publishes the connection with no partially received message. */
	audio->socket = descriptor;
	audio->pending_used = 0U;

	/* HELLO: audiod answers WELCOME with the device. */
	memset(&hello, 0, sizeof(hello));
	hello.header.type = AUDIOD_HELLO;
	hello.header.length = sizeof(hello);
	hello.header.serial = audio->serial++;
	hello.version = AUDIOD_PROTOCOL_VERSION_1;
	error = audio_send(audio, &hello, sizeof(hello));
	if (error != 0)
		return error;

	/* SUBSCRIBE: audiod answers the volume as it stands, and each change. */
	memset(&subscribe, 0, sizeof(subscribe));
	subscribe.header.type = AUDIOD_SUBSCRIBE;
	subscribe.header.length = sizeof(subscribe);
	subscribe.header.serial = audio->serial++;
	subscribe.mask = AUDIO_SUBSCRIBE_VOLUME;
	error = audio_send(audio, &subscribe, sizeof(subscribe));
	if (error != 0)
		return error;

	/* Succeeded: connected; reachable once WELCOME comes. */
	return 0;
}

/* Drops the connection; the next step after the wait connects again. */
static void
audio_drop(
	struct audio_link *audio)
{
	/* The connection. */
	if (audio->socket >= 0)
		(void)close(audio->socket);

	/* Retires pending input and postpones the next connection attempt. */
	audio->socket = -1;
	audio->pending_used = 0U;
	audio->retry_ms = audio_milliseconds() + AUDIO_RETRY_MS;

	/* No longer reachable: said once. */
	if (audio->state.reachable) {
		audio->state.reachable = 0U;
		audio->state.device = 0U;
		audio->changed |= KL_BACKEND_AUDIO_CHANGED_REACHABLE;
	}

	/* Succeeded: the subscription waits to reconnect with empty pending input. */
	return;
}

/* Writes one whole request; a connection that cannot take it is dropped. Returns 0 or an errno value. */
static int
audio_send(
	struct audio_link *audio,
	const void *message,
	uint32_t length)
{
	ssize_t sent;
	int error;

	/* Sends the complete small request without raising SIGPIPE. */
	sent = send(audio->socket, message, length, MSG_NOSIGNAL);
	if (sent != (ssize_t)length) {
		/* Captures the write failure before dropping the unusable connection. */
		error = EIO;
		if (sent < 0)
			error = errno;
		audio_drop(audio);
		return error;
	}

	/* Succeeded: the daemon received the complete request. */
	return 0;
}

/* Reads the bytes that have come and handles each whole message. */
static void
audio_read(
	struct audio_link *audio)
{
	struct audiod_header header;
	ssize_t count;

	/* Until nothing more is waiting. */
	for (;;) {
		/* Bytes, as many as there is room for; none waiting ends the read. */
		count = recv(audio->socket, audio->pending + audio->pending_used, sizeof(audio->pending) - audio->pending_used, 0);
		if (count < 0 &&
		    (errno == EAGAIN ||
		    errno == EWOULDBLOCK))
			break;

		/* A closed or failed stream is replaced on a later update. */
		if (count <= 0) {
			audio_drop(audio);
			return;
		}

		/* Kept after what came before. */
		audio->pending_used += (size_t)count;

		/* Each whole message kept. */
		while (audio->pending_used >= sizeof(header)) {
			/* Rejects a header that cannot frame a valid daemon message. */
			memcpy(&header, audio->pending, sizeof(header));
			if (header.length < sizeof(header) || header.length > AUDIOD_MESSAGE_MAX) {
				/* A broken stream: connect again. */
				audio_drop(audio);
				return;
			}

			/* A message not whole yet waits for more bytes. */
			if (audio->pending_used < header.length)
				break;

			/* The message, then the bytes after it. */
			audio_message(audio, audio->pending, header.length);
			audio->pending_used -= header.length;
			memmove(audio->pending, audio->pending + header.length, audio->pending_used);
		}
	}

	/* Succeeded: all complete messages currently available have been applied. */
	return;
}

/* Handles one message from audiod: WELCOME and VOLUME_CHANGED change the state, the rest is passed over. */
static void
audio_message(
	struct audio_link *audio,
	const uint8_t *bytes,
	uint32_t length)
{
	struct audiod_welcome welcome;
	struct audiod_volume volume;
	struct audiod_header header;

	/* Its type. */
	memcpy(&header, bytes, sizeof(header));

	/* WELCOME: reachable, with the device. */
	if (header.type == AUDIOD_WELCOME && length >= sizeof(welcome)) {
		memcpy(&welcome, bytes, sizeof(welcome));
		audio->state.reachable = 1U;
		audio->state.device = 0U;

		/* Marks a reported output device as available. */
		if (welcome.device != 0U)
			audio->state.device = 1U;

		/* Keeps the format reported by the daemon. */
		audio->state.rate = welcome.rate;
		audio->state.channels = welcome.channels;
		audio->changed |= KL_BACKEND_AUDIO_CHANGED_REACHABLE;
		return;
	}

	/* VOLUME_CHANGED: the volume as it stands. */
	if (header.type == AUDIOD_VOLUME_CHANGED && length >= sizeof(volume)) {
		memcpy(&volume, bytes, sizeof(volume));
		audio->state.left = volume.left;
		audio->state.right = volume.right;
		audio->state.muted = 0U;

		/* Marks a muted output independently of its retained volume. */
		if (volume.muted != 0U)
			audio->state.muted = 1U;

		/* Notifies the subscriber that its visible volume state changed. */
		audio->changed |= KL_BACKEND_AUDIO_CHANGED_VOLUME;
	}

	/* Succeeded: the report is applied or its unneeded type is passed over. */
	return;
}

/* Gives the monotonic clock in milliseconds (0 when it cannot be read). */
static uint64_t
audio_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* The clock; one that fails reads as the start of time. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: seconds and nanoseconds as milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
