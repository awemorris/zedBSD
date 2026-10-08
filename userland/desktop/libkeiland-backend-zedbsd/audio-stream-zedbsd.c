/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams through audiod (keiland-backend.h, WS191,
 * plan/ws191/design.md): one connection to audiod for each stream, as an
 * application of its own had before.  The connection says HELLO, makes
 * one playback stream and hands its shared memory over as the ring; the
 * controls go to audiod as its requests, and its answers and events come
 * back as the stream's reports.  audiod's protocol is
 * userland/base/audiod/protocol.h.
 *
 * Nothing waits: the socket does not block, and every step (the
 * connection, the greeting, the stream, the answers) is taken by
 * kl_backend_audio_stream_next as it comes.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include "userland/base/audiod/protocol.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* audiod's socket (a host test builds with another path; plan/ws191/tests/host-audio-stream.sh). */
#ifndef AUDIO_SOCKET_PATH
#define AUDIO_SOCKET_PATH AUDIOD_SOCKET_PATH
#endif

/* The stream's number on its own connection (one stream a connection). */
#define STREAM_NUMBER		1U

/* The bytes kept of messages not read whole yet. */
#define STREAM_PENDING		(AUDIOD_MESSAGE_MAX * 8U)

/* The controls sent and not answered yet, and the reports not taken yet. */
#define STREAM_WAITING		16U
#define STREAM_REPORTS		32U

/* audiod's ring is the Keiland ring: the same layout, version and formats. */
_Static_assert(sizeof(struct audiod_shm_header) == sizeof(struct kl_backend_audio_ring), "the ring's header");
_Static_assert(offsetof(struct audiod_shm_header, version) == offsetof(struct kl_backend_audio_ring, version), "version");
_Static_assert(offsetof(struct audiod_shm_header, format) == offsetof(struct kl_backend_audio_ring, format), "format");
_Static_assert(offsetof(struct audiod_shm_header, channels) == offsetof(struct kl_backend_audio_ring, channels), "channels");
_Static_assert(offsetof(struct audiod_shm_header, rate) == offsetof(struct kl_backend_audio_ring, rate), "rate");
_Static_assert(offsetof(struct audiod_shm_header, frame_bytes) == offsetof(struct kl_backend_audio_ring, frame_bytes), "frame_bytes");
_Static_assert(offsetof(struct audiod_shm_header, capacity_frames) == offsetof(struct kl_backend_audio_ring, capacity_frames), "capacity");
_Static_assert(offsetof(struct audiod_shm_header, period_frames) == offsetof(struct kl_backend_audio_ring, period_frames), "period");
_Static_assert(offsetof(struct audiod_shm_header, write_position) == offsetof(struct kl_backend_audio_ring, write_position), "write");
_Static_assert(offsetof(struct audiod_shm_header, read_position) == offsetof(struct kl_backend_audio_ring, read_position), "read");
_Static_assert(offsetof(struct audiod_shm_header, played_position) == offsetof(struct kl_backend_audio_ring, played_position), "played");
_Static_assert(offsetof(struct audiod_shm_header, played_time_ns) == offsetof(struct kl_backend_audio_ring, played_time_ns), "played time");
_Static_assert(offsetof(struct audiod_shm_header, played_sequence) == offsetof(struct kl_backend_audio_ring, played_sequence), "played sequence");
_Static_assert(offsetof(struct audiod_shm_header, write_sequence) == offsetof(struct kl_backend_audio_ring, write_sequence), "write sequence");
_Static_assert(offsetof(struct audiod_shm_header, read_sequence) == offsetof(struct kl_backend_audio_ring, read_sequence), "read sequence");
_Static_assert(offsetof(struct audiod_shm_header, underruns) == offsetof(struct kl_backend_audio_ring, underruns), "underruns");
_Static_assert(offsetof(struct audiod_shm_header, overruns) == offsetof(struct kl_backend_audio_ring, overruns), "overruns");
_Static_assert(offsetof(struct audiod_shm_header, state) == offsetof(struct kl_backend_audio_ring, state), "state");
_Static_assert(AUDIOD_SHM_HEADER == KL_BACKEND_AUDIO_RING_HEADER, "the ring's page");
_Static_assert(AUDIOD_VERSION == KL_BACKEND_AUDIO_RING_VERSION, "the ring's version is audiod's");
_Static_assert(AUDIOD_FORMAT_S16_LE == KL_BACKEND_AUDIO_FORMAT_S16_LE, "S16_LE");
_Static_assert(AUDIOD_FORMAT_S32_LE == KL_BACKEND_AUDIO_FORMAT_S32_LE, "S32_LE");
_Static_assert(AUDIOD_FORMAT_F32_LE == KL_BACKEND_AUDIO_FORMAT_F32_LE, "F32_LE");
_Static_assert(AUDIOD_STATE_RUNNING == KL_BACKEND_AUDIO_STATE_RUNNING, "running");

/* How far a stream has come. */
enum stream_stage {
	STREAM_CONNECTING,	/* the connection is being made */
	STREAM_GREETING,	/* HELLO sent, WELCOME awaited */
	STREAM_CREATING,	/* STREAM_CREATE sent, STREAM_CREATED awaited */
	STREAM_READY,		/* the ring is the client's; controls go to audiod */
	STREAM_ENDED		/* FAILED or LOST was reported; nothing more comes */
};

/*
 * A control sent to audiod and not answered yet: the serials audiod
 * answers with (two for the flush of a drain, which is STOP then FLUSH),
 * how many answers are still to come, the first error among them, and the
 * client's request they answer together.  used 0 is a free slot.
 */
struct stream_waiting {
	unsigned used;
	uint32_t serials[2];
	unsigned remaining;
	unsigned error;
	uint32_t request;
};

/*
 * One playback stream and its own connection to audiod.
 *
 * The compositor owns it from kl_backend_audio_stream_open to
 * kl_backend_audio_stream_close; only the compositor's thread touches it.
 * socket is -1 once the stream ended.  The reports are a ring the steps
 * fill and kl_backend_audio_stream_next empties; a ring that is full drops
 * nothing that matters, since only the controls' answers (at most
 * STREAM_WAITING of them), one drain and one count of underruns wait in it.
 */
struct kl_backend_audio_stream {
	int socket;
	enum stream_stage stage;
	struct kl_backend_audio_stream_format format;
	uint32_t next_serial;

	/* The controls not answered yet, and the drain under way (DRAIN has no DONE, only DRAINED). */
	struct stream_waiting waiting[STREAM_WAITING];
	unsigned draining;
	uint32_t drain_serial;
	uint32_t drain_request;

	/* The underruns audiod counted, and the count last reported. */
	uint32_t underruns;
	uint32_t underruns_told;

	/* The bytes of messages not read whole, and a descriptor that came with them. */
	unsigned char pending[STREAM_PENDING];
	size_t pending_used;
	int pending_fd;

	/* The reports not taken yet. */
	struct kl_backend_audio_stream_report reports[STREAM_REPORTS];
	unsigned report_first;
	unsigned report_count;
};

static void stream_step(struct kl_backend_audio_stream *stream);
static void stream_connected(struct kl_backend_audio_stream *stream);
static void stream_read(struct kl_backend_audio_stream *stream);
static int stream_receive(struct kl_backend_audio_stream *stream);
static void stream_message(struct kl_backend_audio_stream *stream, const unsigned char *bytes, uint32_t length);
static void stream_answer(struct kl_backend_audio_stream *stream, uint32_t serial, unsigned error);
static int stream_drain(struct kl_backend_audio_stream *stream, uint32_t request);
static int stream_request(struct kl_backend_audio_stream *stream, uint32_t type, struct stream_waiting *waiting);
static uint32_t stream_type(unsigned what);
static int stream_send(struct kl_backend_audio_stream *stream, const void *message, uint32_t length);
static void stream_end(struct kl_backend_audio_stream *stream, unsigned what, unsigned error);
static void stream_report(struct kl_backend_audio_stream *stream, const struct kl_backend_audio_stream_report *report);
static unsigned stream_error(uint32_t error);

/*
 * Tells whether streams can be made: audiod's protocol is always there on
 * zedBSD (whether audiod runs is a stream's own FAILED).
 */
int
kl_backend_audio_stream_supported(
	void)
{
	/* Succeeded: zedBSD's backend makes streams. */
	return 1;
}

/*
 * Starts making a stream: the connection to audiod, without waiting.
 */
struct kl_backend_audio_stream *
kl_backend_audio_stream_open(
	const struct kl_backend_audio_stream_format *format)
{
	struct kl_backend_audio_stream *stream;
	struct sockaddr_un address;
	int status;

	/* The record. */
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* What is asked for, nothing sent yet, no descriptor come. */
	stream->format = *format;
	stream->next_serial = 1U;
	stream->pending_fd = -1;
	stream->stage = STREAM_CONNECTING;

	/* A stream socket that does not block, and that the programs the desktop starts do not inherit. */
	stream->socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (stream->socket < 0) {
		stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_NO_MEMORY);
		return stream;
	}

	/* audiod's socket: connected at once, or soon (EINPROGRESS elsewhere); anything else (EAGAIN: its backlog is full; ENOENT: it does not run) is audiod not there. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", AUDIO_SOCKET_PATH);
	status = connect(stream->socket, (struct sockaddr *)&address, sizeof(address));
	if (status != 0 && errno != EINPROGRESS) {
		stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_UNAVAILABLE);
		return stream;
	}

	/* Connected at once: the greeting goes now. */
	if (status == 0)
		stream_connected(stream);

	/* Succeeded: the stream is on its way; its outcome comes as a report. */
	return stream;
}

/*
 * Sends a control to audiod; its answer comes as a RESULT.
 */
int
kl_backend_audio_stream_control(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	uint32_t request)
{
	struct kl_backend_audio_stream_report report;
	struct stream_waiting *waiting;
	unsigned slot;
	int error;

	/* Only a ready stream takes controls. */
	if (stream->stage != STREAM_READY)
		return ENOTCONN;

	/* A drain is answered at once: audiod sends no DONE for it. */
	if (what == KL_BACKEND_AUDIO_DRAIN) {
		error = stream_drain(stream, request);
		if (error != 0)
			return error;

		/* The answer the compositor sends on. */
		memset(&report, 0, sizeof(report));
		report.what = KL_BACKEND_AUDIO_RESULT;
		report.error = KL_BACKEND_AUDIO_ERROR_NONE;
		report.request = request;
		report.fd = -1;
		stream_report(stream, &report);
		return 0;
	}

	/* A free slot for the answer; none is audiod not keeping up. */
	for (slot = 0U; slot < STREAM_WAITING; slot++) {
		if (!stream->waiting[slot].used)
			break;
	}

	/* Every slot waits for an answer. */
	if (slot == STREAM_WAITING)
		return EAGAIN;

	/* The slot, filled as the requests go. */
	waiting = &stream->waiting[slot];
	memset(waiting, 0, sizeof(*waiting));
	waiting->request = request;

	/*
	 * The flush of a drain under way ends the drain: STOP first (audiod's
	 * FLUSH leaves a drain running), then FLUSH, one answer for both.
	 */
	if (what == KL_BACKEND_AUDIO_FLUSH && stream->draining) {
		error = stream_request(stream, AUDIOD_STREAM_STOP, waiting);
		if (error != 0)
			return error;
	}

	/* The control itself. */
	error = stream_request(stream, stream_type(what), waiting);
	if (error == EAGAIN && waiting->remaining != 0U) {
		/* The STOP went and the FLUSH did not: its answer says the flush was not done. */
		waiting->error = KL_BACKEND_AUDIO_ERROR_UNAVAILABLE;
		error = 0;
	}

	/* Not sent: nothing waits. */
	if (error != 0)
		return error;

	/* No drain is under way after a control (a flush of one ended it): a DRAINED of it is passed over. */
	stream->draining = 0U;

	/* Succeeded: the answers are awaited under their serials. */
	waiting->used = 1U;
	return 0;
}

/*
 * Takes the next thing that came of the stream, after reading what audiod
 * has sent.
 */
int
kl_backend_audio_stream_next(
	struct kl_backend_audio_stream *stream,
	struct kl_backend_audio_stream_report *report)
{
	/* What audiod has sent since the last look. */
	stream_step(stream);

	/* Nothing waits to be taken. */
	if (stream->report_count == 0U)
		return 0;

	/* The oldest report. */
	*report = stream->reports[stream->report_first];
	stream->report_first = (stream->report_first + 1U) % STREAM_REPORTS;
	stream->report_count--;

	/* Succeeded: *report is the next thing that came. */
	return 1;
}

/*
 * Ends a stream: closing the connection makes audiod destroy the stream.
 */
void
kl_backend_audio_stream_close(
	struct kl_backend_audio_stream *stream)
{
	struct kl_backend_audio_stream_report *report;
	unsigned index;

	/* Nothing to close. */
	if (stream == NULL)
		return;

	/* The connection and a descriptor come with no message yet. */
	if (stream->socket >= 0)
		(void)close(stream->socket);
	if (stream->pending_fd >= 0)
		(void)close(stream->pending_fd);

	/* A ring handed over in a report nobody took. */
	for (index = 0U; index < stream->report_count; index++) {
		report = &stream->reports[(stream->report_first + index) % STREAM_REPORTS];
		if (report->what == KL_BACKEND_AUDIO_READY && report->fd >= 0)
			(void)close(report->fd);
	}

	/* The record. */
	free(stream);
}

/*
 * Lets ended pump threads go: zedBSD's streams have none.
 */
void
kl_backend_audio_stream_reap(
	void)
{
	/* Succeeded: nothing to let go. */
	return;
}

/*
 * Waits for every pump thread to end: zedBSD's streams have none.
 */
void
kl_backend_audio_stream_reap_all(
	void)
{
	/* Succeeded: nothing to wait for. */
	return;
}

/* Moves the stream on: the connection made, then what audiod has sent. */
static void
stream_step(
	struct kl_backend_audio_stream *stream)
{
	struct kl_backend_audio_stream_report report;
	struct pollfd descriptor;
	socklen_t length;
	int error;
	int status;

	/* An ended stream has nothing more. */
	if (stream->stage == STREAM_ENDED)
		return;

	/* A connection under way: done once the socket can be written. */
	if (stream->stage == STREAM_CONNECTING) {
		descriptor.fd = stream->socket;
		descriptor.events = POLLOUT;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, 0);
		if (status <= 0)
			return;

		/* Whether the connection was made. */
		error = 0;
		length = sizeof(error);
		status = getsockopt(stream->socket, SOL_SOCKET, SO_ERROR, &error, &length);
		if (status != 0 || error != 0) {
			stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_UNAVAILABLE);
			return;
		}

		/* Connected: the greeting goes. */
		stream_connected(stream);
		if (stream->stage == STREAM_ENDED)
			return;
	}

	/* What audiod has sent. */
	stream_read(stream);
	if (stream->stage == STREAM_ENDED)
		return;

	/* The underruns counted since the last report, as one report of the total. */
	if (stream->underruns != stream->underruns_told) {
		stream->underruns_told = stream->underruns;
		memset(&report, 0, sizeof(report));
		report.what = KL_BACKEND_AUDIO_UNDERRUN;
		report.count = stream->underruns;
		report.fd = -1;
		stream_report(stream, &report);
	}
}

/* Says HELLO on a connection just made; WELCOME is awaited. */
static void
stream_connected(
	struct kl_backend_audio_stream *stream)
{
	struct audiod_hello hello;
	int error;

	/* HELLO: audiod answers WELCOME with whether there is a device. */
	memset(&hello, 0, sizeof(hello));
	hello.header.type = AUDIOD_HELLO;
	hello.header.length = sizeof(hello);
	hello.header.serial = stream->next_serial++;
	hello.version = AUDIOD_VERSION;
	error = stream_send(stream, &hello, sizeof(hello));
	if (error != 0)
		return;

	/* Succeeded: the greeting is with audiod. */
	stream->stage = STREAM_GREETING;
}

/* Reads the bytes that have come and handles each whole message. */
static void
stream_read(
	struct kl_backend_audio_stream *stream)
{
	struct audiod_header header;
	int status;

	/* Until nothing more is waiting. */
	for (;;) {
		/* Bytes, as many as there is room for; 0 is nothing more waiting, -1 the stream ended. */
		status = stream_receive(stream);
		if (status <= 0)
			return;

		/* Each whole message kept. */
		while (stream->pending_used >= sizeof(header)) {
			/* A header that cannot frame one of audiod's messages is a broken connection. */
			memcpy(&header, stream->pending, sizeof(header));
			if (header.length < sizeof(header) || header.length > AUDIOD_MESSAGE_MAX) {
				stream_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
				return;
			}

			/* A message not whole yet waits for more bytes. */
			if (stream->pending_used < header.length)
				break;

			/* The message, then the bytes after it. */
			stream_message(stream, stream->pending, header.length);
			if (stream->stage == STREAM_ENDED)
				return;
			stream->pending_used -= header.length;
			memmove(stream->pending, stream->pending + header.length, stream->pending_used);
		}
	}
}

/* Receives bytes and a descriptor that comes with them: 1 bytes came, 0 none waiting, -1 the stream ended. */
static int
stream_receive(
	struct kl_backend_audio_stream *stream)
{
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *item;
	struct msghdr message;
	size_t least;
	struct iovec vector;
	ssize_t count;
	int descriptor;

	/* Room after the bytes kept; none left is a peer sending nonsense. */
	if (stream->pending_used == sizeof(stream->pending)) {
		stream_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
		return -1;
	}

	/* The bytes and the room for one descriptor. */
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = stream->pending + stream->pending_used;
	vector.iov_len = sizeof(stream->pending) - stream->pending_used;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	count = recvmsg(stream->socket, &message, MSG_CMSG_CLOEXEC);
	if (count < 0 &&
	    (errno == EAGAIN ||
	     errno == EWOULDBLOCK))
		return 0;

	/* A closed or failed connection is audiod gone (or full before the greeting). */
	if (count <= 0) {
		if (stream->stage == STREAM_READY) {
			stream_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
		} else {
			stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_UNAVAILABLE);
		}

		/* The stream ended. */
		return -1;
	}

	/* A descriptor that came: kept for STREAM_CREATED; a second one is not wanted. */
	for (item = CMSG_FIRSTHDR(&message); item != NULL; item = CMSG_NXTHDR(&message, item)) {
		if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS)
			continue;
		least = CMSG_LEN(sizeof(int));
		if (item->cmsg_len < least)
			continue;
		memcpy(&descriptor, CMSG_DATA(item), sizeof(descriptor));
		if (stream->pending_fd >= 0) {
			(void)close(descriptor);
		} else {
			stream->pending_fd = descriptor;
		}
	}

	/* Kept after what came before. */
	stream->pending_used += (size_t)count;

	/* Succeeded: bytes came. */
	return 1;
}

/* Handles one message from audiod. */
static void
stream_message(
	struct kl_backend_audio_stream *stream,
	const unsigned char *bytes,
	uint32_t length)
{
	struct kl_backend_audio_stream_report report;
	struct audiod_stream_create create;
	struct audiod_stream_created created;
	struct audiod_welcome welcome;
	struct audiod_result result;
	struct audiod_event event;
	struct audiod_header header;
	int error;

	/* Its type. */
	memcpy(&header, bytes, sizeof(header));

	/* WELCOME: with a device, the stream is asked for; without one, there is no sound. */
	if (header.type == AUDIOD_WELCOME && stream->stage == STREAM_GREETING) {
		if (length < sizeof(welcome)) {
			stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_FAILED);
			return;
		}

		/* The greeting's words. */
		memcpy(&welcome, bytes, sizeof(welcome));

		/* No sound device. */
		if (welcome.device == 0U) {
			stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_NO_DEVICE);
			return;
		}

		/* The playback stream, as asked for. */
		memset(&create, 0, sizeof(create));
		create.header.type = AUDIOD_STREAM_CREATE;
		create.header.length = sizeof(create);
		create.header.serial = stream->next_serial++;
		create.header.stream = STREAM_NUMBER;
		create.direction = AUDIOD_PLAYBACK;
		create.format = stream->format.format;
		create.channels = stream->format.channels;
		create.rate = stream->format.rate;
		create.buffer_frames = stream->format.buffer_frames;
		create.period_frames = stream->format.period_frames;
		error = stream_send(stream, &create, sizeof(create));
		if (error != 0)
			return;
		stream->stage = STREAM_CREATING;
		return;
	}

	/* STREAM_CREATED: the ring's memory came with it; it is the client's from now. */
	if (header.type == AUDIOD_STREAM_CREATED && stream->stage == STREAM_CREATING) {
		if (length < sizeof(created) || stream->pending_fd < 0) {
			stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_FAILED);
			return;
		}

		/* The answer's words. */
		memcpy(&created, bytes, sizeof(created));

		/* The report hands the descriptor over. */
		memset(&report, 0, sizeof(report));
		report.what = KL_BACKEND_AUDIO_READY;
		report.error = KL_BACKEND_AUDIO_ERROR_NONE;
		report.fd = stream->pending_fd;
		report.bytes = created.shm_bytes;
		report.capacity_frames = created.capacity_frames;
		report.period_frames = stream->format.period_frames;
		stream->pending_fd = -1;
		stream->stage = STREAM_READY;
		stream_report(stream, &report);
		return;
	}

	/* ERROR before the stream is made: audiod refused the greeting or what was asked for. */
	if (header.type == AUDIOD_ERROR && (stream->stage == STREAM_GREETING || stream->stage == STREAM_CREATING)) {
		memset(&result, 0, sizeof(result));
		if (length >= sizeof(result))
			memcpy(&result, bytes, sizeof(result));
		stream_end(stream, KL_BACKEND_AUDIO_FAILED, stream_error(result.error));
		return;
	}

	/* DONE and ERROR answer a control. */
	if (header.type == AUDIOD_DONE || header.type == AUDIOD_ERROR) {
		memset(&result, 0, sizeof(result));
		if (length >= sizeof(result))
			memcpy(&result, bytes, sizeof(result));

		/* A drain audiod refused: its DRAINED will never come, so the stream is given up. */
		if (header.type == AUDIOD_ERROR && stream->draining && header.serial == stream->drain_serial) {
			stream->draining = 0U;
			stream_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_FAILED);
			return;
		}

		/* Which error, none for DONE. */
		error = KL_BACKEND_AUDIO_ERROR_NONE;
		if (header.type == AUDIOD_ERROR)
			error = (int)stream_error(result.error);
		stream_answer(stream, header.serial, (unsigned)error);
		return;
	}

	/* DRAINED: the drain under way is complete (an earlier drain's is passed over). */
	if (header.type == AUDIOD_DRAINED) {
		if (!stream->draining || header.serial != stream->drain_serial)
			return;
		stream->draining = 0U;
		memset(&report, 0, sizeof(report));
		report.what = KL_BACKEND_AUDIO_DRAINED;
		report.request = stream->drain_request;
		report.fd = -1;
		stream_report(stream, &report);
		return;
	}

	/* UNDERRUN: the total, reported once a step. */
	if (header.type == AUDIOD_UNDERRUN && length >= sizeof(event)) {
		memcpy(&event, bytes, sizeof(event));
		stream->underruns = event.count;
		return;
	}

	/* Succeeded: the message (REQUEST, OVERRUN, VOLUME_CHANGED or another) is passed over. */
	return;
}

/* Takes audiod's answer to a control by the serial it was sent with; the last answer of a control reports it. */
static void
stream_answer(
	struct kl_backend_audio_stream *stream,
	uint32_t serial,
	unsigned error)
{
	struct kl_backend_audio_stream_report report;
	struct stream_waiting *waiting;
	unsigned slot;
	unsigned index;

	/* The control the serial names. */
	waiting = NULL;
	for (slot = 0U; slot < STREAM_WAITING && waiting == NULL; slot++) {
		if (!stream->waiting[slot].used)
			continue;
		for (index = 0U; index < stream->waiting[slot].remaining; index++) {
			if (stream->waiting[slot].serials[index] == serial)
				waiting = &stream->waiting[slot];
		}
	}

	/* An answer to nothing awaited (HELLO's own, or a stray one) is passed over. */
	if (waiting == NULL)
		return;

	/* The first error among the answers is the control's. */
	if (waiting->error == KL_BACKEND_AUDIO_ERROR_NONE)
		waiting->error = error;

	/* One answer fewer to come (the serials still awaited stay at the front). */
	waiting->remaining--;
	if (waiting->serials[0] == serial)
		waiting->serials[0] = waiting->serials[1];
	if (waiting->remaining != 0U)
		return;

	/* The last answer: the slot is free again, and the request is answered. */
	waiting->used = 0U;
	memset(&report, 0, sizeof(report));
	report.what = KL_BACKEND_AUDIO_RESULT;
	report.error = waiting->error;
	report.request = waiting->request;
	report.fd = -1;
	stream_report(stream, &report);
}

/* Sends DRAIN; the drain under way is this one from now (an earlier one's DRAINED no longer counts). Returns 0 or an errno value. */
static int
stream_drain(
	struct kl_backend_audio_stream *stream,
	uint32_t request)
{
	struct audiod_header message;
	int error;

	/* The request, under the next serial. */
	memset(&message, 0, sizeof(message));
	message.type = AUDIOD_STREAM_DRAIN;
	message.length = sizeof(message);
	message.serial = stream->next_serial++;
	message.stream = STREAM_NUMBER;
	error = stream_send(stream, &message, sizeof(message));
	if (error != 0)
		return error;

	/* DRAINED comes with this serial once everything has been heard. */
	stream->draining = 1U;
	stream->drain_serial = message.serial;
	stream->drain_request = request;

	/* Succeeded: the drain is with audiod. */
	return 0;
}

/* Sends one of a control's requests and awaits its answer in the slot. Returns 0 or an errno value. */
static int
stream_request(
	struct kl_backend_audio_stream *stream,
	uint32_t type,
	struct stream_waiting *waiting)
{
	struct audiod_header message;
	int error;

	/* The request, under the next serial. */
	memset(&message, 0, sizeof(message));
	message.type = type;
	message.length = sizeof(message);
	message.serial = stream->next_serial++;
	message.stream = STREAM_NUMBER;
	error = stream_send(stream, &message, sizeof(message));
	if (error != 0)
		return error;

	/* Its answer is awaited. */
	waiting->serials[waiting->remaining] = message.serial;
	waiting->remaining++;

	/* Succeeded: the request is with audiod. */
	return 0;
}

/* Gives audiod's request for a control (START, STOP or FLUSH). */
static uint32_t
stream_type(
	unsigned what)
{
	/* Which request. */
	switch (what) {
	case KL_BACKEND_AUDIO_START:
		return AUDIOD_STREAM_START;
	case KL_BACKEND_AUDIO_STOP:
		return AUDIOD_STREAM_STOP;
	default:
		break;
	}

	/* Succeeded: the flush. */
	return AUDIOD_STREAM_FLUSH;
}

/* Writes one whole message; a connection that cannot take it ends the stream. Returns 0 or an errno value. */
static int
stream_send(
	struct kl_backend_audio_stream *stream,
	const void *message,
	uint32_t length)
{
	ssize_t sent;

	/* Sends the small message whole without raising SIGPIPE. */
	sent = send(stream->socket, message, length, MSG_NOSIGNAL);
	if (sent == (ssize_t)length)
		return 0;

	/* A full queue: the control can be asked again; nothing of it was sent. */
	if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && stream->stage == STREAM_READY)
		return EAGAIN;

	/* A connection that took part of a message, or failed, is gone. */
	if (stream->stage == STREAM_READY) {
		stream_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
	} else {
		stream_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_UNAVAILABLE);
	}

	/* The stream ended. */
	return ENOTCONN;
}

/* Ends the stream with FAILED or LOST: the connection closes and nothing more comes. */
static void
stream_end(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	unsigned error)
{
	struct kl_backend_audio_stream_report report;

	/* The connection; the stream is gone in audiod with it. */
	if (stream->socket >= 0)
		(void)close(stream->socket);
	stream->socket = -1;
	stream->stage = STREAM_ENDED;

	/* The last report. */
	memset(&report, 0, sizeof(report));
	report.what = what;
	report.error = error;
	report.fd = -1;
	stream_report(stream, &report);
}

/* Keeps a report for kl_backend_audio_stream_next. */
static void
stream_report(
	struct kl_backend_audio_stream *stream,
	const struct kl_backend_audio_stream_report *report)
{
	unsigned slot;

	/*
	 * A full ring cannot happen (it holds more than the answers that can
	 * wait, one drain, one count and the last report); were it full, the
	 * report would be dropped and its descriptor closed.
	 */
	if (stream->report_count == STREAM_REPORTS) {
		if (report->fd >= 0)
			(void)close(report->fd);
		return;
	}

	/* After the reports kept. */
	slot = (stream->report_first + stream->report_count) % STREAM_REPORTS;
	stream->reports[slot] = *report;
	stream->report_count++;
}

/* Gives the backend's error for an errno value audiod answered with. */
static unsigned
stream_error(
	uint32_t error)
{
	/* What audiod could not do with what was asked for. */
	if (error == EINVAL || error == ENOENT)
		return KL_BACKEND_AUDIO_ERROR_INVALID;

	/* Out of memory. */
	if (error == ENOMEM)
		return KL_BACKEND_AUDIO_ERROR_NO_MEMORY;


	/* Anything else. */
	return KL_BACKEND_AUDIO_ERROR_FAILED;
}
